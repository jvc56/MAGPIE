#!/usr/bin/env python3
"""Analyze portable PEG completion work separately from local timing."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import pathlib
import statistics
from typing import Any

from run_peg_completion_tail import (
    DEEP_LABEL,
    DEEP_SCHEDULE,
    FIRST2_LABEL,
    FIRST2_SCHEDULE,
    LEGACY_LABEL,
    arm_is_complete,
)
from run_peg_time_calibration import load_records, read_panel


QUANTILES = (0.5, 0.9, 0.95, 0.99, 1.0)


def quantile(values: list[float], probability: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, math.ceil(probability * len(ordered)) - 1)]


def summarize(values: list[float]) -> dict[str, float | int]:
    return {
        "n": len(values),
        **{
            ("max" if probability == 1 else f"p{int(probability * 100)}"):
            quantile(values, probability)
            for probability in QUANTILES
        },
    }


def events_for(
    records: list[dict[str, Any]], sequence: int, stage: int
) -> list[dict[str, Any]]:
    return sorted(
        (
            record
            for record in records
            if record.get("kind") == "event"
            and int(record.get("sequence", -1)) == sequence
            and int(record.get("stage", -1)) == stage
        ),
        key=lambda record: float(record["completed_seconds"]),
    )


def work_delta(
    start: dict[str, Any] | None,
    end: dict[str, Any],
    candidates: int,
) -> dict[str, float | int]:
    start_scenarios = int(start["cumulative_scenarios"]) if start else 0
    start_nodes = int(start["nested_endgame_nodes"]) if start else 0
    start_wall = float(start["completed_seconds"]) if start else 0.0
    start_cpu = float(start["process_cpu_seconds"]) if start else 0.0
    wall = float(end["completed_seconds"]) - start_wall
    cpu = float(end["process_cpu_seconds"]) - start_cpu
    return {
        "scenarios": int(end["cumulative_scenarios"]) - start_scenarios,
        "nested_nodes": int(end["nested_endgame_nodes"]) - start_nodes,
        "candidates": candidates,
        "wall_seconds": wall,
        "process_cpu_seconds": cpu,
        "scheduled_core_occupancy": cpu / (wall * 18.0) if wall > 0 else 0.0,
    }


def trace_observations(
    records: list[dict[str, Any]],
    position: dict[str, Any],
    arm: dict[str, Any],
    trace: str,
    event_index: dict[tuple[int, int], list[dict[str, Any]]] | None = None,
) -> list[dict[str, Any]]:
    sequence = int(arm["sequence"])
    if event_index is None:
        seed_events = events_for(records, sequence, 0)
        stage1 = events_for(records, sequence, 1)
    else:
        seed_events = event_index.get((sequence, 0), [])
        stage1 = event_index.get((sequence, 1), [])
    if not seed_events or not stage1:
        return []
    seed = seed_events[-1]
    common = {
        "position": position["position"],
        "bag": int(position["bag"]),
        "root_candidate_total": int(arm["root_candidate_total"]),
        "seed_scenarios": int(seed["cumulative_scenarios"]),
        "arm_sequence": sequence,
    }
    observations = [
        {
            **common,
            "stage": f"{trace}:greedy_seed",
            **work_delta(None, seed, int(arm["root_candidate_total"])),
        }
    ]
    if trace == "first2":
        boundary = stage1[-1]
        observations.append(
            {
                **common,
                "stage": "first2:2ply_wave",
                "pre_stage_scenarios": int(seed["cumulative_scenarios"]),
                "pre_stage_nested_nodes": int(
                    seed["nested_endgame_nodes"]
                ),
                **work_delta(seed, boundary, 2),
            }
        )
        return observations

    if len(stage1) != 16:
        return []
    boundary16 = stage1[-1]
    observations.append(
        {
            **common,
            "stage": "deep:2ply_to_16",
            **work_delta(seed, boundary16, 16),
        }
    )
    stage2 = (
        event_index.get((sequence, 2), [])
        if event_index is not None
        else events_for(records, sequence, 2)
    )
    if len(stage2) != 2:
        return []
    observations.append(
        {
            **common,
            "stage": "deep:3ply_wave_2",
            "pre_stage_scenarios": int(
                boundary16["cumulative_scenarios"]
            ),
            "pre_stage_nested_nodes": int(
                boundary16["nested_endgame_nodes"]
            ),
            **work_delta(boundary16, stage2[-1], 2),
        }
    )
    return observations


def solve_linear(matrix: list[list[float]], vector: list[float]) -> list[float]:
    augmented = [
        [*row, value] for row, value in zip(matrix, vector, strict=True)
    ]
    size = len(vector)
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        if abs(augmented[column][column]) < 1.0e-12:
            return [0.0] * size
        divisor = augmented[column][column]
        augmented[column] = [value / divisor for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                value - factor * pivot_value
                for value, pivot_value in zip(
                    augmented[row], augmented[column], strict=True
                )
            ]
    return [augmented[row][-1] for row in range(size)]


def ols(features: list[list[float]], outcomes: list[float]) -> list[float]:
    width = len(features[0])
    gram = [
        [
            sum(row[left] * row[right] for row in features)
            for right in range(width)
        ]
        for left in range(width)
    ]
    for index in range(width):
        gram[index][index] += 1.0e-9
    product = [
        sum(row[column] * outcome for row, outcome in zip(features, outcomes, strict=True))
        for column in range(width)
    ]
    return solve_linear(gram, product)


def dot(left: list[float], right: list[float]) -> float:
    return sum(a * b for a, b in zip(left, right, strict=True))


def admission_features(observation: dict[str, Any]) -> list[float]:
    return [
        1.0,
        math.log1p(float(observation["root_candidate_total"])),
        math.log1p(float(observation["pre_stage_scenarios"])),
        math.log1p(float(observation["pre_stage_nested_nodes"])),
    ]


def conformal_audit(
    admissions: list[dict[str, Any]],
) -> dict[str, Any] | None:
    if len(admissions) != 320:
        return None
    ordered = sorted(admissions, key=lambda row: str(row["position"]))
    training, calibration, heldout = ordered[:128], ordered[128:256], ordered[256:]
    training_x = [admission_features(row) for row in training]
    scenario_coef = ols(
        training_x,
        [math.log1p(float(row["scenarios"])) for row in training],
    )
    node_coef = ols(
        training_x,
        [math.log1p(float(row["nested_nodes"])) for row in training],
    )
    residuals = []
    for row in calibration:
        features = admission_features(row)
        residuals.append(
            max(
                math.log1p(float(row["scenarios"])) - dot(scenario_coef, features),
                math.log1p(float(row["nested_nodes"])) - dot(node_coef, features),
            )
        )
    rank = math.ceil((len(calibration) + 1) * 0.99)
    conformal_quantile = sorted(residuals)[min(rank, len(residuals)) - 1]
    flat_scenarios = max(int(row["scenarios"]) for row in training + calibration)
    flat_nodes = max(int(row["nested_nodes"]) for row in training + calibration)
    flat_failures = 0
    conditioned_failures = 0
    predicted_scenarios = []
    predicted_nodes = []
    for row in heldout:
        features = admission_features(row)
        scenario_bound = max(
            0, math.ceil(math.exp(dot(scenario_coef, features) + conformal_quantile) - 1)
        )
        node_bound = max(
            0, math.ceil(math.exp(dot(node_coef, features) + conformal_quantile) - 1)
        )
        predicted_scenarios.append(scenario_bound)
        predicted_nodes.append(node_bound)
        flat_failures += int(
            int(row["scenarios"]) > flat_scenarios
            or int(row["nested_nodes"]) > flat_nodes
        )
        conditioned_failures += int(
            int(row["scenarios"]) > scenario_bound
            or int(row["nested_nodes"]) > node_bound
        )
    return {
        "split": {"training": 128, "calibration": 128, "heldout": 64},
        "target_miscoverage": 0.01,
        "conformal_rank": rank,
        "conformal_residual_quantile": conformal_quantile,
        "features": [
            "log1p(root_candidate_total)",
            "log1p(pre_stage_scenarios)",
            "log1p(pre_stage_nested_nodes)",
        ],
        "timing_feature_used": False,
        "flat_bound": {
            "scenarios": flat_scenarios,
            "nested_nodes": flat_nodes,
            "candidate_overhead": 2,
            "heldout_false_starts": flat_failures,
            "heldout_n": len(heldout),
        },
        "feature_conditioned_bound": {
            "scenario_bound": summarize([float(value) for value in predicted_scenarios]),
            "nested_node_bound": summarize([float(value) for value in predicted_nodes]),
            "candidate_overhead": 2,
            "heldout_false_starts": conditioned_failures,
            "heldout_n": len(heldout),
            "coverage_scope": "bag-marginal split-conformal, not conditional",
        },
    }


def sentinel_diagnostics(records: list[dict[str, Any]]) -> dict[str, Any]:
    sentinels = [
        record
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "sentinel"
        and record.get("status") == "completed"
    ]
    if not sentinels:
        return {"n": 0}
    throughputs = [
        float(record["nested_endgame_nodes"]) / float(record["wall_seconds"])
        for record in sentinels
    ]
    median = statistics.median(throughputs)
    normalized = [value / median for value in throughputs]
    cv = (
        statistics.stdev(normalized) / statistics.mean(normalized)
        if len(normalized) > 1
        else 0.0
    )
    third = max(1, len(normalized) // 3)
    early = statistics.mean(normalized[:third])
    late = statistics.mean(normalized[-third:])
    deviations = [abs(value - 1.0) for value in normalized]
    mad = statistics.median(deviations)
    noisy_threshold = max(0.20, 3.0 * mad)
    noisy = [
        str(record["label"])
        for record, value in zip(sentinels, normalized, strict=True)
        if abs(value - 1.0) > noisy_threshold
    ]
    occupancies = [
        float(record["scheduled_core_occupancy"]) for record in sentinels
    ]
    return {
        "n": len(sentinels),
        "node_throughput_per_wall_second": summarize(throughputs),
        "normalized_throughput_cv": cv,
        "early_normalized_mean": early,
        "late_normalized_mean": late,
        "early_to_late_drift_fraction": late / early - 1.0,
        "scheduled_core_occupancy": summarize(occupancies),
        "noisy_threshold_fraction_from_median": noisy_threshold,
        "noisy_segments": noisy,
    }


def local_wall_model(observations: list[dict[str, Any]]) -> dict[str, Any]:
    if len(observations) < 4:
        return {"n": len(observations)}
    features = [
        [
            1.0,
            float(row["scenarios"]) / 1.0e6,
            float(row["nested_nodes"]) / 1.0e6,
            float(row["candidates"]),
        ]
        for row in observations
    ]
    outcomes = [float(row["wall_seconds"]) for row in observations]
    coefficients = ols(features, outcomes)
    predictions = [dot(coefficients, row) for row in features]
    mean = statistics.mean(outcomes)
    total = sum((value - mean) ** 2 for value in outcomes)
    residual = sum(
        (value - prediction) ** 2
        for value, prediction in zip(outcomes, predictions, strict=True)
    )
    return {
        "n": len(observations),
        "formula": "wall_s = intercept + scenario_millions*b_s + nested_node_millions*b_n + candidates*b_c",
        "coefficients": {
            "intercept": coefficients[0],
            "scenario_millions": coefficients[1],
            "nested_node_millions": coefficients[2],
            "candidates": coefficients[3],
        },
        "r_squared": 1.0 - residual / total if total > 0 else 0.0,
        "purpose": "local seconds conversion only; not a strength feature",
    }


def timing_accounting(
    records: list[dict[str, Any]],
    observations: list[dict[str, Any]],
    positions: list[dict[str, Any]],
) -> dict[str, Any]:
    invocations = [
        record for record in records if record.get("kind") == "invocation"
    ]
    processes = [record for record in records if record.get("kind") == "process"]
    process_sequences = {int(record["sequence"]) for record in processes}
    orphaned = [
        {
            "sequence": int(record["sequence"]),
            "position": str(record["position"]),
            "label": str(record["label"]),
        }
        for record in invocations
        if int(record["sequence"]) not in process_sequences
    ]
    run_span = 0.0
    if invocations and processes:
        started = min(
            dt.datetime.fromisoformat(str(record["started_utc"]))
            for record in invocations
        )
        finished = max(
            dt.datetime.fromisoformat(str(record["finished_utc"]))
            for record in processes
        )
        run_span = (finished - started).total_seconds()
    completed_wall = sum(
        float(record["wall_seconds"])
        for record in records
        if record.get("kind") == "arm"
        and record.get("status") == "completed"
    )

    trace_totals: dict[tuple[str, int, str], float] = {}
    for row in observations:
        trace = str(row["stage"]).split(":", 1)[0]
        key = (str(row["position"]), int(row["bag"]), trace)
        trace_totals[key] = trace_totals.get(key, 0.0) + float(
            row["wall_seconds"]
        )
    duration_estimates: dict[tuple[int, str], dict[str, float]] = {}
    for bag in range(1, 5):
        for trace in ("first2", "deep"):
            values = [
                value
                for (_, row_bag, row_trace), value in trace_totals.items()
                if row_bag == bag and row_trace == trace
            ]
            if values:
                duration_estimates[(bag, trace)] = {
                    "median": statistics.median(values),
                    "mean": statistics.mean(values),
                    "p90": quantile(values, 0.90),
                }
    projections = {"median": 0.0, "mean": 0.0, "p90": 0.0}
    for position in positions:
        bag = int(position["bag"])
        position_id = str(position["position"])
        if (position_id, bag, "first2") not in trace_totals:
            for estimate in projections:
                projections[estimate] += duration_estimates.get(
                    (bag, "first2"), {}
                ).get(estimate, 0.0)
        if bag in {2, 3} and (position_id, bag, "deep") not in trace_totals:
            for estimate in projections:
                projections[estimate] += duration_estimates.get(
                    (bag, "deep"), {}
                ).get(estimate, 0.0)
    return {
        "invocations": len(invocations),
        "process_records": len(processes),
        "orphaned_invocations": orphaned,
        "run_span_seconds": run_span,
        "completed_arm_wall_seconds": completed_wall,
        "projected_remaining_arm_wall_seconds": projections,
        "duration_estimates_by_bag_trace": {
            f"{bag}|{trace}": estimates
            for (bag, trace), estimates in duration_estimates.items()
        },
        "projection_excludes_future_sentinel_overhead": True,
    }


def format_number(value: float, digits: int = 2) -> str:
    if math.isnan(value):
        return "NA"
    if abs(value) >= 1000:
        return f"{value:,.0f}"
    return f"{value:.{digits}f}"


def make_markdown(summary: dict[str, Any]) -> str:
    accounting = summary["accounting"]
    lines = [
        "# PEG admission/completion-tail calibration",
        "",
        f"Status: **{accounting['complete_positions']}/{accounting['panel_positions']} "
        "positions complete**. This report is final only when the numerator is 1,280.",
        "",
        "The strength-portable boundary is the completed stage work vector "
        "`(scenarios, nested endgame nodes, candidate overhead)`. Wall time is "
        "reported separately as an M5 conversion and is not used as a strength feature.",
        "",
        "## Completion work",
        "",
        "| bag | stage | n | scenarios median / p90 / p95 / p99 / max | "
        "nested nodes median / p90 / p95 / p99 / max | "
        "wall s median / p90 / p95 / p99 / max | occupancy median |",
        "|---:|---|---:|---|---|---|---:|",
    ]
    for key, row in summary["quantiles"].items():
        bag, stage = key.split("|", 1)
        scenario = row["scenarios"]
        nodes = row["nested_nodes"]
        wall = row["wall_seconds"]
        occupancy = row["scheduled_core_occupancy"]
        scenario_text = " / ".join(
            format_number(float(scenario[name]), 0)
            for name in ("p50", "p90", "p95", "p99", "max")
        )
        node_text = " / ".join(
            format_number(float(nodes[name]), 0)
            for name in ("p50", "p90", "p95", "p99", "max")
        )
        wall_text = " / ".join(
            format_number(float(wall[name]), 2)
            for name in ("p50", "p90", "p95", "p99", "max")
        )
        lines.append(
            f"| {bag} | {stage} | {row['n']} | {scenario_text} | "
            f"{node_text} | {wall_text} | {occupancy['p50']:.3f} |"
        )
    lines.extend(["", "## Admission-bound audit", ""])
    if not summary["conformal"]:
        lines.append(
            "Unavailable until all 320 positions per bag complete; a p90 or "
            "an interim maximum is not treated as a deadline-safe bound."
        )
    else:
        lines.append(
            "Each bag uses 128 training, 128 conformal-calibration, and 64 "
            "prospectively ordered held-out positions. The 99% conformal rank "
            "is the maximum calibration residual (rank 128/128)."
        )
        lines.extend(
            [
                "",
                "| bag / stage | flat vector bound (scenarios; nodes; candidates) | "
                "flat false starts | conditioned false starts | "
                "conditioned median bound (scenarios; nodes; candidates) |",
                "|---:|---|---:|---:|---|",
            ]
        )
        for key, audit in summary["conformal"].items():
            bag, stage = key.split("|", 1)
            flat = audit["flat_bound"]
            conditioned = audit["feature_conditioned_bound"]
            lines.append(
                f"| {bag} {stage} | {flat['scenarios']:,}; "
                f"{flat['nested_nodes']:,}; 2 | "
                f"{flat['heldout_false_starts']}/64 | "
                f"{conditioned['heldout_false_starts']}/64 | "
                f"{conditioned['scenario_bound']['p50']:,.0f}; "
                f"{conditioned['nested_node_bound']['p50']:,.0f}; 2 |"
            )
    sentinel = summary["sentinels"]
    lines.extend(["", "## Load diagnostics", ""])
    if sentinel.get("n", 0):
        lines.append(
            f"{sentinel['n']} identical sentinels: normalized throughput CV "
            f"{sentinel['normalized_throughput_cv']:.3f}; early-to-late drift "
            f"{sentinel['early_to_late_drift_fraction']:+.1%}; median "
            f"scheduled-core occupancy "
            f"{sentinel['scheduled_core_occupancy']['p50']:.3f}. "
            f"Noisy segments: {sentinel['noisy_segments'] or 'none'}."
        )
    else:
        lines.append("No completed sentinels.")
    lines.extend(
        [
            "",
            "Structurally complete work observations remain valid when a "
            "sentinel marks their local wall conversion as noisy.",
            "",
            "## Accounting",
            "",
            f"- Complete explicit two-candidate 2-ply traces: "
            f"{accounting['complete_first2_traces']}/1,280",
            f"- Complete 16-candidate 2-ply + two-candidate 3-ply traces: "
            f"{accounting['complete_deep_traces']}/640",
            f"- Censored attempts recorded: {accounting['censored_attempts']}",
            f"- Failed/interrupted process records: "
            f"{accounting['failed_processes']}",
            f"- Invocation records without a process footer (active or "
            f"interrupted): {len(accounting['timing']['orphaned_invocations'])}",
            f"- Observed run span: "
            f"{accounting['timing']['run_span_seconds'] / 3600:.2f} h; "
            f"projected remaining arm wall time at current per-bag means: "
            f"{accounting['timing']['projected_remaining_arm_wall_seconds']['mean'] / 3600:.2f} h "
            f"(median/p90 scenario: "
            f"{accounting['timing']['projected_remaining_arm_wall_seconds']['median'] / 3600:.2f}/"
            f"{accounting['timing']['projected_remaining_arm_wall_seconds']['p90'] / 3600:.2f} h; "
            "future sentinel overhead excluded)",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--panel",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/admission_positions_20260730.tsv"
        ),
    )
    parser.add_argument(
        "--records",
        type=pathlib.Path,
        default=pathlib.Path(
            "obj/peg_completion_tail_20260730/records.jsonl"
        ),
    )
    parser.add_argument("--json", type=pathlib.Path, required=True)
    parser.add_argument("--markdown", type=pathlib.Path, required=True)
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = args.panel if args.panel.is_absolute() else repo / args.panel
    records_path = (
        args.records if args.records.is_absolute() else repo / args.records
    )
    positions = read_panel(panel)
    records = load_records(records_path)
    arm_index: dict[tuple[str, str], dict[str, Any]] = {}
    event_index: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for record in records:
        if record.get("kind") == "arm" and record.get("mode") == "arm":
            arm_index[(str(record["position"]), str(record["label"]))] = record
        elif record.get("kind") == "event":
            event_index.setdefault(
                (int(record["sequence"]), int(record["stage"])), []
            ).append(record)
    for events in event_index.values():
        events.sort(key=lambda record: float(record["completed_seconds"]))

    def indexed_completed_arm(
        position: dict[str, Any], label: str, schedule: list[int]
    ) -> dict[str, Any] | None:
        key = (str(position["position"]), label)
        arm = arm_index.get(key)
        if arm_is_complete(arm, schedule):
            return arm
        legacy = arm_index.get(
            (str(position["position"]), LEGACY_LABEL)
        )
        if arm_is_complete(legacy, schedule):
            return legacy
        return arm

    observations = []
    complete_first2 = 0
    complete_deep = 0
    for position in positions:
        first2 = indexed_completed_arm(
            position, FIRST2_LABEL, FIRST2_SCHEDULE
        )
        if arm_is_complete(first2, FIRST2_SCHEDULE):
            trace = trace_observations(
                records, position, first2, "first2", event_index
            )
            if len(trace) == 2:
                complete_first2 += 1
                observations.extend(trace)
        if int(position["bag"]) in {2, 3}:
            deep = indexed_completed_arm(
                position, DEEP_LABEL, DEEP_SCHEDULE
            )
            if arm_is_complete(deep, DEEP_SCHEDULE):
                trace = trace_observations(
                    records, position, deep, "deep", event_index
                )
                if len(trace) == 3:
                    complete_deep += 1
                    observations.extend(trace)

    quantiles = {}
    for bag in range(1, 5):
        stages = sorted(
            {
                str(row["stage"])
                for row in observations
                if int(row["bag"]) == bag
            }
        )
        for stage in stages:
            subset = [
                row
                for row in observations
                if int(row["bag"]) == bag and row["stage"] == stage
            ]
            quantiles[f"{bag}|{stage}"] = {
                "n": len(subset),
                **{
                    metric: summarize(
                        [float(row[metric]) for row in subset]
                    )
                    for metric in (
                        "scenarios",
                        "nested_nodes",
                        "wall_seconds",
                        "process_cpu_seconds",
                        "scheduled_core_occupancy",
                        "root_candidate_total",
                    )
                },
            }
    conformal = {}
    for stage, bags in (
        ("first2:2ply_wave", range(1, 5)),
        ("deep:3ply_wave_2", (2, 3)),
    ):
        for bag in bags:
            audit = conformal_audit(
                [
                    row
                    for row in observations
                    if row["stage"] == stage
                    and int(row["bag"]) == bag
                ]
            )
            if audit is not None:
                conformal[f"{bag}|{stage}"] = audit
    summary = {
        "schema_version": 1,
        "accounting": {
            "panel_positions": len(positions),
            "complete_positions": sum(
                1
                for position in positions
                if arm_is_complete(
                    indexed_completed_arm(
                        position, FIRST2_LABEL, FIRST2_SCHEDULE
                    ),
                    FIRST2_SCHEDULE,
                )
                and (
                    int(position["bag"]) not in {2, 3}
                    or arm_is_complete(
                        indexed_completed_arm(
                            position, DEEP_LABEL, DEEP_SCHEDULE
                        ),
                        DEEP_SCHEDULE,
                    )
                )
            ),
            "complete_first2_traces": complete_first2,
            "complete_deep_traces": complete_deep,
            "censored_attempts": sum(
                record.get("kind") == "censored_attempt" for record in records
            ),
            "failed_processes": sum(
                record.get("kind") == "process"
                and int(record.get("return_code", 0)) != 0
                for record in records
            ),
            "timing": timing_accounting(records, observations, positions),
        },
        "quantile_definition": "nearest-rank empirical quantiles",
        "quantiles": quantiles,
        "conformal": conformal,
        "sentinels": sentinel_diagnostics(records),
        "local_wall_model": local_wall_model(observations),
    }
    json_text = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    markdown_text = make_markdown(summary)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json_text)
    args.markdown.write_text(markdown_text)
    print(
        f"analyzed {summary['accounting']['complete_positions']}/"
        f"{len(positions)} positions",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

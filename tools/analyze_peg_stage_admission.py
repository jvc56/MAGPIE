#!/usr/bin/env python3
"""Audit the frozen PEG whole-stage admission rule on append-only traces."""

from __future__ import annotations

import argparse
import collections
import json
import math
import pathlib
from typing import Any


def load_jsonls(paths: list[pathlib.Path]) -> list[dict[str, Any]]:
    """Load independently resumable runs without colliding sequence ids."""
    combined: list[dict[str, Any]] = []
    sequence_offset = 0
    for path in paths:
        records = [
            json.loads(line) for line in path.read_text().splitlines() if line
        ]
        sequence_ids = [
            int(record["sequence"])
            for record in records
            if "sequence" in record
        ]
        for record in records:
            copied = dict(record)
            if "sequence" in copied:
                copied["sequence"] = int(copied["sequence"]) + sequence_offset
            combined.append(copied)
        if sequence_ids:
            sequence_offset += max(sequence_ids) + 1
    return combined


def scale_cost(
    cost: dict[str, float], scale: float, scenarios: int, nodes: int
) -> dict[str, float]:
    scaled = dict(cost)
    scaled["fixed"] *= scale
    if scenarios > 0:
        scaled["scenario"] *= scale
    if nodes > 0 or scale > 1.0:
        scaled["node"] *= scale
    return scaled


def estimate_seconds(cost: dict[str, float], scenarios: int, nodes: int) -> float:
    return cost["fixed"] + cost["scenario"] * scenarios + cost["node"] * nodes


def update_runtime_rate(
    rate: dict[str, float], workers: int, seconds: float, nodes: int
) -> None:
    if workers < 1 or seconds < 0.01 or nodes < 1:
        return
    observed = seconds / nodes * workers
    if not math.isfinite(observed) or observed <= 0.0:
        return
    if int(rate["observations"]) == 0:
        rate["expected"] = observed
        rate["deadline"] = observed
    else:
        rate["expected"] = 0.8 * rate["expected"] + 0.2 * observed
        rate["deadline"] = max(0.98 * rate["deadline"], observed)
    rate["observations"] += 1


def intended_stage_events(
    previous_stage_events: list[dict[str, Any]], candidates: int
) -> list[dict[str, Any]]:
    ranked = sorted(
        previous_stage_events,
        key=lambda event: -(
            float(event["win"]) + 1.0e-4 * float(event["spread"])
        ),
    )
    if len(ranked) < candidates:
        raise ValueError("previous stage did not finish enough candidates")
    return ranked[:candidates]


def replay_stages(
    records: list[dict[str, Any]],
    artifact: dict[str, Any],
    stage_indices: set[int] | None = None,
) -> list[dict[str, Any]]:
    by_sequence: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    for record in records:
        if "sequence" in record:
            by_sequence[int(record["sequence"])].append(record)

    reference = artifact["reference_cost"]
    fit = artifact["fit"]
    runtime_config = artifact.get("runtime_rate")
    runtime_rate = {"observations": 0, "expected": 0.0, "deadline": 0.0}
    rows: list[dict[str, Any]] = []
    for sequence, trace in sorted(by_sequence.items()):
        invocation = next(
            (record for record in trace if record.get("kind") == "invocation"),
            None,
        )
        arm = next(
            (record for record in reversed(trace) if record.get("kind") == "arm"),
            None,
        )
        if invocation is None or arm is None or invocation.get("mode") != "arm":
            continue
        schedule = invocation.get("schedule")
        if not isinstance(schedule, list) or not schedule:
            continue
        stages = {
            int(record["stage"]): record
            for record in trace
            if record.get("kind") == "stage"
        }
        events = collections.defaultdict(list)
        for record in trace:
            if record.get("kind") == "event":
                events[int(record["stage"])].append(record)
        bag = int(invocation["bag"])
        runtime_ready = runtime_config is None or int(
            runtime_rate["observations"]
        ) >= int(runtime_config["minimum_prior_observations_for_stage_1"])
        for stage_index, scheduled_candidates in enumerate(schedule, start=1):
            if stage_indices is not None and stage_index not in stage_indices:
                continue
            model_key = f"stage_{stage_index}"
            if model_key not in artifact:
                continue
            # A later stage is observable only if its shallower predecessor
            # completed and the runner reached this boundary. Missing data is
            # never manufactured into a successful admission observation.
            if (
                stage_index - 1 not in stages
                or stage_index not in stages
                or not events[stage_index - 1]
            ):
                continue
            stage_record = stages[stage_index]
            candidate_count = int(stage_record["candidate_total"])
            if candidate_count != int(scheduled_candidates):
                raise ValueError(
                    f"sequence {sequence} stage {stage_index}: "
                    "candidate count differs from frozen schedule"
                )
            selected_previous = intended_stage_events(
                events[stage_index - 1], candidate_count
            )
            stage_scenarios = sum(
                int(event["scenarios"]) for event in selected_previous
            )
            selected_previous_nodes = sum(
                int(event.get("nested_endgame_nodes", 0))
                for event in selected_previous
            )
            observed_previous_scenarios = sum(
                int(event["scenarios"]) for event in events[stage_index - 1]
            )
            observed_previous_nodes = sum(
                int(event.get("nested_endgame_nodes", 0))
                for event in events[stage_index - 1]
            )
            previous_record = stages[stage_index - 1]
            observed_previous_seconds = float(
                previous_record["end_seconds"]
            ) - float(previous_record["start_seconds"])

            stage_model = artifact[model_key][str(bag)]
            local_scale = float(reference["reference_workers"]) / float(
                invocation["threads"]
            )
            expected_base_cost = {
                "fixed": float(reference["fixed_seconds"]) * local_scale,
                "scenario": float(reference["seconds_per_scenario"])
                * local_scale,
                "node": float(reference["seconds_per_endgame_node"])
                * local_scale,
            }
            deadline_base_cost = {
                key: value * float(fit["deadline_slowdown"])
                for key, value in expected_base_cost.items()
            }
            if runtime_config is not None and runtime_ready:
                expected_base_cost["node"] = max(
                    expected_base_cost["node"],
                    runtime_rate["expected"] / float(invocation["threads"]),
                )
                deadline_base_cost["node"] = max(
                    deadline_base_cost["node"],
                    runtime_rate["deadline"]
                    / float(invocation["threads"])
                    * float(runtime_config["deadline_safety_multiplier"]),
                )
            modeled_previous_seconds = estimate_seconds(
                expected_base_cost,
                observed_previous_scenarios,
                observed_previous_nodes,
            )
            live_scale = 1.0
            if (
                observed_previous_seconds >= 0.01
                and modeled_previous_seconds > 0.0
            ):
                live_scale = max(
                    0.25,
                    min(
                        4.0,
                        observed_previous_seconds / modeled_previous_seconds,
                    ),
                )
            expected_cost = scale_cost(
                expected_base_cost,
                live_scale,
                observed_previous_scenarios,
                observed_previous_nodes,
            )
            if "warm_deadline_cost_multiplier" in stage_model:
                # V4 separates the large no-observation residual from the
                # smaller bag-conditioned residual that remains after a
                # portable hardware/load node rate exists. Apply either
                # residual to the entire cost model, matching C exactly.
                deadline_residual = float(
                    stage_model[
                        "warm_deadline_cost_multiplier"
                        if runtime_ready
                        else "deadline_cost_multiplier"
                    ]
                )
                deadline_cost = {
                    key: value * deadline_residual
                    for key, value in deadline_base_cost.items()
                }
            else:
                # Retain the v1-v3 behavior so the failed prospective panels
                # remain exactly reproducible with their frozen artifacts.
                deadline_cost = {
                    key: value
                    * (
                        1.0
                        if key == "node"
                        and runtime_ready
                        and not bool(
                            runtime_config is not None
                            and runtime_config.get(
                                "retain_deadline_residual_on_node", False
                            )
                        )
                        else float(stage_model["deadline_cost_multiplier"])
                    )
                    for key, value in deadline_base_cost.items()
                }
            # A node-free predecessor can establish only that movegen/scenario
            # work is fast. It cannot shrink an unseen exact-endgame rate.
            deadline_live_scale = (
                live_scale
                if observed_previous_nodes > 0
                else max(1.0, live_scale)
            )
            deadline_cost = scale_cost(
                deadline_cost,
                deadline_live_scale,
                observed_previous_scenarios,
                observed_previous_nodes,
            )
            cold_bound_nodes = (
                stage_scenarios
                * float(stage_model["completion_nodes_per_scenario"])
            )
            absolute_bound_nodes = float(
                stage_model.get("completion_absolute_nodes", 0.0)
            )
            if absolute_bound_nodes > 0.0:
                cold_bound_nodes = min(
                    cold_bound_nodes, absolute_bound_nodes
                )
            prior_bound_nodes = (
                selected_previous_nodes
                * float(
                    stage_model.get(
                        "completion_previous_node_multiplier", 0.0
                    )
                )
            )
            bound_nodes = math.ceil(
                max(cold_bound_nodes, prior_bound_nodes)
                * float(fit["completion_multiplier"])
            )
            bound_seconds = estimate_seconds(
                deadline_cost, stage_scenarios, bound_nodes
            )
            remaining_seconds = float(invocation["budget_seconds"]) - float(
                stage_record["start_seconds"]
            )
            eligible_for_gate = runtime_ready
            admitted = eligible_for_gate and bound_seconds <= remaining_seconds
            completed_candidates = int(stage_record["completed_candidates"])
            if len(events[stage_index]) != completed_candidates:
                raise ValueError(
                    f"sequence {sequence} stage {stage_index}: "
                    "event and completion counts differ"
                )
            completed = completed_candidates == candidate_count
            actual_stage_seconds = float(stage_record["end_seconds"]) - float(
                stage_record["start_seconds"]
            )
            actual_stage_scenarios = sum(
                int(event["scenarios"]) for event in events[stage_index]
            )
            actual_stage_nodes = sum(
                int(event.get("nested_endgame_nodes", 0))
                for event in events[stage_index]
            )
            # The observation runner deliberately supplies an ample physical
            # deadline so it can measure complete stages.  Passing that loose
            # deadline is not enough: production admits as soon as this
            # predicted bound fits, so a completed stage that took longer than
            # its own bound is a prospective false start at a tighter clock.
            completion_bound_covered_actual = (
                completed and actual_stage_seconds <= bound_seconds
            )
            rows.append(
                {
                    "sequence": sequence,
                    "position": str(invocation["position"]),
                    "bag": bag,
                    "stage_index": stage_index,
                    "fidelity_plies": stage_index + 1,
                    "candidate_count": candidate_count,
                    "stage_scenarios": stage_scenarios,
                    "selected_previous_stage_nodes": selected_previous_nodes,
                    "live_cost_scale": live_scale,
                    "runtime_rate_observations": int(
                        runtime_rate["observations"]
                    ),
                    "runtime_rate_ready": runtime_ready,
                    "eligible_for_gate": eligible_for_gate,
                    "completion_bound_nodes": bound_nodes,
                    "completion_bound_seconds": bound_seconds,
                    "remaining_seconds": remaining_seconds,
                    "actual_stage_seconds": actual_stage_seconds,
                    "actual_stage_scenarios": actual_stage_scenarios,
                    "actual_stage_nodes": actual_stage_nodes,
                    "completion_bound_slack_seconds": (
                        bound_seconds - actual_stage_seconds
                    ),
                    "completion_bound_covered_actual": (
                        completion_bound_covered_actual
                    ),
                    "completed": completed,
                    "admitted": admitted,
                    "false_start": (
                        admitted and not completion_bound_covered_actual
                    ),
                    "conservative_refusal": (not admitted) and completed,
                }
            )
        # Production updates the persistent rate after the solve, not between
        # depths in one invocation. Use the deepest observed non-greedy stage
        # with positive exact-endgame work. A later stage can legitimately
        # complete with zero nodes and must not erase a shallower usable rate;
        # even a legacy partial positive-node stage is valid throughput
        # evidence, although it is never treated as a completed ranking.
        observed_stage_indices = [
            stage_index
            for stage_index in range(1, len(schedule) + 1)
            if stage_index in stages
            and events[stage_index]
            and sum(
                int(event.get("nested_endgame_nodes", 0))
                for event in events[stage_index]
            )
            > 0
        ]
        if runtime_config is not None and observed_stage_indices:
            observed_stage = max(observed_stage_indices)
            observed_record = stages[observed_stage]
            update_runtime_rate(
                runtime_rate,
                int(invocation["threads"]),
                float(observed_record["end_seconds"])
                - float(observed_record["start_seconds"]),
                sum(
                    int(event.get("nested_endgame_nodes", 0))
                    for event in events[observed_stage]
                ),
            )
    return rows


def replay_stage_one(
    records: list[dict[str, Any]], artifact: dict[str, Any]
) -> list[dict[str, Any]]:
    """Compatibility helper used by the focused stage-one unit tests."""
    return replay_stages(records, artifact, {1})


def summarize(rows: list[dict[str, Any]], artifact: dict[str, Any]) -> dict[str, Any]:
    gate = artifact["gate"]
    audited = [row for row in rows if row.get("eligible_for_gate", True)]
    admitted = [row for row in audited if row["admitted"]]
    false_starts = sum(bool(row["false_start"]) for row in audited)
    per_bag = {
        str(bag): {
            "positions": sum(row["bag"] == bag for row in audited),
            "admitted": sum(row["bag"] == bag and row["admitted"] for row in audited),
            "false_starts": sum(
                row["bag"] == bag and row["false_start"] for row in audited
            ),
        }
        for bag in range(1, 5)
    }
    evidence = 1.0 - float(gate["target_completion_quantile"]) ** len(admitted)
    gate_passed = (
        len(admitted) >= int(gate["minimum_admitted_stages"])
        and false_starts == int(gate["required_false_starts"])
        and all(
            data["positions"] >= int(gate["minimum_positions_per_bag"])
            for data in per_bag.values()
        )
        and evidence >= float(gate["target_quantile_evidence"])
    )
    return {
        "artifact_id": artifact["artifact_id"],
        "stage_index": rows[0]["stage_index"] if rows else None,
        "positions": len(audited),
        "warmup_positions": len(rows) - len(audited),
        "completed": sum(bool(row["completed"]) for row in audited),
        "admitted": len(admitted),
        "false_starts": false_starts,
        "conservative_refusals": sum(
            bool(row["conservative_refusal"]) for row in audited
        ),
        "admission_fraction": len(admitted) / len(audited) if audited else 0.0,
        "target_quantile_evidence": evidence,
        "per_bag": per_bag,
        "gate_passed": gate_passed,
        "rows": rows,
    }


def summarize_by_stage(
    rows: list[dict[str, Any]], artifact: dict[str, Any]
) -> dict[str, Any]:
    stage_indices = sorted({int(row["stage_index"]) for row in rows})
    stages = {
        str(stage_index): summarize(
            [row for row in rows if int(row["stage_index"]) == stage_index],
            artifact,
        )
        for stage_index in stage_indices
    }
    return {
        "artifact_id": artifact["artifact_id"],
        "gate_passed": bool(stages)
        and all(stage["gate_passed"] for stage in stages.values()),
        "stages": stages,
    }


def markdown_stage(summary: dict[str, Any]) -> list[str]:
    lines = [
        f"## Stage {summary['stage_index']} "
        f"({int(summary['stage_index']) + 1}-ply)",
        "",
        f"Positions: {summary['positions']}; completed: {summary['completed']}; "
        f"admitted: {summary['admitted']}; false starts: "
        f"{summary['false_starts']}; conservative refusals: "
        f"{summary['conservative_refusals']}.",
        f"Cold runtime-calibration positions excluded from the gate: "
        f"{summary['warmup_positions']}.",
        "",
        f"Prospective gate: **{'PASS' if summary['gate_passed'] else 'FAIL'}** "
        f"(p95 evidence {summary['target_quantile_evidence']:.2%}).",
        "",
        "| bag | positions | admitted | false starts |",
        "|---:|---:|---:|---:|",
    ]
    for bag, data in summary["per_bag"].items():
        lines.append(
            f"| {bag} | {data['positions']} | {data['admitted']} | "
            f"{data['false_starts']} |"
        )
    lines.extend(
        [
            "",
            "The gate counts only stages the frozen model would have admitted. "
            "A refusal is safe but reduces usefulness; a false start is a "
            "safety failure.",
            "",
        ]
    )
    return lines


def markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# PEG complete-stage admission validation",
        "",
        f"Artifact: `{summary['artifact_id']}`",
        "",
        f"All represented depth gates: "
        f"**{'PASS' if summary['gate_passed'] else 'FAIL'}**.",
        "",
    ]
    for stage_index in sorted(summary["stages"], key=int):
        lines.extend(markdown_stage(summary["stages"][stage_index]))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--records", type=pathlib.Path, nargs="+", required=True
    )
    parser.add_argument(
        "--artifact",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/adaptive_complete_stage_admission_v6_20260801.json"
        ),
    )
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    parser.add_argument(
        "--stage",
        type=int,
        action="append",
        help="audit only this stage index; may be repeated",
    )
    args = parser.parse_args()
    artifact = json.loads(args.artifact.read_text())
    selected_stages = set(args.stage) if args.stage else None
    summary = summarize_by_stage(
        replay_stages(load_jsonls(args.records), artifact, selected_stages),
        artifact,
    )
    rendered = markdown(summary)
    if args.json:
        args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if args.markdown:
        args.markdown.write_text(rendered)
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

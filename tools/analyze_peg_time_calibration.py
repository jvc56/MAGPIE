#!/usr/bin/env python3
"""Analyze PEG calibration JSONL without substituting censored oracle values."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import random
import statistics
from collections import defaultdict
from typing import Any, Callable, Iterable


PAIR_SPECS = (("30s", "60s"), ("60s", "120s"))
METRICS = ("utility", "win", "spread")


def load_records(path: pathlib.Path) -> list[dict[str, Any]]:
    return [
        json.loads(line)
        for line in path.read_text().splitlines()
        if line.strip()
    ]


def latest_by(
    records: Iterable[dict[str, Any]],
    key_function: Callable[[dict[str, Any]], tuple[Any, ...]],
) -> dict[tuple[Any, ...], dict[str, Any]]:
    latest: dict[tuple[Any, ...], dict[str, Any]] = {}
    for record in records:
        key = key_function(record)
        if key not in latest or int(record.get("sequence", -1)) >= int(
            latest[key].get("sequence", -1)
        ):
            latest[key] = record
    return latest


def percentile(sorted_values: list[float], probability: float) -> float:
    if not sorted_values:
        return math.nan
    index = probability * (len(sorted_values) - 1)
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return sorted_values[lower]
    fraction = index - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def stratified_mean_ci(
    observations: list[dict[str, Any]],
    field: str,
    *,
    iterations: int = 10_000,
    seed: int = 633,
) -> dict[str, Any]:
    values = [float(observation[field]) for observation in observations]
    if not values:
        return {"n": 0, "mean": math.nan, "ci_low": math.nan, "ci_high": math.nan}
    by_bag: dict[int, list[float]] = defaultdict(list)
    for observation in observations:
        by_bag[int(observation["bag"])].append(float(observation[field]))
    generator = random.Random(seed)
    bootstrap = []
    for _ in range(iterations):
        sample = []
        for bag_values in by_bag.values():
            sample.extend(
                generator.choice(bag_values) for _ in range(len(bag_values))
            )
        bootstrap.append(statistics.fmean(sample))
    bootstrap.sort()
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "ci_low": percentile(bootstrap, 0.025),
        "ci_high": percentile(bootstrap, 0.975),
    }


def wilson_interval(successes: int, total: int) -> dict[str, Any]:
    if total == 0:
        return {"n": 0, "rate": math.nan, "ci_low": math.nan, "ci_high": math.nan}
    z = 1.959963984540054
    proportion = successes / total
    denominator = 1.0 + z * z / total
    center = (proportion + z * z / (2 * total)) / denominator
    half_width = (
        z
        * math.sqrt(
            proportion * (1 - proportion) / total + z * z / (4 * total * total)
        )
        / denominator
    )
    return {
        "n": total,
        "successes": successes,
        "rate": proportion,
        "ci_low": center - half_width,
        "ci_high": center + half_width,
    }


def describe(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"n": 0, "mean": math.nan, "median": math.nan}
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def value_maps(
    records: list[dict[str, Any]],
    judges: dict[tuple[Any, ...], dict[str, Any]],
) -> dict[tuple[str, str], dict[str, dict[str, float]]]:
    maps: dict[tuple[str, str], dict[str, dict[str, float]]] = {}
    for key, judge in judges.items():
        position, label = key
        if int(judge.get("accepted", 0)) != 1:
            continue
        sequence = int(judge["sequence"])
        values = {
            str(record["move"]): {
                "win": float(record["win"]),
                "spread": float(record["spread"]),
                "utility": float(record["utility"]),
            }
            for record in records
            if record.get("kind") == "value"
            and record.get("position") == position
            and record.get("label") == label
            and int(record.get("sequence", -1)) == sequence
        }
        if len(values) == int(judge.get("nominee_count", -1)):
            maps[(str(position), str(label))] = values
    return maps


def analyze(records: list[dict[str, Any]]) -> dict[str, Any]:
    arm_records = [
        record
        for record in records
        if record.get("kind") == "arm" and record.get("mode") == "arm"
    ]
    arms = latest_by(
        arm_records,
        lambda record: (str(record["position"]), str(record["label"])),
    )
    judge_records = [
        record for record in records if record.get("kind") == "judge"
    ]
    judges = latest_by(
        judge_records,
        lambda record: (str(record["position"]), str(record["label"])),
    )
    oracle_values = value_maps(records, judges)
    positions = sorted({position for position, _ in arms})

    arm_completion: dict[str, Any] = {}
    arm_strength: dict[str, Any] = {}
    for label in ("greedy", "30s", "60s", "120s"):
        rows = [record for (_, arm_label), record in arms.items() if arm_label == label]
        completed = sum(record.get("status") == "completed" for record in rows)
        arm_completion[label] = {
            "total": len(rows),
            "completed": completed,
            "partial_or_seed_fallback": len(rows) - completed,
        }
        fields = (
            "refine_completed_candidates",
            "cumulative_scenarios",
            "nested_endgame_nodes",
            "wall_seconds",
            "process_cpu_seconds",
            "scheduled_core_occupancy",
        )
        arm_strength[label] = {
            field: describe([float(record.get(field, 0)) for record in rows])
            for field in fields
        }
        last_completion_times = []
        for row in rows:
            sequence = int(row.get("sequence", -1))
            completion_times = [
                float(record["completed_seconds"])
                for record in records
                if record.get("kind") == "event"
                and int(record.get("sequence", -2)) == sequence
                and int(record.get("stage", -1)) == 1
            ]
            if completion_times:
                last_completion_times.append(max(completion_times))
        arm_strength[label]["last_completed_candidate_seconds"] = describe(
            last_completion_times
        )

    marginals: dict[str, Any] = {}
    for before_label, after_label in PAIR_SPECS:
        pair_name = f"{before_label}_to_{after_label}"
        observations: list[dict[str, Any]] = []
        conditional: list[dict[str, Any]] = []
        disagreements = 0
        nominated = 0
        censored_disagreements = 0
        for position in positions:
            before = arms.get((position, before_label))
            after = arms.get((position, after_label))
            if before is None or after is None:
                continue
            nominated += 1
            bag = int(before["bag"])
            before_move = str(before["move"])
            after_move = str(after["move"])
            if before_move == after_move:
                observation = {
                    "position": position,
                    "bag": bag,
                    "utility": 0.0,
                    "win": 0.0,
                    "spread": 0.0,
                    "disagreement": False,
                }
                observations.append(observation)
                continue
            disagreements += 1
            values = oracle_values.get((position, "stride4"))
            if (
                values is None
                or before_move not in values
                or after_move not in values
            ):
                censored_disagreements += 1
                continue
            observation = {
                "position": position,
                "bag": bag,
                "utility": values[after_move]["utility"]
                - values[before_move]["utility"],
                "win": values[after_move]["win"] - values[before_move]["win"],
                "spread": values[after_move]["spread"]
                - values[before_move]["spread"],
                "disagreement": True,
            }
            observations.append(observation)
            conditional.append(observation)
        marginals[pair_name] = {
            "disagreement": wilson_interval(disagreements, nominated),
            "accepted": len(observations),
            "censored_disagreements": censored_disagreements,
            "all_accepted": {
                metric: stratified_mean_ci(observations, metric)
                for metric in METRICS
            },
            "conditional_disagreement": {
                metric: stratified_mean_ci(conditional, metric)
                for metric in METRICS
            },
        }

    accepted_stride4 = sum(
        int(judge.get("accepted", 0)) == 1
        for (position, label), judge in judges.items()
        if label == "stride4"
    )
    censored_stride4 = sum(
        int(judge.get("accepted", 0)) != 1
        for (position, label), judge in judges.items()
        if label == "stride4"
    )
    agreements = sum(
        record.get("kind") == "agreement" and int(record.get("accepted", 0)) == 1
        for record in records
    )

    sensitivity_rows = []
    sensitivity_positions = []
    for (position, label), stride2 in judges.items():
        if label != "stride2" or int(stride2.get("accepted", 0)) != 1:
            continue
        stride4_values = oracle_values.get((position, "stride4"))
        stride2_values = oracle_values.get((position, "stride2"))
        if stride4_values is None or stride2_values is None:
            continue
        common_moves = sorted(set(stride4_values) & set(stride2_values))
        if not common_moves:
            continue
        for move in common_moves:
            sensitivity_rows.append(
                {
                    "position": position,
                    "move": move,
                    "win_abs_diff": abs(
                        stride2_values[move]["win"] - stride4_values[move]["win"]
                    ),
                    "spread_abs_diff": abs(
                        stride2_values[move]["spread"]
                        - stride4_values[move]["spread"]
                    ),
                    "utility_abs_diff": abs(
                        stride2_values[move]["utility"]
                        - stride4_values[move]["utility"]
                    ),
                }
            )
        best4 = max(common_moves, key=lambda move: stride4_values[move]["utility"])
        best2 = max(common_moves, key=lambda move: stride2_values[move]["utility"])
        sensitivity_positions.append(
            {"position": position, "best_agrees": best4 == best2}
        )
    stride2_total = sum(label == "stride2" for _, label in judges)
    sensitivity = {
        "accepted_positions": len(sensitivity_positions),
        "censored_positions": stride2_total - len(sensitivity_positions),
        "best_nominee_agreement_rate": (
            statistics.fmean(
                float(row["best_agrees"]) for row in sensitivity_positions
            )
            if sensitivity_positions
            else math.nan
        ),
        "win_abs_diff": describe(
            [float(row["win_abs_diff"]) for row in sensitivity_rows]
        ),
        "spread_abs_diff": describe(
            [float(row["spread_abs_diff"]) for row in sensitivity_rows]
        ),
        "utility_abs_diff": describe(
            [float(row["utility_abs_diff"]) for row in sensitivity_rows]
        ),
    }

    sentinels = latest_by(
        [
            record
            for record in records
            if record.get("kind") == "arm" and record.get("mode") == "sentinel"
        ],
        lambda record: (str(record["label"]),),
    )
    ordered_sentinels = [
        sentinels[(label,)]
        for label in ("before", "between", "after")
        if (label,) in sentinels
    ]
    throughputs = [
        float(record["nested_endgame_nodes"]) / float(record["wall_seconds"])
        for record in ordered_sentinels
        if float(record.get("wall_seconds", 0)) > 0
    ]
    median_throughput = statistics.median(throughputs) if throughputs else math.nan
    sentinel_rows = []
    for record in ordered_sentinels:
        throughput = float(record["nested_endgame_nodes"]) / float(
            record["wall_seconds"]
        )
        normalized = throughput / median_throughput if median_throughput else math.nan
        sentinel_rows.append(
            {
                "label": record["label"],
                "throughput_nodes_per_second": throughput,
                "normalized_throughput": normalized,
                "occupancy": float(record["scheduled_core_occupancy"]),
                "noisy": abs(normalized - 1.0) > 0.20
                or float(record["scheduled_core_occupancy"]) < 0.50,
            }
        )
    throughput_cv = (
        statistics.pstdev(throughputs) / statistics.fmean(throughputs)
        if len(throughputs) > 1 and statistics.fmean(throughputs) != 0
        else 0.0
    )
    drift = (
        throughputs[-1] / throughputs[0] - 1.0
        if len(throughputs) >= 2 and throughputs[0] != 0
        else math.nan
    )
    segment_noise = {"early": False, "late": False}
    if len(sentinel_rows) == 3:
        for segment, endpoints in (
            ("early", sentinel_rows[:2]),
            ("late", sentinel_rows[1:]),
        ):
            endpoint_ratio = (
                endpoints[1]["throughput_nodes_per_second"]
                / endpoints[0]["throughput_nodes_per_second"]
            )
            segment_noise[segment] = (
                any(bool(endpoint["noisy"]) for endpoint in endpoints)
                or endpoint_ratio < 0.80
                or endpoint_ratio > 1.20
            )
    noisy_quality_rows = sum(
        1
        for record in arm_records
        if segment_noise.get(str(record.get("block")), False)
    )
    monitoring = {
        "sentinels": sentinel_rows,
        "normalized_throughput_cv": throughput_cv,
        "early_to_late_drift": drift,
        "segment_noise": segment_noise,
        "arm_rows_in_flagged_segments": noisy_quality_rows,
        "timing_only_exclusion": False,
    }

    return {
        "position_count": len(positions),
        "bag_counts": {
            str(bag): sum(
                1
                for position in positions
                if (position, "greedy") in arms
                if int(arms[(position, "greedy")]["bag"]) == bag
            )
            for bag in range(1, 5)
        },
        "arm_completion": arm_completion,
        "judge_completion": {
            "agreements_exact_zero": agreements,
            "stride4_accepted": accepted_stride4,
            "stride4_censored": censored_stride4,
        },
        "arm_strength": arm_strength,
        "marginals": marginals,
        "sensitivity": sensitivity,
        "monitoring": monitoring,
    }


def format_ci(summary: dict[str, Any], scale: float = 1.0) -> str:
    if int(summary["n"]) == 0:
        return "n/a"
    return (
        f"{summary['mean'] * scale:.4f} "
        f"[{summary['ci_low'] * scale:.4f}, "
        f"{summary['ci_high'] * scale:.4f}] (n={summary['n']})"
    )


def render_markdown(result: dict[str, Any]) -> str:
    lines = [
        "# PEG time calibration analysis",
        "",
        f"Positions: {result['position_count']}; bag counts: "
        f"{result['bag_counts']}. Utility is `win + 1e-4 * spread`. "
        "Intervals are deterministic bag-stratified bootstrap 95% CIs; "
        "disagreement-rate intervals are Wilson 95% CIs.",
        "",
        "## Completion",
        "",
        "| Arm | Runs | Full 2-ply stage | Partial/censored stage |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, row in result["arm_completion"].items():
        lines.append(
            f"| {label} | {row['total']} | {row['completed']} | "
            f"{row['partial_or_seed_fallback']} |"
        )
    judge = result["judge_completion"]
    lines.extend(
        [
            "",
            f"Exact all-arm agreements: {judge['agreements_exact_zero']}; "
            f"stride-4 3-ply judges accepted: {judge['stride4_accepted']}; "
            f"censored: {judge['stride4_censored']}. No censored judgment is "
            "replaced by a seed or 2-ply value.",
            "",
            "## Marginal quality",
            "",
            "| Step | Disagreement rate (95% CI) | Utility gain (95% CI) | "
            "Win gain, pp (95% CI) | Spread gain (95% CI) |",
            "| --- | --- | --- | --- | --- |",
        ]
    )
    for name, marginal in result["marginals"].items():
        disagreement = marginal["disagreement"]
        rate = (
            f"{disagreement['rate']:.3f} "
            f"[{disagreement['ci_low']:.3f}, {disagreement['ci_high']:.3f}] "
            f"({disagreement['successes']}/{disagreement['n']})"
            if disagreement["n"]
            else "n/a"
        )
        accepted = marginal["all_accepted"]
        lines.append(
            f"| {name.replace('_', ' ')} | {rate} | "
            f"{format_ci(accepted['utility'])} | "
            f"{format_ci(accepted['win'], 100.0)} | "
            f"{format_ci(accepted['spread'])} |"
        )
    lines.extend(
        [
            "",
            "Conditional on the two arms nominating different moves:",
            "",
            "| Step | Accepted disagreements | Utility gain (95% CI) | "
            "Win gain, pp (95% CI) | Spread gain (95% CI) | Censored |",
            "| --- | ---: | --- | --- | --- | ---: |",
        ]
    )
    for name, marginal in result["marginals"].items():
        conditional = marginal["conditional_disagreement"]
        lines.append(
            f"| {name.replace('_', ' ')} | "
            f"{conditional['utility']['n']} | "
            f"{format_ci(conditional['utility'])} | "
            f"{format_ci(conditional['win'], 100.0)} | "
            f"{format_ci(conditional['spread'])} | "
            f"{marginal['censored_disagreements']} |"
        )
    lines.extend(
        [
            "",
            "## Portable work",
            "",
            "| Arm | Completed 2-ply candidates, median | Scenarios, median | "
            "Nested endgame nodes, median | Wall seconds, median | Occupancy, median |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for label, strength in result["arm_strength"].items():
        lines.append(
            f"| {label} | "
            f"{strength['refine_completed_candidates']['median']:.1f} | "
            f"{strength['cumulative_scenarios']['median']:.0f} | "
            f"{strength['nested_endgame_nodes']['median']:.0f} | "
            f"{strength['wall_seconds']['median']:.2f} | "
            f"{strength['scheduled_core_occupancy']['median']:.3f} |"
        )
    sensitivity = result["sensitivity"]
    lines.extend(
        [
            "",
            "## Stride-2 sensitivity",
            "",
            f"Accepted positions: {sensitivity['accepted_positions']}; "
            f"censored: {sensitivity['censored_positions']}; best-nominee "
            f"agreement: {sensitivity['best_nominee_agreement_rate']:.3f}. "
            f"Across common nominees, median absolute stride-2 versus stride-4 "
            f"difference was {sensitivity['win_abs_diff']['median']:.6f} win, "
            f"{sensitivity['spread_abs_diff']['median']:.3f} spread, and "
            f"{sensitivity['utility_abs_diff']['median']:.6f} utility.",
            "",
            "## Load monitoring",
            "",
            f"Normalized sentinel throughput CV: "
            f"{result['monitoring']['normalized_throughput_cv']:.3f}; "
            f"before-to-after drift: "
            f"{result['monitoring']['early_to_late_drift']:+.1%}. "
            f"Flagged segments: {result['monitoring']['segment_noise']}. "
            "Timing flags do not remove structurally valid quality observations.",
            "",
            "| Sentinel | Nodes/s | Normalized | CPU/wall/10 occupancy | Noisy |",
            "| --- | ---: | ---: | ---: | --- |",
        ]
    )
    for sentinel in result["monitoring"]["sentinels"]:
        lines.append(
            f"| {sentinel['label']} | "
            f"{sentinel['throughput_nodes_per_second']:.0f} | "
            f"{sentinel['normalized_throughput']:.3f} | "
            f"{sentinel['occupancy']:.3f} | {sentinel['noisy']} |"
        )
    lines.extend(
        [
            "",
            "Wall time is a local conversion estimate only. Candidate "
            "completions, cumulative scenarios, and nested endgame nodes are "
            "the strength/work coordinates.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("records", nargs="+", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    args = parser.parse_args()
    records = []
    for records_path in args.records:
        records.extend(load_records(records_path))
    result = analyze(records)
    json_text = json.dumps(result, indent=2, sort_keys=True, allow_nan=True) + "\n"
    markdown = render_markdown(result)
    if args.json:
        args.json.write_text(json_text)
    else:
        print(json_text, end="")
    if args.markdown:
        args.markdown.write_text(markdown)
    else:
        print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Analyze PEG quality and work at completed-candidate boundaries."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
import sys
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from analyze_peg_time_calibration import (  # noqa: E402
    describe,
    latest_by,
    stratified_mean_ci,
    summarize_sentinel_run,
    value_maps,
    wilson_interval,
)


CHECKPOINTS = (0, 1, 2, 4, 8, 12, 16, 24, 32)
METRICS = ("utility", "win", "spread")
WORK_FIELDS = (
    "cumulative_scenarios",
    "nested_endgame_nodes",
    "completed_seconds",
    "process_cpu_seconds",
    "scheduled_core_occupancy",
)


def load_records(path: pathlib.Path) -> list[dict[str, Any]]:
    return [
        json.loads(line)
        for line in path.read_text().splitlines()
        if line.strip()
    ]


def metric_delta(
    values: dict[str, dict[str, float]], after: str, before: str, metric: str
) -> float:
    return float(values[after][metric]) - float(values[before][metric])


def zero_observation(position: str, bag: int) -> dict[str, Any]:
    return {
        "position": position,
        "bag": bag,
        "utility": 0.0,
        "win": 0.0,
        "spread": 0.0,
    }


def comparison_observation(
    *,
    position: str,
    bag: int,
    before: str,
    after: str,
    values: dict[str, dict[str, float]] | None,
) -> dict[str, Any] | None:
    if before == after:
        return zero_observation(position, bag)
    if values is None or before not in values or after not in values:
        return None
    return {
        "position": position,
        "bag": bag,
        **{
            metric: metric_delta(values, after, before, metric)
            for metric in METRICS
        },
    }


def summaries(observations: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        metric: stratified_mean_ci(observations, metric)
        for metric in METRICS
    }


def describe_work(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        field: describe([float(row[field]) for row in rows])
        for field in WORK_FIELDS
    }


def checkpoint_work(
    checkpoint: dict[str, Any], bag: int, position: str
) -> dict[str, Any]:
    wall = float(checkpoint["completed_seconds"])
    cpu = float(checkpoint["process_cpu_seconds"])
    return {
        "position": position,
        "bag": bag,
        "cumulative_scenarios": float(checkpoint["cumulative_scenarios"]),
        "nested_endgame_nodes": float(checkpoint["nested_endgame_nodes"]),
        "completed_seconds": wall,
        "process_cpu_seconds": cpu,
        "scheduled_core_occupancy": cpu / (wall * 10.0) if wall > 0 else 0.0,
    }


def analyze(records: list[dict[str, Any]]) -> dict[str, Any]:
    arm_records = [
        record
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "arm"
        and record.get("label") == "checkpoint"
    ]
    arms = latest_by(
        arm_records, lambda record: (str(record["position"]),)
    )
    maps = latest_by(
        (
            record
            for record in records
            if record.get("kind") == "checkpoint_map"
        ),
        lambda record: (str(record["position"]),),
    )
    judge_records = [
        record for record in records if record.get("kind") == "judge"
    ]
    judges = latest_by(
        judge_records,
        lambda record: (str(record["position"]), str(record["label"])),
    )
    oracle_values = value_maps(records, judges)
    positions = sorted(position for (position,) in maps)

    traces = {
        "total": len(positions),
        "full_stage": sum(
            int(maps[(position,)]["full_stage"]) for position in positions
        ),
        "partial_stage": sum(
            not int(maps[(position,)]["full_stage"]) for position in positions
        ),
        "returned": len(arms),
    }
    frontier_counts = [
        len(maps[(position,)]["frontier_moves"]) for position in positions
    ]
    frontier = {
        "distinct_checkpoint_nominees": describe(
            [float(count) for count in frontier_counts]
        ),
        "total_position_nominees": sum(frontier_counts),
        "exact_all_checkpoint_agreements": sum(count == 1 for count in frontier_counts),
    }

    checkpoint_rows: dict[str, Any] = {}
    previous_k: int | None = None
    for k in CHECKPOINTS:
        eligible = []
        gain_rows = []
        marginal_rows = []
        full_regret_rows = []
        conditional_full_regret = []
        oracle_frontier_regret_rows = []
        work_rows = []
        work_increment_rows = []
        disagree_greedy = 0
        disagree_full = 0
        censored_gain = 0
        censored_full_regret = 0
        for position in positions:
            checkpoint_map = maps[(position,)]
            checkpoints = checkpoint_map["checkpoints"]
            if int(checkpoint_map["completed_candidates"]) < k:
                continue
            eligible.append(position)
            bag = int(checkpoint_map["bag"])
            current = checkpoints[k]
            current_move = str(current["move"])
            greedy_move = str(checkpoints[0]["move"])
            final_move = str(checkpoints[-1]["move"])
            if current_move != greedy_move:
                disagree_greedy += 1
            if current_move != final_move:
                disagree_full += 1
            values = oracle_values.get((position, "checkpoint_stride4"))

            gain = comparison_observation(
                position=position,
                bag=bag,
                before=greedy_move,
                after=current_move,
                values=values,
            )
            if gain is None:
                censored_gain += 1
            else:
                gain_rows.append(gain)

            full_regret = comparison_observation(
                position=position,
                bag=bag,
                before=current_move,
                after=final_move,
                values=values,
            )
            if full_regret is None:
                censored_full_regret += 1
            else:
                full_regret_rows.append(full_regret)
                if current_move != final_move:
                    conditional_full_regret.append(full_regret)

            if values is None:
                if len(checkpoint_map["frontier_moves"]) == 1:
                    oracle_frontier_regret_rows.append(
                        zero_observation(position, bag)
                    )
            else:
                frontier_moves = [
                    str(move)
                    for move in checkpoint_map["frontier_moves"]
                    if str(move) in values
                ]
                if len(frontier_moves) == len(
                    checkpoint_map["frontier_moves"]
                ):
                    oracle_best = max(
                        frontier_moves,
                        key=lambda move: float(values[move]["utility"]),
                    )
                    oracle_frontier_regret_rows.append(
                        comparison_observation(
                            position=position,
                            bag=bag,
                            before=current_move,
                            after=oracle_best,
                            values=values,
                        )
                    )

            work_rows.append(checkpoint_work(current, bag, position))
            if previous_k is not None:
                previous = checkpoints[previous_k]
                current_work = checkpoint_work(current, bag, position)
                previous_work = checkpoint_work(previous, bag, position)
                work_increment_rows.append(
                    {
                        "position": position,
                        "bag": bag,
                        **{
                            field: current_work[field] - previous_work[field]
                            for field in WORK_FIELDS
                            if field != "scheduled_core_occupancy"
                        },
                        "scheduled_core_occupancy": current_work[
                            "scheduled_core_occupancy"
                        ],
                    }
                )
                previous_move = str(previous["move"])
                marginal = comparison_observation(
                    position=position,
                    bag=bag,
                    before=previous_move,
                    after=current_move,
                    values=values,
                )
                if marginal is not None:
                    marginal_rows.append(marginal)

        checkpoint_rows[str(k)] = {
            "eligible": len(eligible),
            "censored_for_missing_candidates": len(positions) - len(eligible),
            "disagreement_with_greedy": wilson_interval(
                disagree_greedy, len(eligible)
            ),
            "disagreement_with_full": wilson_interval(
                disagree_full, len(eligible)
            ),
            "gain_vs_greedy": summaries(gain_rows),
            "gain_vs_greedy_accepted": len(gain_rows),
            "gain_vs_greedy_censored": censored_gain,
            "marginal_vs_previous_checkpoint": summaries(marginal_rows),
            "full_minus_stop": summaries(full_regret_rows),
            "full_minus_stop_accepted": len(full_regret_rows),
            "full_minus_stop_censored": censored_full_regret,
            "conditional_full_minus_stop": summaries(
                conditional_full_regret
            ),
            "oracle_frontier_minus_stop": summaries(
                [
                    row
                    for row in oracle_frontier_regret_rows
                    if row is not None
                ]
            ),
            "work": describe_work(work_rows),
            "incremental_work": describe_work(work_increment_rows),
        }
        previous_k = k

    stabilization = []
    for position in positions:
        checkpoint_map = maps[(position,)]
        checkpoints = checkpoint_map["checkpoints"]
        final_move = str(checkpoints[-1]["move"])
        first_final = next(
            int(checkpoint["completed_candidates"])
            for checkpoint in checkpoints
            if str(checkpoint["move"]) == final_move
        )
        stabilization.append(
            {
                "position": position,
                "bag": int(checkpoint_map["bag"]),
                "first_final_candidate": first_final,
            }
        )
    stabilization_summary = {
        "overall": describe(
            [float(row["first_final_candidate"]) for row in stabilization]
        ),
        "by_bag": {
            str(bag): describe(
                [
                    float(row["first_final_candidate"])
                    for row in stabilization
                    if int(row["bag"]) == bag
                ]
            )
            for bag in range(1, 5)
        },
    }

    judge_completion = {}
    judge_strength = {}
    for label in ("checkpoint_stride4", "checkpoint_stride2"):
        rows = [
            judge
            for (_, judge_label), judge in judges.items()
            if judge_label == label
        ]
        judge_completion[label] = {
            "attempted": len(rows),
            "accepted": sum(int(row.get("accepted", 0)) == 1 for row in rows),
            "censored": sum(int(row.get("accepted", 0)) != 1 for row in rows),
        }
        judge_strength[label] = {
            field: describe([float(row.get(field, 0)) for row in rows])
            for field in (
                "cumulative_scenarios",
                "nested_endgame_nodes",
                "wall_seconds",
                "process_cpu_seconds",
                "scheduled_core_occupancy",
            )
        }

    sensitivity_positions = []
    sensitivity_nominees = []
    for position in positions:
        values4 = oracle_values.get((position, "checkpoint_stride4"))
        values2 = oracle_values.get((position, "checkpoint_stride2"))
        if values4 is None or values2 is None:
            continue
        common = sorted(set(values4) & set(values2))
        if not common:
            continue
        max4 = max(float(values4[move]["utility"]) for move in common)
        max2 = max(float(values2[move]["utility"]) for move in common)
        best4 = {
            move
            for move in common
            if abs(float(values4[move]["utility"]) - max4) <= 1.0e-12
        }
        best2 = {
            move
            for move in common
            if abs(float(values2[move]["utility"]) - max2) <= 1.0e-12
        }
        sensitivity_positions.append(bool(best4 & best2))
        for move in common:
            sensitivity_nominees.append(
                {
                    metric: abs(
                        float(values4[move][metric])
                        - float(values2[move][metric])
                    )
                    for metric in METRICS
                }
            )
    sensitivity = {
        "accepted_positions": len(sensitivity_positions),
        "best_nominee_agreement_rate": (
            statistics.fmean(float(value) for value in sensitivity_positions)
            if sensitivity_positions
            else math.nan
        ),
        **{
            f"{metric}_abs_diff": describe(
                [float(row[metric]) for row in sensitivity_nominees]
            )
            for metric in METRICS
        },
    }

    sentinel_records = [
        record
        for record in records
        if record.get("kind") == "arm" and record.get("mode") == "sentinel"
    ]
    monitoring = summarize_sentinel_run(sentinel_records, arm_records)

    return {
        "position_count": len(positions),
        "bag_counts": {
            str(bag): sum(
                int(maps[(position,)]["bag"]) == bag for position in positions
            )
            for bag in range(1, 5)
        },
        "trace_completion": traces,
        "frontier": frontier,
        "checkpoint_curve": checkpoint_rows,
        "final_move_stabilization": stabilization_summary,
        "judge_completion": judge_completion,
        "judge_strength": judge_strength,
        "sensitivity": sensitivity,
        "monitoring": monitoring,
    }


def format_ci(summary: dict[str, Any], scale: float = 1.0) -> str:
    if int(summary["n"]) == 0:
        return "n/a"
    return (
        f"{summary['mean'] * scale:+.4f} "
        f"[{summary['ci_low'] * scale:+.4f}, "
        f"{summary['ci_high'] * scale:+.4f}]"
    )


def format_rate(summary: dict[str, Any]) -> str:
    if int(summary["n"]) == 0:
        return "n/a"
    return (
        f"{summary['successes']}/{summary['n']} = {summary['rate']:.3f} "
        f"[{summary['ci_low']:.3f}, {summary['ci_high']:.3f}]"
    )


def render_markdown(result: dict[str, Any]) -> str:
    trace = result["trace_completion"]
    frontier = result["frontier"]
    lines = [
        "# PEG candidate-checkpoint calibration",
        "",
        f"Positions: {result['position_count']}; bag counts: "
        f"{result['bag_counts']}. Utility is `win + 1e-4 * spread`. "
        "Intervals are deterministic bag-stratified bootstrap 95% CIs; "
        "rates use Wilson 95% CIs.",
        "",
        "## Completion",
        "",
        f"Uncapped 2-ply traces returned: {trace['returned']}; full 32-candidate "
        f"stages: {trace['full_stage']}; partial: {trace['partial_stage']}. "
        f"Checkpoint frontiers contained {frontier['total_position_nominees']} "
        f"position-nominees (median "
        f"{frontier['distinct_checkpoint_nominees']['median']:.1f}, max "
        f"{frontier['distinct_checkpoint_nominees']['max']:.0f}); "
        f"{frontier['exact_all_checkpoint_agreements']} positions never changed "
        "nominee.",
        "",
        "| Judge | Accepted/attempted | Censored | Median scenarios | "
        "Median nested nodes | Median wall seconds | Median occupancy |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for label in ("checkpoint_stride4", "checkpoint_stride2"):
        completion = result["judge_completion"][label]
        strength = result["judge_strength"][label]
        lines.append(
            f"| {label} | {completion['accepted']}/{completion['attempted']} | "
            f"{completion['censored']} | "
            f"{strength['cumulative_scenarios']['median']:.0f} | "
            f"{strength['nested_endgame_nodes']['median']:.0f} | "
            f"{strength['wall_seconds']['median']:.2f} | "
            f"{strength['scheduled_core_occupancy']['median']:.3f} |"
        )

    lines.extend(
        [
            "",
            "## Fixed candidate-count stopping policies",
            "",
            "Positive gain is better than greedy. Positive `full minus stop` "
            "means the completed 32-candidate stage was better than stopping "
            "at that checkpoint.",
            "",
            "| 2-ply candidates | Accepted | Disagree with greedy | "
            "Gain utility (95% CI) | Win gain, pp | Spread gain | "
            "Disagree with full | Full minus stop utility (95% CI) |",
            "| ---: | ---: | --- | --- | --- | --- | --- | --- |",
        ]
    )
    for k in CHECKPOINTS:
        row = result["checkpoint_curve"][str(k)]
        lines.append(
            f"| {k} | {row['gain_vs_greedy_accepted']}/{row['eligible']} | "
            f"{format_rate(row['disagreement_with_greedy'])} | "
            f"{format_ci(row['gain_vs_greedy']['utility'])} | "
            f"{format_ci(row['gain_vs_greedy']['win'], 100.0)} | "
            f"{format_ci(row['gain_vs_greedy']['spread'])} | "
            f"{format_rate(row['disagreement_with_full'])} | "
            f"{format_ci(row['full_minus_stop']['utility'])} |"
        )

    lines.extend(
        [
            "",
            "Conditional regret when the checkpoint and full stage nominated "
            "different moves:",
            "",
            "| Candidates | Disagreements | Full minus stop utility | "
            "Win, pp | Spread |",
            "| ---: | ---: | --- | --- | --- |",
        ]
    )
    for k in CHECKPOINTS:
        row = result["checkpoint_curve"][str(k)]
        conditional = row["conditional_full_minus_stop"]
        lines.append(
            f"| {k} | {conditional['utility']['n']} | "
            f"{format_ci(conditional['utility'])} | "
            f"{format_ci(conditional['win'], 100.0)} | "
            f"{format_ci(conditional['spread'])} |"
        )

    lines.extend(
        [
            "",
            "## Portable work",
            "",
            "| Candidates | Scenarios, median | Nested nodes, median | "
            "Wall seconds, median | CPU seconds, median | Occupancy, median |",
            "| ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for k in CHECKPOINTS:
        work = result["checkpoint_curve"][str(k)]["work"]
        lines.append(
            f"| {k} | {work['cumulative_scenarios']['median']:.0f} | "
            f"{work['nested_endgame_nodes']['median']:.0f} | "
            f"{work['completed_seconds']['median']:.2f} | "
            f"{work['process_cpu_seconds']['median']:.2f} | "
            f"{work['scheduled_core_occupancy']['median']:.3f} |"
        )

    stabilization = result["final_move_stabilization"]
    sensitivity = result["sensitivity"]
    monitoring = result["monitoring"]
    lines.extend(
        [
            "",
            "## Stabilization and sensitivity",
            "",
            f"The final 2-ply move first became the running checkpoint winner "
            f"at candidate {stabilization['overall']['median']:.1f} median "
            f"(mean {stabilization['overall']['mean']:.2f}, max "
            f"{stabilization['overall']['max']:.0f}). Bag medians were "
            + ", ".join(
                f"{bag}: {stabilization['by_bag'][str(bag)]['median']:.1f}"
                for bag in range(1, 5)
            )
            + ".",
            "",
            f"Stride-2 accepted positions: "
            f"{sensitivity['accepted_positions']}; best-frontier-nominee "
            f"agreement with stride 4: "
            f"{sensitivity['best_nominee_agreement_rate']:.3f}. Median "
            f"absolute stride difference across common nominees: "
            f"{sensitivity['win_abs_diff']['median']:.6f} win, "
            f"{sensitivity['spread_abs_diff']['median']:.3f} spread, "
            f"{sensitivity['utility_abs_diff']['median']:.6f} utility.",
            "",
            "## Load monitoring",
            "",
            f"Normalized throughput CV: "
            f"{monitoring['normalized_throughput_cv']:.3f}; before-to-after "
            f"drift: {monitoring['early_to_late_drift']:+.1%}; flagged "
            f"segments: {monitoring['segment_noise']}.",
            "",
            "| Sentinel | Nodes/s | Normalized | CPU/wall/10 occupancy | Noisy |",
            "| --- | ---: | ---: | ---: | --- |",
        ]
    )
    for sentinel in monitoring["sentinels"]:
        lines.append(
            f"| {sentinel['label']} | "
            f"{sentinel['throughput_nodes_per_second']:.0f} | "
            f"{sentinel['normalized_throughput']:.3f} | "
            f"{sentinel['occupancy']:.3f} | {sentinel['noisy']} |"
        )
    lines.extend(
        [
            "",
            "Candidate completions are the stopping boundary. Scenarios and "
            "nested nodes are the portable work coordinates; wall time is a "
            "local conversion only.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("records", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--markdown", type=pathlib.Path)
    args = parser.parse_args()
    result = analyze(load_records(args.records))
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

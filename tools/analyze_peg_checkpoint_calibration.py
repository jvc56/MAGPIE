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
    percentile,
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


def cohort(position: str) -> str:
    return "expansion" if position.startswith("x-") else "initial"


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


def describe_quantiles(values: list[float]) -> dict[str, Any]:
    result = describe(values)
    ordered = sorted(values)
    result.update(
        {
            "p75": percentile(ordered, 0.75),
            "p90": percentile(ordered, 0.90),
            "p95": percentile(ordered, 0.95),
        }
    )
    return result


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


def patience_stop(
    checkpoints: list[dict[str, Any]],
    minimum_candidates: int,
    patience: int,
) -> int:
    last_change = 0
    for completed in range(1, len(checkpoints)):
        if str(checkpoints[completed]["move"]) != str(
            checkpoints[completed - 1]["move"]
        ):
            last_change = completed
        if (
            completed >= minimum_candidates
            and completed - last_change >= patience
        ):
            return completed
    return len(checkpoints) - 1


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
    self_utility_ties = []
    for position in positions:
        arm = arms.get((position,))
        if arm is None:
            continue
        sequence = int(arm["sequence"])
        events = sorted(
            (
                record
                for record in records
                if record.get("kind") == "event"
                and int(record.get("sequence", -1)) == sequence
                and int(record.get("stage", -1)) == 1
            ),
            key=lambda record: int(record["rank"]),
        )
        best_key = -math.inf
        best_move = ""
        for event in events:
            key = float(event["win"]) + 1.0e-4 * float(event["spread"])
            if key > best_key + 1.0e-12:
                best_key = key
                best_move = str(event["move"])
            elif (
                abs(key - best_key) <= 1.0e-12
                and str(event["move"]) != best_move
            ):
                self_utility_ties.append(
                    {
                        "position": position,
                        "candidate": int(event["rank"]) + 1,
                        "incumbent_move": best_move,
                        "tied_move": str(event["move"]),
                        "self_utility": key,
                    }
                )
    frontier["self_utility_checkpoint_ties"] = self_utility_ties

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
            "gain_vs_greedy_by_bag": {
                str(bag): summaries(
                    [
                        row
                        for row in gain_rows
                        if int(row["bag"]) == bag
                    ]
                )
                for bag in range(1, 5)
            },
            "gain_vs_greedy_by_cohort": {
                name: summaries(
                    [
                        row
                        for row in gain_rows
                        if cohort(str(row["position"])) == name
                    ]
                )
                for name in ("initial", "expansion")
            },
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

    admission_rows = []
    for position in positions:
        checkpoint_map = maps[(position,)]
        if int(checkpoint_map["completed_candidates"]) < 2:
            continue
        bag = int(checkpoint_map["bag"])
        before = checkpoint_work(
            checkpoint_map["checkpoints"][0], bag, position
        )
        after = checkpoint_work(
            checkpoint_map["checkpoints"][2], bag, position
        )
        wall = after["completed_seconds"] - before["completed_seconds"]
        cpu = (
            after["process_cpu_seconds"] - before["process_cpu_seconds"]
        )
        admission_rows.append(
            {
                "position": position,
                "bag": bag,
                "cumulative_scenarios": (
                    after["cumulative_scenarios"]
                    - before["cumulative_scenarios"]
                ),
                "nested_endgame_nodes": (
                    after["nested_endgame_nodes"]
                    - before["nested_endgame_nodes"]
                ),
                "completed_seconds": wall,
                "process_cpu_seconds": cpu,
                "scheduled_core_occupancy": (
                    cpu / (wall * 10.0) if wall > 0 else 0.0
                ),
            }
        )
    admission = {
        "minimum_completed_candidates": 2,
        "overall": {
            field: describe_quantiles(
                [float(row[field]) for row in admission_rows]
            )
            for field in WORK_FIELDS
        },
        "by_bag": {
            str(bag): {
                field: describe_quantiles(
                    [
                        float(row[field])
                        for row in admission_rows
                        if int(row["bag"]) == bag
                    ]
                )
                for field in WORK_FIELDS
            }
            for bag in range(1, 5)
        },
    }

    stopping_policies = {}
    for policy_name, minimum_candidates, patience in (
        ("min8_patience4", 8, 4),
        ("min8_patience8", 8, 8),
        ("min12_patience4", 12, 4),
        ("min12_patience8", 12, 8),
    ):
        stops = []
        stop_rows = []
        gain_rows = []
        full_regret_rows = []
        conditional_full_regret = []
        work_rows = []
        disagreements = 0
        censored = 0
        for position in positions:
            checkpoint_map = maps[(position,)]
            checkpoints = checkpoint_map["checkpoints"]
            stop = patience_stop(
                checkpoints, minimum_candidates, patience
            )
            stops.append(float(stop))
            stop_rows.append({"position": position, "stop": float(stop)})
            bag = int(checkpoint_map["bag"])
            greedy_move = str(checkpoints[0]["move"])
            stop_move = str(checkpoints[stop]["move"])
            full_move = str(checkpoints[-1]["move"])
            values = oracle_values.get((position, "checkpoint_stride4"))
            gain = comparison_observation(
                position=position,
                bag=bag,
                before=greedy_move,
                after=stop_move,
                values=values,
            )
            regret = comparison_observation(
                position=position,
                bag=bag,
                before=stop_move,
                after=full_move,
                values=values,
            )
            if gain is None or regret is None:
                censored += 1
                continue
            gain_rows.append(gain)
            full_regret_rows.append(regret)
            if stop_move != full_move:
                disagreements += 1
                conditional_full_regret.append(regret)
            work_rows.append(
                checkpoint_work(checkpoints[stop], bag, position)
            )
        stopping_policies[policy_name] = {
            "minimum_candidates": minimum_candidates,
            "patience": patience,
            "stop_candidates": describe(stops),
            "gain_vs_greedy": summaries(gain_rows),
            "full_minus_stop": summaries(full_regret_rows),
            "conditional_full_minus_stop": summaries(
                conditional_full_regret
            ),
            "disagreement_with_full": wilson_interval(
                disagreements, len(stops)
            ),
            "work": describe_work(work_rows),
            "accepted": len(gain_rows),
            "censored": censored,
            "by_cohort": {
                name: {
                    "stop_candidates": describe(
                        [
                            float(row["stop"])
                            for row in stop_rows
                            if cohort(str(row["position"])) == name
                        ]
                    ),
                    "gain_vs_greedy": summaries(
                        [
                            row
                            for row in gain_rows
                            if cohort(str(row["position"])) == name
                        ]
                    ),
                    "full_minus_stop": summaries(
                        [
                            row
                            for row in full_regret_rows
                            if cohort(str(row["position"])) == name
                        ]
                    ),
                }
                for name in ("initial", "expansion")
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
        sensitivity_positions.append(
            {
                "position": position,
                "agrees": bool(best4 & best2),
                "stride4_best": sorted(best4),
                "stride2_best": sorted(best2),
            }
        )
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
            statistics.fmean(
                float(row["agrees"]) for row in sensitivity_positions
            )
            if sensitivity_positions
            else math.nan
        ),
        "best_disagreement_positions": [
            row for row in sensitivity_positions if not row["agrees"]
        ],
        **{
            f"{metric}_abs_diff": describe(
                [float(row[metric]) for row in sensitivity_nominees]
            )
            for metric in METRICS
        },
    }
    sensitivity_curve = {}
    for k in CHECKPOINTS:
        stride4_rows = []
        stride2_rows = []
        paired_differences = []
        for position in positions:
            checkpoint_map = maps[(position,)]
            if int(checkpoint_map["completed_candidates"]) < k:
                continue
            values4 = oracle_values.get((position, "checkpoint_stride4"))
            values2 = oracle_values.get((position, "checkpoint_stride2"))
            if values4 is None or values2 is None:
                continue
            bag = int(checkpoint_map["bag"])
            greedy_move = str(checkpoint_map["checkpoints"][0]["move"])
            current_move = str(checkpoint_map["checkpoints"][k]["move"])
            gain4 = comparison_observation(
                position=position,
                bag=bag,
                before=greedy_move,
                after=current_move,
                values=values4,
            )
            gain2 = comparison_observation(
                position=position,
                bag=bag,
                before=greedy_move,
                after=current_move,
                values=values2,
            )
            if gain4 is None or gain2 is None:
                continue
            stride4_rows.append(gain4)
            stride2_rows.append(gain2)
            paired_differences.append(
                {
                    "position": position,
                    "bag": bag,
                    **{
                        metric: float(gain2[metric])
                        - float(gain4[metric])
                        for metric in METRICS
                    },
                }
            )
        sensitivity_curve[str(k)] = {
            "stride4_gain_vs_greedy": summaries(stride4_rows),
            "stride2_gain_vs_greedy": summaries(stride2_rows),
            "stride2_minus_stride4": summaries(paired_differences),
        }
    sensitivity["checkpoint_curve"] = sensitivity_curve

    sentinel_records = [
        record
        for record in records
        if record.get("kind") == "arm" and record.get("mode") == "sentinel"
    ]
    monitoring = summarize_sentinel_run(sentinel_records, arm_records)

    full_oracle_best_agreements = 0
    full_oracle_best_accepted = 0
    for position in positions:
        checkpoint_map = maps[(position,)]
        moves = [str(move) for move in checkpoint_map["frontier_moves"]]
        final_move = str(checkpoint_map["final_move"])
        if len(moves) == 1:
            full_oracle_best_agreements += 1
            full_oracle_best_accepted += 1
            continue
        values = oracle_values.get((position, "checkpoint_stride4"))
        if values is None or any(move not in values for move in moves):
            continue
        maximum = max(float(values[move]["utility"]) for move in moves)
        best = {
            move
            for move in moves
            if abs(float(values[move]["utility"]) - maximum) <= 1.0e-12
        }
        full_oracle_best_accepted += 1
        full_oracle_best_agreements += int(final_move in best)
    oracle_frontier = {
        "full_move_best_agreements": full_oracle_best_agreements,
        "accepted": full_oracle_best_accepted,
        "best_minus_full": checkpoint_rows["32"][
            "oracle_frontier_minus_stop"
        ],
    }

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
        "stage_admission": admission,
        "stopping_policies": stopping_policies,
        "oracle_frontier": oracle_frontier,
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
            "Initial-panel versus expansion replication:",
            "",
            "| Cohort | 8-candidate gain | 12-candidate gain | "
            "Full gain | min8/patience8 gain | min8/patience8 full-minus-stop "
            "| Mean adaptive stop |",
            "| --- | --- | --- | --- | --- | --- | ---: |",
        ]
    )
    adaptive = result["stopping_policies"]["min8_patience8"]
    for name in ("initial", "expansion"):
        gain8 = result["checkpoint_curve"]["8"][
            "gain_vs_greedy_by_cohort"
        ][name]["utility"]
        gain12 = result["checkpoint_curve"]["12"][
            "gain_vs_greedy_by_cohort"
        ][name]["utility"]
        gain32 = result["checkpoint_curve"]["32"][
            "gain_vs_greedy_by_cohort"
        ][name]["utility"]
        adaptive_cohort = adaptive["by_cohort"][name]
        lines.append(
            f"| {name} | {format_ci(gain8)} | {format_ci(gain12)} | "
            f"{format_ci(gain32)} | "
            f"{format_ci(adaptive_cohort['gain_vs_greedy']['utility'])} | "
            f"{format_ci(adaptive_cohort['full_minus_stop']['utility'])} | "
            f"{adaptive_cohort['stop_candidates']['mean']:.2f} |"
        )

    lines.extend(
        [
            "",
            "## Adaptive plateau stopping",
            "",
            "A `min N, patience P` rule stops after at least N candidates "
            "once P consecutive candidates have failed to change the running "
            "winner. These are retrospective policy simulations; they use no "
            "oracle information to choose the stop.",
            "",
            "| Rule | Stop candidates, mean / median | Disagree with full | "
            "Gain vs greedy utility | Full minus stop utility | "
            "Nested nodes, median |",
            "| --- | ---: | --- | --- | --- | ---: |",
        ]
    )
    for name, policy in result["stopping_policies"].items():
        stops = policy["stop_candidates"]
        lines.append(
            f"| {name.replace('_', ' ')} | "
            f"{stops['mean']:.2f} / {stops['median']:.1f} | "
            f"{format_rate(policy['disagreement_with_full'])} | "
            f"{format_ci(policy['gain_vs_greedy']['utility'])} | "
            f"{format_ci(policy['full_minus_stop']['utility'])} | "
            f"{policy['work']['nested_endgame_nodes']['median']:.0f} |"
        )

    lines.extend(
        [
            "",
            "Marginal value and work since the previous reported checkpoint:",
            "",
            "| Step | Utility gain (95% CI) | Win gain, pp | Spread gain | "
            "Scenario gain, mean / median | Nested-node gain, mean / median |",
            "| --- | --- | --- | --- | ---: | ---: |",
        ]
    )
    previous = None
    for k in CHECKPOINTS:
        if previous is None:
            previous = k
            continue
        row = result["checkpoint_curve"][str(k)]
        marginal = row["marginal_vs_previous_checkpoint"]
        work = row["incremental_work"]
        lines.append(
            f"| {previous} to {k} | "
            f"{format_ci(marginal['utility'])} | "
            f"{format_ci(marginal['win'], 100.0)} | "
            f"{format_ci(marginal['spread'])} | "
            f"{work['cumulative_scenarios']['mean']:.0f} / "
            f"{work['cumulative_scenarios']['median']:.0f} | "
            f"{work['nested_endgame_nodes']['mean']:.0f} / "
            f"{work['nested_endgame_nodes']['median']:.0f} |"
        )
        previous = k

    lines.extend(
        [
            "",
            "Mean utility gain versus greedy by bag size:",
            "",
            "| Bag | 2 candidates | 4 candidates | 8 candidates | "
            "12 candidates | 32 candidates |",
            "| ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for bag in range(1, 5):
        cells = [
            result["checkpoint_curve"][str(k)]["gain_vs_greedy_by_bag"][
                str(bag)
            ]["utility"]["mean"]
            for k in (2, 4, 8, 12, 32)
        ]
        lines.append(
            f"| {bag} | "
            + " | ".join(f"{value:+.4f}" for value in cells)
            + " |"
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
    admission = result["stage_admission"]
    oracle_frontier = result["oracle_frontier"]
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
            "On the common stride-sensitivity subset, fixed-stop utility gains "
            "were:",
            "",
            "| Candidates | Stride-4 gain | Stride-2 gain | "
            "Stride-2 minus stride-4 |",
            "| ---: | --- | --- | --- |",
        ]
    )
    for k in CHECKPOINTS:
        row = sensitivity["checkpoint_curve"][str(k)]
        lines.append(
            f"| {k} | "
            f"{format_ci(row['stride4_gain_vs_greedy']['utility'])} | "
            f"{format_ci(row['stride2_gain_vs_greedy']['utility'])} | "
            f"{format_ci(row['stride2_minus_stride4']['utility'])} |"
        )
    disagreement_text = ", ".join(
        f"{row['position']} ({'/'.join(row['stride4_best'])} vs "
        f"{'/'.join(row['stride2_best'])})"
        for row in sensitivity["best_disagreement_positions"]
    )
    lines.extend(
        [
            "",
            (
                f"Best-frontier sensitivity disagreements: {disagreement_text}."
                if disagreement_text
                else "There were no best-frontier sensitivity disagreements."
            ),
            "",
            f"The completed 2-ply move was also in the direct-3-ply best "
            f"checkpoint-nominee set on "
            f"{oracle_frontier['full_move_best_agreements']}/"
            f"{oracle_frontier['accepted']} positions. The hindsight "
            f"direct-3-ply best frontier nominee minus the full 2-ply move was "
            f"{format_ci(oracle_frontier['best_minus_full']['utility'])} "
            f"utility, "
            f"{format_ci(oracle_frontier['best_minus_full']['win'], 100.0)} "
            f"win percentage points, and "
            f"{format_ci(oracle_frontier['best_minus_full']['spread'])} "
            "spread. This is a diagnostic upper bound, not a deployable "
            "stopping policy.",
            "",
            f"Exact running-best self-utility ties occurred at "
            f"{len(frontier['self_utility_checkpoint_ties'])} candidate "
            "boundaries; they are reported as tie diagnostics rather than "
            "evidence of additional quality.",
            "",
            "## Two-candidate stage admission",
            "",
            "PEG cannot compare a new stage until two candidates complete; "
            "work below that boundary is discarded. The incremental cost from "
            "the greedy boundary through two completed 2-ply candidates was:",
            "",
            "| Bag | Scenarios, median / p90 | Nested nodes, median / p90 | "
            "Local wall seconds, median / p90 | Occupancy, median |",
            "| ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for bag in range(1, 5):
        row = admission["by_bag"][str(bag)]
        lines.append(
            f"| {bag} | {row['cumulative_scenarios']['median']:.0f} / "
            f"{row['cumulative_scenarios']['p90']:.0f} | "
            f"{row['nested_endgame_nodes']['median']:.0f} / "
            f"{row['nested_endgame_nodes']['p90']:.0f} | "
            f"{row['completed_seconds']['median']:.2f} / "
            f"{row['completed_seconds']['p90']:.2f} | "
            f"{row['scheduled_core_occupancy']['median']:.3f} |"
        )
    overall_admission = admission["overall"]
    lines.extend(
        [
            "",
            f"Overall two-candidate cost: median/p90 "
            f"{overall_admission['nested_endgame_nodes']['median']:.0f}/"
            f"{overall_admission['nested_endgame_nodes']['p90']:.0f} nested "
            f"nodes and "
            f"{overall_admission['completed_seconds']['median']:.2f}/"
            f"{overall_admission['completed_seconds']['p90']:.2f} local "
            "seconds. A future admission guard should reserve a conservative "
            "two-candidate envelope before starting the stage; this analysis "
            "does not change live policy.",
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

#!/usr/bin/env python3
"""Analyze paired 2-ply/3-ply PEG halving-schedule sweeps."""

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
from run_peg_stage_cap_calibration import SCHEDULES  # noqa: E402


METRICS = ("utility", "win", "spread")
WORK_FIELDS = (
    "incremental_scenarios",
    "incremental_nested_endgame_nodes",
    "incremental_seconds",
    "incremental_process_cpu_seconds",
)
CAP_LABELS = tuple(SCHEDULES)
ENDPOINT_LABELS = (
    "greedy",
    *(
        f"{label}_{fidelity}"
        for label in CAP_LABELS
        for fidelity in ("2ply", "3ply")
    ),
)


def load_records(path: pathlib.Path) -> list[dict[str, Any]]:
    return [
        json.loads(line)
        for line in path.read_text().splitlines()
        if line.strip()
    ]


def zero_observation(position: str, bag: int) -> dict[str, Any]:
    return {
        "position": position,
        "bag": bag,
        "utility": 0.0,
        "win": 0.0,
        "spread": 0.0,
    }


def comparison(
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
            metric: float(values[after][metric])
            - float(values[before][metric])
            for metric in METRICS
        },
    }


def summaries(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        metric: stratified_mean_ci(rows, metric) for metric in METRICS
    }


def describe_work(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        field: describe([float(row[field]) for row in rows if field in row])
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


def endpoint_index(stage_map: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(endpoint["label"]): endpoint
        for endpoint in stage_map["endpoints"]
    }


def accepted_endpoint(
    endpoints: dict[str, dict[str, Any]], label: str
) -> dict[str, Any] | None:
    endpoint = endpoints.get(label)
    if endpoint is None or not int(endpoint.get("accepted", 0)):
        return None
    return endpoint


def latest_judges(
    records: list[dict[str, Any]],
) -> dict[tuple[str, str], dict[str, Any]]:
    return latest_by(
        (record for record in records if record.get("kind") == "judge"),
        lambda record: (str(record["position"]), str(record["label"])),
    )


def analyze(records: list[dict[str, Any]]) -> dict[str, Any]:
    maps = latest_by(
        (
            record
            for record in records
            if record.get("kind") == "stage_cap_map"
        ),
        lambda record: (str(record["position"]),),
    )
    arms = latest_by(
        (
            record
            for record in records
            if record.get("kind") == "arm"
            and record.get("mode") == "arm"
        ),
        lambda record: (str(record["position"]), str(record["label"])),
    )
    judges = latest_judges(records)
    values_by_judge = value_maps(records, judges)
    positions = sorted(position for (position,) in maps)
    endpoint_maps = {
        position: endpoint_index(maps[(position,)]) for position in positions
    }

    endpoint_results = {}
    for label in ENDPOINT_LABELS:
        available = 0
        accepted = 0
        disagreements = 0
        gain_rows = []
        conditional_rows = []
        work_rows = []
        censored_quality = 0
        by_bag = {
            str(bag): {"available": 0, "accepted": 0} for bag in range(1, 5)
        }
        for position in positions:
            endpoints = endpoint_maps[position]
            endpoint = endpoints.get(label)
            if endpoint is None:
                continue
            available += 1
            bag = int(maps[(position,)]["bag"])
            by_bag[str(bag)]["available"] += 1
            if not int(endpoint.get("accepted", 0)):
                continue
            accepted += 1
            by_bag[str(bag)]["accepted"] += 1
            greedy = accepted_endpoint(endpoints, "greedy")
            assert greedy is not None
            move = str(endpoint["move"])
            greedy_move = str(greedy["move"])
            if move != greedy_move:
                disagreements += 1
            values = values_by_judge.get((position, "stagecap_stride4"))
            row = comparison(
                position=position,
                bag=bag,
                before=greedy_move,
                after=move,
                values=values,
            )
            if row is None:
                censored_quality += 1
            else:
                gain_rows.append(row)
                if move != greedy_move:
                    conditional_rows.append(row)
            if all(field in endpoint for field in WORK_FIELDS):
                work_rows.append(endpoint)
        endpoint_results[label] = {
            "available": available,
            "accepted": accepted,
            "censored_stage": available - accepted,
            "censored_quality": censored_quality,
            "completion_by_bag": by_bag,
            "disagreement_with_greedy": wilson_interval(
                disagreements, accepted
            ),
            "gain_vs_greedy": summaries(gain_rows),
            "conditional_gain_vs_greedy": summaries(conditional_rows),
            "stage_work": describe_work(work_rows),
        }

    ply3_increment = {}
    for cap_label in CAP_LABELS:
        label2 = f"{cap_label}_2ply"
        label3 = f"{cap_label}_3ply"
        paired = 0
        disagreements = 0
        rows = []
        conditional_rows = []
        work_rows = []
        censored_quality = 0
        by_bag_rows = {str(bag): [] for bag in range(1, 5)}
        for position in positions:
            endpoints = endpoint_maps[position]
            endpoint2 = accepted_endpoint(endpoints, label2)
            endpoint3 = accepted_endpoint(endpoints, label3)
            if endpoint2 is None or endpoint3 is None:
                continue
            paired += 1
            bag = int(maps[(position,)]["bag"])
            move2 = str(endpoint2["move"])
            move3 = str(endpoint3["move"])
            if move2 != move3:
                disagreements += 1
            values = values_by_judge.get((position, "stagecap_stride4"))
            row = comparison(
                position=position,
                bag=bag,
                before=move2,
                after=move3,
                values=values,
            )
            if row is None:
                censored_quality += 1
            else:
                rows.append(row)
                by_bag_rows[str(bag)].append(row)
                if move2 != move3:
                    conditional_rows.append(row)
            if all(field in endpoint3 for field in WORK_FIELDS):
                work_rows.append(endpoint3)
        ply3_increment[cap_label] = {
            "paired_completed": paired,
            "censored_quality": censored_quality,
            "disagreement": wilson_interval(disagreements, paired),
            "gain_3ply_minus_2ply": summaries(rows),
            "conditional_disagreement_gain": summaries(conditional_rows),
            "gain_by_bag": {
                bag: summaries(bag_rows)
                for bag, bag_rows in by_bag_rows.items()
            },
            "stage_work": describe_work(work_rows),
        }

    cap_marginals = {}
    cap_pairs = tuple(zip(CAP_LABELS, CAP_LABELS[1:]))
    for fidelity in ("2ply", "3ply"):
        for smaller, larger in cap_pairs:
            comparison_label = f"{smaller}_to_{larger}_{fidelity}"
            rows = []
            conditional_rows = []
            paired = 0
            disagreements = 0
            censored_quality = 0
            for position in positions:
                endpoints = endpoint_maps[position]
                before_endpoint = accepted_endpoint(
                    endpoints, f"{smaller}_{fidelity}"
                )
                after_endpoint = accepted_endpoint(
                    endpoints, f"{larger}_{fidelity}"
                )
                if before_endpoint is None or after_endpoint is None:
                    continue
                paired += 1
                bag = int(maps[(position,)]["bag"])
                before_move = str(before_endpoint["move"])
                after_move = str(after_endpoint["move"])
                if before_move != after_move:
                    disagreements += 1
                values = values_by_judge.get(
                    (position, "stagecap_stride4")
                )
                row = comparison(
                    position=position,
                    bag=bag,
                    before=before_move,
                    after=after_move,
                    values=values,
                )
                if row is None:
                    censored_quality += 1
                else:
                    rows.append(row)
                    if before_move != after_move:
                        conditional_rows.append(row)
            cap_marginals[comparison_label] = {
                "paired_completed": paired,
                "censored_quality": censored_quality,
                "disagreement": wilson_interval(disagreements, paired),
                "gain": summaries(rows),
                "conditional_disagreement_gain": summaries(
                    conditional_rows
                ),
            }

    arm_completion = {}
    for label in CAP_LABELS:
        rows = [
            arm
            for (position, arm_label), arm in arms.items()
            if arm_label == label and position in positions
        ]
        arm_completion[label] = {
            "attempted": len(rows),
            "completed_3ply": sum(
                str(row.get("status")) == "completed"
                and int(row.get("last_completed_stage", -1)) == 2
                for row in rows
            ),
            "partial": sum(str(row.get("status")) == "partial" for row in rows),
            "returned_2ply": sum(
                int(row.get("last_completed_stage", -1)) == 1
                for row in rows
            ),
            "wall_seconds": describe(
                [float(row["wall_seconds"]) for row in rows]
            ),
            "process_cpu_seconds": describe(
                [float(row["process_cpu_seconds"]) for row in rows]
            ),
            "scheduled_core_occupancy": describe(
                [float(row["scheduled_core_occupancy"]) for row in rows]
            ),
        }

    event_index: dict[tuple[int, int], list[dict[str, Any]]] = {}
    for record in records:
        if record.get("kind") != "event":
            continue
        key = (int(record["sequence"]), int(record["stage"]))
        event_index.setdefault(key, []).append(record)
    for events in event_index.values():
        events.sort(key=lambda event: int(event["rank"]))

    admission_fields = (
        "scenarios",
        "nested_endgame_nodes",
        "seconds",
        "process_cpu_seconds",
        "scheduled_core_occupancy",
    )
    stage3_admission = {}
    for label in CAP_LABELS:
        rows = []
        for position in positions:
            stage_map = maps[(position,)]
            sequence = stage_map.get("arm_sequences", {}).get(label)
            if sequence is None:
                continue
            events1 = event_index.get((int(sequence), 1), [])
            events2 = event_index.get((int(sequence), 2), [])
            if not events1 or len(events2) < 2:
                continue
            before = max(
                events1,
                key=lambda event: float(event["completed_seconds"]),
            )
            after = max(
                events2[:2],
                key=lambda event: float(event["completed_seconds"]),
            )
            seconds = float(after["completed_seconds"]) - float(
                before["completed_seconds"]
            )
            cpu = float(after["process_cpu_seconds"]) - float(
                before["process_cpu_seconds"]
            )
            rows.append(
                {
                    "position": position,
                    "bag": int(stage_map["bag"]),
                    "scenarios": int(after["cumulative_scenarios"])
                    - int(before["cumulative_scenarios"]),
                    "nested_endgame_nodes": int(
                        after["nested_endgame_nodes"]
                    )
                    - int(before["nested_endgame_nodes"]),
                    "seconds": seconds,
                    "process_cpu_seconds": cpu,
                    "scheduled_core_occupancy": (
                        cpu / (seconds * 10.0) if seconds > 0 else 0.0
                    ),
                }
            )
        stage3_admission[label] = {
            "minimum_completed_candidates": 2,
            "observations": len(rows),
            "overall": {
                field: describe_quantiles(
                    [float(row[field]) for row in rows]
                )
                for field in admission_fields
            },
            "by_bag": {
                str(bag): {
                    field: describe_quantiles(
                        [
                            float(row[field])
                            for row in rows
                            if int(row["bag"]) == bag
                        ]
                    )
                    for field in admission_fields
                }
                for bag in range(1, 5)
            },
        }

    judge_completion = {}
    judge_strength = {}
    for label in ("stagecap_stride4", "stagecap_stride2"):
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
            field: describe([float(row.get(field, 0.0)) for row in rows])
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
        values4 = values_by_judge.get((position, "stagecap_stride4"))
        values2 = values_by_judge.get((position, "stagecap_stride2"))
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
        "best_disagreements": [
            row for row in sensitivity_positions if not row["agrees"]
        ],
        **{
            f"{metric}_abs_diff": describe(
                [float(row[metric]) for row in sensitivity_nominees]
            )
            for metric in METRICS
        },
    }
    sensitivity_ply3_increment = {}
    for cap_label in CAP_LABELS:
        rows4 = []
        rows2 = []
        paired_differences = []
        for position in positions:
            endpoints = endpoint_maps[position]
            endpoint2 = accepted_endpoint(
                endpoints, f"{cap_label}_2ply"
            )
            endpoint3 = accepted_endpoint(
                endpoints, f"{cap_label}_3ply"
            )
            values4 = values_by_judge.get(
                (position, "stagecap_stride4")
            )
            values2 = values_by_judge.get(
                (position, "stagecap_stride2")
            )
            if (
                endpoint2 is None
                or endpoint3 is None
                or values4 is None
                or values2 is None
            ):
                continue
            bag = int(maps[(position,)]["bag"])
            move2 = str(endpoint2["move"])
            move3 = str(endpoint3["move"])
            row4 = comparison(
                position=position,
                bag=bag,
                before=move2,
                after=move3,
                values=values4,
            )
            row2 = comparison(
                position=position,
                bag=bag,
                before=move2,
                after=move3,
                values=values2,
            )
            if row4 is None or row2 is None:
                continue
            rows4.append(row4)
            rows2.append(row2)
            paired_differences.append(
                {
                    "position": position,
                    "bag": bag,
                    **{
                        metric: float(row2[metric])
                        - float(row4[metric])
                        for metric in METRICS
                    },
                }
            )
        sensitivity_ply3_increment[cap_label] = {
            "stride4": summaries(rows4),
            "stride2": summaries(rows2),
            "stride2_minus_stride4": summaries(paired_differences),
        }
    sensitivity["ply3_increment"] = sensitivity_ply3_increment

    sentinel_records = [
        record
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "sentinel"
    ]
    arm_records = [
        record
        for record in records
        if record.get("kind") == "arm" and record.get("mode") == "arm"
    ]
    monitoring = summarize_sentinel_run(sentinel_records, arm_records)

    return {
        "position_count": len(positions),
        "bag_counts": {
            str(bag): sum(
                int(maps[(position,)]["bag"]) == bag
                for position in positions
            )
            for bag in range(1, 5)
        },
        "arm_completion": arm_completion,
        "endpoint_results": endpoint_results,
        "ply3_increment": ply3_increment,
        "cap_marginals": cap_marginals,
        "stage3_admission": stage3_admission,
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
    lines = [
        "# PEG 2-ply/3-ply stage-cap sweep",
        "",
        f"Positions: {result['position_count']}; bag counts: "
        f"{result['bag_counts']}. Utility is `win + 1e-4 * spread`. "
        "Intervals are deterministic bag-stratified bootstrap 95% CIs; "
        "rates use Wilson 95% CIs.",
        "",
        "## Arm and judge completion",
        "",
        "| Schedule | Attempted | Completed through 3 ply | Partial | "
        "Median wall s | Median occupancy |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for label in CAP_LABELS:
        row = result["arm_completion"][label]
        lines.append(
            f"| {SCHEDULES[label]} | {row['attempted']} | "
            f"{row['completed_3ply']} | {row['partial']} | "
            f"{row['wall_seconds']['median']:.2f} | "
            f"{row['scheduled_core_occupancy']['median']:.3f} |"
        )
    lines.extend(
        [
            "",
            "| Judge | Accepted/attempted | Censored | Median wall s | "
            "Median nested nodes |",
            "| --- | ---: | ---: | ---: | ---: |",
        ]
    )
    for label in ("stagecap_stride4", "stagecap_stride2"):
        completion = result["judge_completion"][label]
        strength = result["judge_strength"][label]
        lines.append(
            f"| {label} | {completion['accepted']}/"
            f"{completion['attempted']} | {completion['censored']} | "
            f"{strength['wall_seconds']['median']:.2f} | "
            f"{strength['nested_endgame_nodes']['median']:.0f} |"
        )

    lines.extend(
        [
            "",
            "## Two-candidate admission cost for the 3-ply stage",
            "",
            "Incremental work from the completed 2-ply boundary through two "
            "completed 3-ply candidates:",
            "",
            "| Schedule | Observations | Scenarios median / p90 | "
            "Nested nodes median / p90 | Local wall s median / p90 |",
            "| --- | ---: | ---: | ---: | ---: |",
        ]
    )
    for label in CAP_LABELS:
        row = result["stage3_admission"][label]
        overall = row["overall"]
        lines.append(
            f"| {SCHEDULES[label]} | {row['observations']} | "
            f"{overall['scenarios']['median']:.0f} / "
            f"{overall['scenarios']['p90']:.0f} | "
            f"{overall['nested_endgame_nodes']['median']:.0f} / "
            f"{overall['nested_endgame_nodes']['p90']:.0f} | "
            f"{overall['seconds']['median']:.2f} / "
            f"{overall['seconds']['p90']:.2f} |"
        )
    lines.extend(
        [
            "",
            "For the smallest schedule by bag:",
            "",
            "| Bag | Scenarios median / p90 | Nested nodes median / p90 | "
            "Local wall s median / p90 / max |",
            "| ---: | ---: | ---: | ---: |",
        ]
    )
    for bag in range(1, 5):
        row = result["stage3_admission"]["cap16"]["by_bag"][str(bag)]
        lines.append(
            f"| {bag} | {row['scenarios']['median']:.0f} / "
            f"{row['scenarios']['p90']:.0f} | "
            f"{row['nested_endgame_nodes']['median']:.0f} / "
            f"{row['nested_endgame_nodes']['p90']:.0f} | "
            f"{row['seconds']['median']:.2f} / "
            f"{row['seconds']['p90']:.2f} / "
            f"{row['seconds']['max']:.2f} |"
        )

    lines.extend(
        [
            "",
            "## Endpoint value versus greedy",
            "",
            "| Endpoint | Completed | Disagree with greedy | Utility gain | "
            "Win gain, pp | Spread gain | Median stage nodes |",
            "| --- | ---: | --- | --- | --- | --- | ---: |",
        ]
    )
    for label in ENDPOINT_LABELS:
        if label == "greedy":
            continue
        row = result["endpoint_results"][label]
        lines.append(
            f"| {label} | {row['accepted']}/{row['available']} | "
            f"{format_rate(row['disagreement_with_greedy'])} | "
            f"{format_ci(row['gain_vs_greedy']['utility'])} | "
            f"{format_ci(row['gain_vs_greedy']['win'], 100.0)} | "
            f"{format_ci(row['gain_vs_greedy']['spread'])} | "
            f"{row['stage_work']['incremental_nested_endgame_nodes']['median']:.0f} |"
        )

    lines.extend(
        [
            "",
            "## Incremental value of completing 3 ply",
            "",
            "| Schedule | Paired completed | 3-ply disagreement | "
            "3-ply minus 2-ply utility | Win gain, pp | Spread gain | "
            "Median 3-ply stage nodes |",
            "| --- | ---: | --- | --- | --- | --- | ---: |",
        ]
    )
    for label in CAP_LABELS:
        row = result["ply3_increment"][label]
        lines.append(
            f"| {SCHEDULES[label]} | {row['paired_completed']} | "
            f"{format_rate(row['disagreement'])} | "
            f"{format_ci(row['gain_3ply_minus_2ply']['utility'])} | "
            f"{format_ci(row['gain_3ply_minus_2ply']['win'], 100.0)} | "
            f"{format_ci(row['gain_3ply_minus_2ply']['spread'])} | "
            f"{row['stage_work']['incremental_nested_endgame_nodes']['median']:.0f} |"
        )

    lines.extend(
        [
            "",
            "Conditional gains when 3 ply changed the move:",
            "",
            "| Schedule | Conditional utility | Conditional win, pp | "
            "Conditional spread |",
            "| --- | --- | --- | --- |",
        ]
    )
    for label in CAP_LABELS:
        row = result["ply3_increment"][label][
            "conditional_disagreement_gain"
        ]
        lines.append(
            f"| {SCHEDULES[label]} | {format_ci(row['utility'])} | "
            f"{format_ci(row['win'], 100.0)} | "
            f"{format_ci(row['spread'])} |"
        )

    lines.extend(
        [
            "",
            "## Marginal value of a larger initial cap",
            "",
            "| Comparison | Paired completed | Disagreement | Utility gain | "
            "Win gain, pp | Spread gain |",
            "| --- | ---: | --- | --- | --- | --- |",
        ]
    )
    for label, row in result["cap_marginals"].items():
        lines.append(
            f"| {label} | {row['paired_completed']} | "
            f"{format_rate(row['disagreement'])} | "
            f"{format_ci(row['gain']['utility'])} | "
            f"{format_ci(row['gain']['win'], 100.0)} | "
            f"{format_ci(row['gain']['spread'])} |"
        )

    sensitivity = result["sensitivity"]
    monitoring = result["monitoring"]
    lines.extend(
        [
            "",
            "## Sensitivity and load",
            "",
            f"Stride-2/stride-4 best-nominee agreement: "
            f"{sensitivity['best_nominee_agreement_rate']:.3f} across "
            f"{sensitivity['accepted_positions']} positions.",
            "",
            "| Schedule | Stride-4 3-ply increment | "
            "Stride-2 3-ply increment | Stride-2 minus stride-4 |",
            "| --- | --- | --- | --- |",
        ]
    )
    for label in CAP_LABELS:
        row = sensitivity["ply3_increment"][label]
        lines.append(
            f"| {SCHEDULES[label]} | "
            f"{format_ci(row['stride4']['utility'])} | "
            f"{format_ci(row['stride2']['utility'])} | "
            f"{format_ci(row['stride2_minus_stride4']['utility'])} |"
        )
    lines.extend(
        [
            "",
            f"Sentinel normalized-throughput CV: "
            f"{monitoring['normalized_throughput_cv']:.3f}; early/late "
            f"flags: {monitoring['segment_noise']}. Candidate/stage "
            "completion is the portable boundary; local seconds are only a "
            "machine-specific conversion.",
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

    result = analyze(
        [
            record
            for records_path in args.records
            for record in load_records(records_path)
        ]
    )
    json_text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    markdown_text = render_markdown(result)
    if args.json:
        args.json.write_text(json_text)
    else:
        print(json_text, end="")
    if args.markdown:
        args.markdown.write_text(markdown_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

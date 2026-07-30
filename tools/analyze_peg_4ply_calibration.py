#!/usr/bin/env python3
"""Analyze PEG narrow/wide 4-ply calibration records."""

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
from run_peg_4ply_calibration import SCHEDULES  # noqa: E402


METRICS = ("utility", "win", "spread")
ARM_LABELS = tuple(SCHEDULES)
ENDPOINT_LABELS = (
    "greedy",
    *(
        f"{arm}_{ply}ply"
        for arm in ARM_LABELS
        for ply in (2, 3, 4)
    ),
)
COMPARISONS = {
    "narrow_2ply_to_3ply": ("narrow4_2ply", "narrow4_3ply"),
    "narrow_3ply_to_4ply": ("narrow4_3ply", "narrow4_4ply"),
    "wide_2ply_to_3ply": ("wide4_2ply", "wide4_3ply"),
    "wide_3ply_to_4ply": ("wide4_3ply", "wide4_4ply"),
    "narrow_4ply_to_wide_3ply": (
        "narrow4_4ply",
        "wide4_3ply",
    ),
    "width_at_2ply": ("narrow4_2ply", "wide4_2ply"),
    "width_at_3ply": ("narrow4_3ply", "wide4_3ply"),
    "width_at_4ply": ("narrow4_4ply", "wide4_4ply"),
}
WORK_FIELDS = (
    "incremental_scenarios",
    "incremental_nested_endgame_nodes",
    "incremental_seconds",
    "incremental_process_cpu_seconds",
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


def compare_moves(
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


def summarize_quality(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        metric: stratified_mean_ci(rows, metric) for metric in METRICS
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


def analyze(records: list[dict[str, Any]]) -> dict[str, Any]:
    maps = latest_by(
        (record for record in records if record.get("kind") == "peg4_map"),
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
    judges = latest_by(
        (record for record in records if record.get("kind") == "judge"),
        lambda record: (str(record["position"]), str(record["label"])),
    )
    values_by_judge = value_maps(records, judges)
    positions = sorted(position for (position,) in maps)
    endpoint_maps = {
        position: endpoint_index(maps[(position,)]) for position in positions
    }

    def adjudicated_values(
        position: str,
    ) -> dict[str, dict[str, float]] | None:
        # Full enumeration adjudicates mandatory stride disagreements (and the
        # deliberately coarse bag-1 sample). Keep stride 4 elsewhere as the
        # common primary sample.
        return values_by_judge.get(
            (position, "direct4_stride1")
        ) or values_by_judge.get((position, "direct4_stride4"))

    endpoint_results = {}
    for label in ENDPOINT_LABELS:
        available = accepted = disagreements = censored_quality = 0
        rows = []
        conditional_rows = []
        work_rows = []
        for position in positions:
            endpoints = endpoint_maps[position]
            endpoint = endpoints.get(label)
            if endpoint is None:
                continue
            available += 1
            if not int(endpoint.get("accepted", 0)):
                continue
            accepted += 1
            if label == "greedy":
                row = zero_observation(
                    position, int(maps[(position,)]["bag"])
                )
            else:
                greedy = accepted_endpoint(endpoints, "greedy")
                assert greedy is not None
                before = str(greedy["move"])
                after = str(endpoint["move"])
                if before != after:
                    disagreements += 1
                row = compare_moves(
                    position=position,
                    bag=int(maps[(position,)]["bag"]),
                    before=before,
                    after=after,
                    values=adjudicated_values(position),
                )
                if row is None:
                    censored_quality += 1
                    continue
                if before != after:
                    conditional_rows.append(row)
            rows.append(row)
            work_rows.append(endpoint)
        endpoint_results[label] = {
            "available": available,
            "accepted": accepted,
            "censored_quality": censored_quality,
            "disagreement_vs_greedy": wilson_interval(
                disagreements, accepted
            ),
            "gain_vs_greedy": summarize_quality(rows),
            "conditional_disagreement_gain": summarize_quality(
                conditional_rows
            ),
            "stage_work": {
                field: describe(
                    [
                        float(row[field])
                        for row in work_rows
                        if field in row
                    ]
                )
                for field in WORK_FIELDS
            },
        }

    def build_comparisons(
        values_for_position,
    ) -> dict[str, dict[str, Any]]:
        result = {}
        for name, (before_label, after_label) in COMPARISONS.items():
            paired = disagreements = censored_quality = 0
            rows = []
            conditional_rows = []
            for position in positions:
                endpoints = endpoint_maps[position]
                before = accepted_endpoint(endpoints, before_label)
                after = accepted_endpoint(endpoints, after_label)
                if before is None or after is None:
                    continue
                paired += 1
                before_move = str(before["move"])
                after_move = str(after["move"])
                if before_move != after_move:
                    disagreements += 1
                row = compare_moves(
                    position=position,
                    bag=int(maps[(position,)]["bag"]),
                    before=before_move,
                    after=after_move,
                    values=values_for_position(position),
                )
                if row is None:
                    censored_quality += 1
                    continue
                rows.append(row)
                if before_move != after_move:
                    conditional_rows.append(row)
            result[name] = {
                "paired_completed": paired,
                "censored_quality": censored_quality,
                "disagreement": wilson_interval(disagreements, paired),
                "gain": summarize_quality(rows),
                "gain_by_bag": {
                    str(bag): summarize_quality(
                        [
                            row
                            for row in rows
                            if int(row["bag"]) == bag
                        ]
                    )
                    for bag in range(1, 5)
                },
                "conditional_disagreement_gain": summarize_quality(
                    conditional_rows
                ),
            }
        return result

    comparisons = build_comparisons(adjudicated_values)
    comparisons_stride4 = build_comparisons(
        lambda position: values_by_judge.get(
            (position, "direct4_stride4")
        )
    )

    arm_completion = {}
    for label in ARM_LABELS:
        rows = [
            arm
            for (position, arm_label), arm in arms.items()
            if arm_label == label and position in positions
        ]
        arm_completion[label] = {
            "attempted": len(rows),
            "completed_4ply": sum(
                str(row.get("status")) == "completed"
                and int(row.get("last_completed_stage", -1)) == 3
                for row in rows
            ),
            "partial": sum(str(row.get("status")) == "partial" for row in rows),
            "returned_3ply_or_less": sum(
                int(row.get("last_completed_stage", -1)) < 3
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

    event_index: dict[
        tuple[str, int, int], list[dict[str, Any]]
    ] = {}
    for record in records:
        if record.get("kind") != "event":
            continue
        key = (
            str(record["position"]),
            int(record["sequence"]),
            int(record["stage"]),
        )
        event_index.setdefault(key, []).append(record)
    for events in event_index.values():
        events.sort(key=lambda event: float(event["completed_seconds"]))

    admission_fields = (
        "scenarios",
        "nested_endgame_nodes",
        "seconds",
        "process_cpu_seconds",
        "scheduled_core_occupancy",
    )
    stage4_admission = {}
    for label in ARM_LABELS:
        rows = []
        started_below_two = 0
        for position in positions:
            stage_map = maps[(position,)]
            sequence = stage_map.get("arm_sequences", {}).get(label)
            if sequence is None:
                continue
            events2 = event_index.get(
                (position, int(sequence), 2), []
            )
            events3 = event_index.get(
                (position, int(sequence), 3), []
            )
            if events3 and len(events3) < 2:
                started_below_two += 1
            if not events2 or len(events3) < 2:
                continue
            before = events2[-1]
            after = events3[1]
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
        stage4_admission[label] = {
            "minimum_completed_candidates": 2,
            "observations": len(rows),
            "started_but_fewer_than_two_completed": started_below_two,
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
    for label in (
        "direct4_stride4",
        "direct4_stride2",
        "direct4_stride1",
    ):
        rows = [
            judge
            for (_, judge_label), judge in judges.items()
            if judge_label == label
        ]
        judge_completion[label] = {
            "attempted": len(rows),
            "accepted": sum(int(row.get("accepted", 0)) == 1 for row in rows),
            "censored": sum(int(row.get("accepted", 0)) != 1 for row in rows),
            "wall_seconds": describe(
                [float(row.get("wall_seconds", 0.0)) for row in rows]
            ),
        }

    sensitivity_positions = []
    best_agreements = 0
    sensitivity_rows = {
        name: {"stride4": [], "stride2": [], "stride2_minus_stride4": []}
        for name in COMPARISONS
    }
    for position in positions:
        values4 = values_by_judge.get((position, "direct4_stride4"))
        values2 = values_by_judge.get((position, "direct4_stride2"))
        if values4 is None or values2 is None:
            continue
        sensitivity_positions.append(position)
        best4 = max(
            values4,
            key=lambda move: (
                values4[move]["utility"],
                values4[move]["win"],
                values4[move]["spread"],
                move,
            ),
        )
        best2 = max(
            values2,
            key=lambda move: (
                values2[move]["utility"],
                values2[move]["win"],
                values2[move]["spread"],
                move,
            ),
        )
        best_agreements += int(best4 == best2)
        endpoints = endpoint_maps[position]
        bag = int(maps[(position,)]["bag"])
        for name, (before_label, after_label) in COMPARISONS.items():
            before = accepted_endpoint(endpoints, before_label)
            after = accepted_endpoint(endpoints, after_label)
            if before is None or after is None:
                continue
            before_move = str(before["move"])
            after_move = str(after["move"])
            row4 = compare_moves(
                position=position,
                bag=bag,
                before=before_move,
                after=after_move,
                values=values4,
            )
            row2 = compare_moves(
                position=position,
                bag=bag,
                before=before_move,
                after=after_move,
                values=values2,
            )
            if row4 is None or row2 is None:
                continue
            sensitivity_rows[name]["stride4"].append(row4)
            sensitivity_rows[name]["stride2"].append(row2)
            sensitivity_rows[name]["stride2_minus_stride4"].append(
                {
                    "position": position,
                    "bag": bag,
                    **{
                        metric: float(row2[metric]) - float(row4[metric])
                        for metric in METRICS
                    },
                }
            )
    sensitivity = {
        "accepted_positions": len(sensitivity_positions),
        "positions": sensitivity_positions,
        "best_nominee_agreements": best_agreements,
        "best_nominee_agreement_rate": (
            best_agreements / len(sensitivity_positions)
            if sensitivity_positions
            else math.nan
        ),
        "comparisons": {
            name: {
                sample: summarize_quality(rows)
                for sample, rows in samples.items()
            }
            for name, samples in sensitivity_rows.items()
        },
    }

    full_enumeration = []
    for position in positions:
        values1 = values_by_judge.get((position, "direct4_stride1"))
        if values1 is None:
            continue
        values4 = values_by_judge.get((position, "direct4_stride4"))
        values2 = values_by_judge.get((position, "direct4_stride2"))
        best = {}
        for label, values in (
            ("stride4", values4),
            ("stride2", values2),
            ("stride1", values1),
        ):
            if values is None:
                continue
            best[label] = max(
                values,
                key=lambda move: (
                    values[move]["utility"],
                    values[move]["win"],
                    values[move]["spread"],
                    move,
                ),
            )
        comparison_rows = {}
        endpoints = endpoint_maps[position]
        for name, (before_label, after_label) in COMPARISONS.items():
            before = accepted_endpoint(endpoints, before_label)
            after = accepted_endpoint(endpoints, after_label)
            if before is None or after is None:
                continue
            row = compare_moves(
                position=position,
                bag=int(maps[(position,)]["bag"]),
                before=str(before["move"]),
                after=str(after["move"]),
                values=values1,
            )
            if row is not None:
                comparison_rows[name] = {
                    metric: float(row[metric]) for metric in METRICS
                }
        full_enumeration.append(
            {
                "position": position,
                "bag": int(maps[(position,)]["bag"]),
                "best_nominee": best,
                "comparisons": comparison_rows,
            }
        )

    sentinels = [
        record
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "sentinel"
        and record.get("position") == "peg4-sentinel"
    ]
    monitoring = (
        summarize_sentinel_run(
            sentinels,
            [
                arm
                for (position, _), arm in arms.items()
                if position in positions
            ],
        )
        if sentinels
        else {
            "normalized_throughput_cv": math.nan,
            "early_to_late_drift": math.nan,
            "segment_noise": {},
        }
    )

    return {
        "position_count": len(positions),
        "bag_counts": {
            str(bag): sum(
                int(maps[(position,)]["bag"]) == bag
                for position in positions
            )
            for bag in range(1, 5)
        },
        "endpoint_results": endpoint_results,
        "comparisons": comparisons,
        "comparisons_stride4_only": comparisons_stride4,
        "arm_completion": arm_completion,
        "stage4_admission": stage4_admission,
        "judge_completion": judge_completion,
        "sensitivity": sensitivity,
        "full_enumeration": full_enumeration,
        "monitoring": monitoring,
    }


def format_ci(summary: dict[str, Any], scale: float = 1.0) -> str:
    if int(summary.get("n", 0)) == 0:
        return "n/a"
    return (
        f"{float(summary['mean']) * scale:+.4f} "
        f"[{float(summary['ci_low']) * scale:+.4f}, "
        f"{float(summary['ci_high']) * scale:+.4f}]"
    )


def format_rate(summary: dict[str, Any]) -> str:
    return (
        f"{int(summary['successes'])}/{int(summary['n'])} = "
        f"{float(summary['rate']):.3f}"
    )


def render_markdown(result: dict[str, Any]) -> str:
    lines = [
        "# PEG 4-ply depth-versus-width calibration",
        "",
        f"Positions with endpoint maps: {result['position_count']}.",
        "",
        "## Completion",
        "",
        "| Schedule | Attempted | Completed 4 ply | Partial | "
        "Median wall seconds |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for label in ARM_LABELS:
        row = result["arm_completion"][label]
        lines.append(
            f"| `{SCHEDULES[label]}` | {row['attempted']} | "
            f"{row['completed_4ply']} | {row['partial']} | "
            f"{row['wall_seconds']['median']:.2f} |"
        )
    lines.extend(
        [
            "",
            "## Direct-4-ply value",
            "",
            "The primary table uses the common stride-4 judge except where a "
            "completed full-enumeration judge adjudicates a mandatory stride "
            "disagreement or an intentionally coarse small-bag sample.",
            "",
            "| Comparison | Paired | Move changes | Utility (95% CI) | "
            "Win pp (95% CI) | Spread (95% CI) |",
            "| --- | ---: | --- | --- | --- | --- |",
        ]
    )
    for name, row in result["comparisons"].items():
        lines.append(
            f"| {name} | {row['paired_completed']} | "
            f"{format_rate(row['disagreement'])} | "
            f"{format_ci(row['gain']['utility'])} | "
            f"{format_ci(row['gain']['win'], 100.0)} | "
            f"{format_ci(row['gain']['spread'])} |"
        )
    stride4_depth = result["comparisons_stride4_only"][
        "narrow_3ply_to_4ply"
    ]["gain"]["utility"]
    lines.extend(
        [
            "",
            "For transparency, the unadjudicated all-stride-4 narrow 3→4 "
            f"utility was {format_ci(stride4_depth)}.",
        ]
    )
    for row in result["full_enumeration"]:
        depth = row["comparisons"].get("narrow_3ply_to_4ply")
        if depth is None:
            continue
        lines.append(
            f"Full enumeration at `{row['position']}` selected "
            f"`{row['best_nominee'].get('stride1')}` and valued narrow "
            f"3→4 at {depth['utility']:+.6f} utility."
        )
    lines.extend(
        [
            "",
            "Narrow 3→4 mean utility by bag:",
            "",
            "| Bag | Utility (95% CI) |",
            "| ---: | --- |",
        ]
    )
    depth_by_bag = result["comparisons"]["narrow_3ply_to_4ply"][
        "gain_by_bag"
    ]
    for bag in range(1, 5):
        lines.append(
            f"| {bag} | "
            f"{format_ci(depth_by_bag[str(bag)]['utility'])} |"
        )
    lines.extend(
        [
            "",
            "## Two-candidate 4-ply admission boundary",
            "",
            "| Schedule | Observations | Started below two | "
            "Scenarios median / p90 | Nodes median / p90 | "
            "Local seconds median / p90 |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for label in ARM_LABELS:
        row = result["stage4_admission"][label]
        overall = row["overall"]
        lines.append(
            f"| `{SCHEDULES[label]}` | {row['observations']} | "
            f"{row['started_but_fewer_than_two_completed']} | "
            f"{overall['scenarios']['median']:.0f} / "
            f"{overall['scenarios']['p90']:.0f} | "
            f"{overall['nested_endgame_nodes']['median']:.0f} / "
            f"{overall['nested_endgame_nodes']['p90']:.0f} | "
            f"{overall['seconds']['median']:.2f} / "
            f"{overall['seconds']['p90']:.2f} |"
        )
    sensitivity = result["sensitivity"]
    monitoring = result["monitoring"]
    lines.extend(
        [
            "",
            "## Judges and monitoring",
            "",
            f"Stride-4 judges: "
            f"{result['judge_completion']['direct4_stride4']['accepted']}/"
            f"{result['judge_completion']['direct4_stride4']['attempted']} "
            "accepted.",
            "",
            f"Stride-2 judges: "
            f"{result['judge_completion']['direct4_stride2']['accepted']}/"
            f"{result['judge_completion']['direct4_stride2']['attempted']} "
            f"accepted; best agreement "
            f"{sensitivity['best_nominee_agreements']}/"
            f"{sensitivity['accepted_positions']}.",
            "",
            f"Full-enumeration judges: "
            f"{result['judge_completion']['direct4_stride1']['accepted']}/"
            f"{result['judge_completion']['direct4_stride1']['attempted']} "
            "accepted.",
            "",
            f"Sentinel normalized-throughput CV "
            f"{monitoring['normalized_throughput_cv']:.3f}; drift "
            f"{monitoring['early_to_late_drift']:+.1%}; flags "
            f"{monitoring['segment_noise']}.",
            "",
            "Intervals are deterministic bag-stratified bootstrap 95% CIs. "
            "Candidate completion is the portable boundary; local seconds "
            "are conversion estimates only.",
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
            for path in args.records
            for record in load_records(path)
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

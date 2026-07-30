#!/usr/bin/env python3
"""Analyze the common direct-3/direct-4 fresh-quality pilot."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
from typing import Any

from run_peg_time_calibration import load_records


COMPARISONS = {
    "full32_minus_adaptive": ("min8_patience8", "fixed_32"),
    "deep16_minus_adaptive": ("min8_patience8", "deep_16_8"),
    "deep24_minus_deep16": ("deep_16_8", "deep_24_12"),
}


def nearest_rank(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    return ordered[
        min(len(ordered) - 1, math.ceil(probability * len(ordered)) - 1)
    ]


def describe(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "p75": nearest_rank(values, 0.75),
        "p90": nearest_rank(values, 0.90),
        "max": max(values),
    }


def best_move(values: dict[str, dict[str, float]]) -> str:
    return max(
        values,
        key=lambda move: (
            float(values[move]["utility"]),
            float(values[move]["win"]),
            float(values[move]["spread"]),
            move,
        ),
    )


def rank_pair_agreement(
    first: dict[str, dict[str, float]],
    second: dict[str, dict[str, float]],
) -> dict[str, int]:
    moves = sorted(first)
    concordant = 0
    discordant = 0
    tied = 0
    for left_index, left in enumerate(moves):
        for right in moves[left_index + 1 :]:
            first_diff = (
                float(first[left]["utility"])
                - float(first[right]["utility"])
            )
            second_diff = (
                float(second[left]["utility"])
                - float(second[right]["utility"])
            )
            product = first_diff * second_diff
            if product > 0:
                concordant += 1
            elif product < 0:
                discordant += 1
            else:
                tied += 1
    return {
        "concordant": concordant,
        "discordant": discordant,
        "tied": tied,
    }


def analyze(
    records: list[dict[str, Any]], selection: dict[str, Any]
) -> dict[str, Any]:
    maps = {
        str(record["position"]): record
        for record in records
        if record.get("kind") == "fresh_quality_map"
    }
    judges = {
        (str(record["position"]), str(record["label"])): record
        for record in records
        if record.get("kind") == "judge"
    }
    values_by_sequence: dict[int, dict[str, dict[str, float]]] = {}
    for record in records:
        if record.get("kind") != "value":
            continue
        values_by_sequence.setdefault(int(record["sequence"]), {})[
            str(record["move"])
        ] = {
            metric: float(record[metric])
            for metric in ("utility", "win", "spread")
        }

    accepted_positions = []
    runtimes = {3: [], 4: []}
    best_flips = []
    rank_counts = {"concordant": 0, "discordant": 0, "tied": 0}
    nominee_shifts = []
    comparisons = {
        name: [] for name in COMPARISONS
    }
    by_bag_correction = {bag: [] for bag in range(1, 5)}
    for selected in selection["selected"]:
        position = str(selected["position"])
        bag = int(selected["bag"])
        quality_map = maps.get(position)
        judge3 = judges.get((position, "fresh_direct3_stride4"))
        judge4 = judges.get((position, "fresh_direct4_stride4"))
        if (
            quality_map is None
            or judge3 is None
            or judge4 is None
            or int(judge3.get("accepted", 0)) != 1
            or int(judge4.get("accepted", 0)) != 1
        ):
            continue
        values3 = values_by_sequence.get(int(judge3["sequence"]), {})
        values4 = values_by_sequence.get(int(judge4["sequence"]), {})
        nominees = [str(move) for move in quality_map["nominees"]]
        if set(values3) != set(nominees) or set(values4) != set(nominees):
            continue
        accepted_positions.append(position)
        runtimes[3].append(float(judge3["wall_seconds"]))
        runtimes[4].append(float(judge4["wall_seconds"]))
        best3 = best_move(values3)
        best4 = best_move(values4)
        if best3 != best4:
            best_flips.append(
                {
                    "position": position,
                    "bag": bag,
                    "direct3_best": best3,
                    "direct4_best": best4,
                }
            )
        agreement = rank_pair_agreement(values3, values4)
        for key in rank_counts:
            rank_counts[key] += agreement[key]
        for move in nominees:
            nominee_shifts.append(
                float(values4[move]["utility"])
                - float(values3[move]["utility"])
            )
        for name, (before_policy, after_policy) in COMPARISONS.items():
            before = str(
                quality_map["policies"][before_policy]["move"]
            )
            after = str(quality_map["policies"][after_policy]["move"])
            delta3 = (
                float(values3[after]["utility"])
                - float(values3[before]["utility"])
            )
            delta4 = (
                float(values4[after]["utility"])
                - float(values4[before]["utility"])
            )
            row = {
                "position": position,
                "bag": bag,
                "direct3_delta": delta3,
                "direct4_delta": delta4,
                "direct4_minus_direct3": delta4 - delta3,
                "sign_flip": int(
                    delta3 * delta4 < 0
                    or (delta3 == 0) != (delta4 == 0)
                ),
            }
            comparisons[name].append(row)
            by_bag_correction[bag].append(
                row["direct4_minus_direct3"]
            )

    comparison_summary = {}
    for name, rows in comparisons.items():
        delta3 = [float(row["direct3_delta"]) for row in rows]
        delta4 = [float(row["direct4_delta"]) for row in rows]
        denominator = sum(value * value for value in delta3)
        comparison_summary[name] = {
            "n": len(rows),
            "direct3": describe(delta3),
            "direct4": describe(delta4),
            "direct4_minus_direct3": describe(
                [float(row["direct4_minus_direct3"]) for row in rows]
            ),
            "sign_flips": sum(int(row["sign_flip"]) for row in rows),
            "attenuation_amplification_slope_through_origin": (
                sum(a * b for a, b in zip(delta3, delta4, strict=True))
                / denominator
                if denominator > 0
                else math.nan
            ),
        }
    runtime4 = describe(runtimes[4])
    conservative_seconds = (
        max(float(runtime4["mean"]), float(runtime4["p75"]))
        if runtime4.get("n", 0)
        else math.nan
    )
    overnight_positions = (
        math.floor(9 * 3600 / conservative_seconds)
        if conservative_seconds and not math.isnan(conservative_seconds)
        else 0
    )
    return {
        "selected_positions": len(selection["selected"]),
        "accepted_common_positions": len(accepted_positions),
        "accepted_position_ids": accepted_positions,
        "runtime_seconds": {
            "direct3": describe(runtimes[3]),
            "direct4": runtime4,
            "direct4_total": sum(runtimes[4]),
        },
        "direct3_vs_direct4_best": {
            "flips": len(best_flips),
            "flip_rate": (
                len(best_flips) / len(accepted_positions)
                if accepted_positions
                else math.nan
            ),
            "details": best_flips,
        },
        "rank_pair_stability": {
            **rank_counts,
            "discordance_rate_excluding_ties": (
                rank_counts["discordant"]
                / (rank_counts["concordant"] + rank_counts["discordant"])
                if rank_counts["concordant"] + rank_counts["discordant"]
                else math.nan
            ),
        },
        "nominee_direct4_minus_direct3_utility": describe(
            nominee_shifts
        ),
        "comparisons": comparison_summary,
        "bag_conditioned_direct4_minus_direct3_utility": {
            str(bag): describe(values)
            for bag, values in by_bag_correction.items()
        },
        "overnight_projection": {
            "budget_hours": 9,
            "per_position_seconds": conservative_seconds,
            "projected_positions_before_sentinels": overnight_positions,
            "basis": "max(pilot mean, pilot p75) direct-4 wall time",
        },
    }


def make_markdown(result: dict[str, Any]) -> str:
    runtime3 = result["runtime_seconds"]["direct3"]
    runtime4 = result["runtime_seconds"]["direct4"]
    best = result["direct3_vs_direct4_best"]
    rank = result["rank_pair_stability"]
    lines = [
        "# Fresh PEG direct-4 oracle pilot",
        "",
        f"Accepted common direct-3/direct-4 judgments: "
        f"{result['accepted_common_positions']}/"
        f"{result['selected_positions']}.",
        "",
        "| judge | mean s | median s | p75 s | p90 s | max s |",
        "|---|---:|---:|---:|---:|---:|",
        f"| direct 3 | {runtime3.get('mean', math.nan):.2f} | "
        f"{runtime3.get('median', math.nan):.2f} | "
        f"{runtime3.get('p75', math.nan):.2f} | "
        f"{runtime3.get('p90', math.nan):.2f} | "
        f"{runtime3.get('max', math.nan):.2f} |",
        f"| direct 4 | {runtime4.get('mean', math.nan):.2f} | "
        f"{runtime4.get('median', math.nan):.2f} | "
        f"{runtime4.get('p75', math.nan):.2f} | "
        f"{runtime4.get('p90', math.nan):.2f} | "
        f"{runtime4.get('max', math.nan):.2f} |",
        "",
        f"Best-nominee flips: {best['flips']}/"
        f"{result['accepted_common_positions']} "
        f"({best['flip_rate']:.1%}). Pair-rank discordance excluding ties: "
        f"{rank['discordance_rate_excluding_ties']:.1%}.",
        "",
        "| comparison | n | mean direct-3 | mean direct-4 | "
        "mean 4−3 correction | sign flips | 4-on-3 slope |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for name, row in result["comparisons"].items():
        lines.append(
            f"| {name} | {row['n']} | "
            f"{row['direct3'].get('mean', math.nan):+.6f} | "
            f"{row['direct4'].get('mean', math.nan):+.6f} | "
            f"{row['direct4_minus_direct3'].get('mean', math.nan):+.6f} | "
            f"{row['sign_flips']} | "
            f"{row['attenuation_amplification_slope_through_origin']:.3f} |"
        )
    projection = result["overnight_projection"]
    lines.extend(
        [
            "",
            f"A 9-hour block projects to about "
            f"{projection['projected_positions_before_sentinels']} direct-4 "
            "positions before sentinel overhead, using "
            "`max(mean, p75)` pilot wall time. This is a scheduling estimate, "
            "not a strength result.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--records",
        type=pathlib.Path,
        default=pathlib.Path("obj/peg_quality_20260730/records.jsonl"),
    )
    parser.add_argument("--selection", type=pathlib.Path, required=True)
    parser.add_argument("--json", type=pathlib.Path, required=True)
    parser.add_argument("--markdown", type=pathlib.Path, required=True)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parent.parent
    records = args.records if args.records.is_absolute() else repo / args.records
    selection = (
        args.selection
        if args.selection.is_absolute()
        else repo / args.selection
    )
    output_json = args.json if args.json.is_absolute() else repo / args.json
    output_markdown = (
        args.markdown
        if args.markdown.is_absolute()
        else repo / args.markdown
    )
    result = analyze(
        load_records(records), json.loads(selection.read_text())
    )
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_markdown.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    output_markdown.write_text(make_markdown(result))
    print(
        f"analyzed {result['accepted_common_positions']}/"
        f"{result['selected_positions']} common pilot positions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

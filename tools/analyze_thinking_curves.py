#!/usr/bin/env python3
"""Summarize provisional regret-vs-work curves from THINKING_CURVE_POINT rows."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path
from typing import Callable


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def mean_ci(values: list[float]) -> tuple[float, float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, mean, mean
    sem = statistics.stdev(values) / math.sqrt(len(values))
    return mean, mean - 1.96 * sem, mean + 1.96 * sem


def robust_cv(values: list[float]) -> float:
    """Median absolute deviation as a scale-free stability diagnostic."""
    if not values:
        return math.nan
    median = statistics.median(values)
    if median == 0.0:
        return math.nan
    mad = statistics.median(abs(value - median) for value in values)
    return 1.4826 * mad / abs(median)


def ordered_half_drift(
    group: list[dict[str, str]],
    value_for: Callable[[dict[str, str]], float],
) -> float:
    """Late-vs-early median fractional rate change in source order."""
    ordered = sorted(group, key=lambda row: int(row["source_index"]))
    half = len(ordered) // 2
    if half == 0:
        return math.nan
    early = [value_for(row) for row in ordered[:half]]
    late = [value_for(row) for row in ordered[-half:]]
    early_median = statistics.median(early)
    if early_median == 0.0:
        return math.nan
    return statistics.median(late) / early_median - 1.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()

    # A resumed chunk can repeat its last position. Keep the latest complete
    # observation for each position/depth/target.
    rows: dict[tuple[int, int, int, int], dict[str, str]] = {}
    with args.log.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            fields = parse_fields(line)
            key = (
                int(fields["source_index"]),
                int(fields["plies"]),
                int(fields["target_nodes"]),
                int(fields["final"]),
            )
            rows[key] = fields

    grouped: dict[tuple[int, int], list[dict[str, str]]] = {}
    for fields in rows.values():
        target = int(fields["target_nodes"])
        final = int(fields["final"])
        grouped.setdefault((int(fields["plies"]), -1 if final else target), []).append(
            fields
        )

    output_rows: list[dict[str, object]] = []
    for (plies, target), group in sorted(grouped.items()):
        regrets = [float(row["judge_regret"]) for row in group]
        elapsed = [int(row["elapsed_ns"]) / 1e9 for row in group]
        actual_nodes = [int(row["nodes"]) for row in group]
        nps = [
            nodes / seconds
            for nodes, seconds in zip(actual_nodes, elapsed, strict=True)
            if seconds > 0.0
        ]
        has_cpu = all("cpu_ns" in row for row in group)
        cpu_seconds = (
            [int(row["cpu_ns"]) / 1e9 for row in group] if has_cpu else []
        )
        scheduled_cores = (
            [
                cpu / seconds
                for cpu, seconds in zip(cpu_seconds, elapsed, strict=True)
                if seconds > 0.0
            ]
            if has_cpu
            else []
        )
        hits = [
            int(row["selected_rank"]) == int(row["judge_best_rank"])
            for row in group
        ]
        stable = [
            int(row["selected_rank"])
            == int(
                rows[
                    (
                        int(row["source_index"]),
                        int(row["plies"]),
                        0,
                        1,
                    )
                ]["selected_rank"]
            )
            for row in group
        ]
        mean, low, high = mean_ci(regrets)
        output_rows.append(
            {
                "plies": plies,
                "target_nodes": "final" if target < 0 else target,
                "positions": len(group),
                "mean_judge_regret": mean,
                "ci95_low": low,
                "ci95_high": high,
                "judge_best_rate": statistics.fmean(hits),
                "final_choice_stability": statistics.fmean(stable),
                "mean_elapsed_seconds": statistics.fmean(elapsed),
                "mean_actual_nodes": statistics.fmean(actual_nodes),
                "median_nps": statistics.median(nps),
                "robust_nps_cv": robust_cv(nps),
                "nps_late_vs_early": ordered_half_drift(
                    group,
                    lambda row: int(row["nodes"])
                    / (int(row["elapsed_ns"]) / 1e9),
                ),
                "mean_scheduled_cores": (
                    statistics.fmean(scheduled_cores)
                    if scheduled_cores
                    else math.nan
                ),
                "robust_scheduled_cores_cv": robust_cv(scheduled_cores),
            }
        )

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=output_rows[0].keys() if output_rows else [])
            if output_rows:
                writer.writeheader()
                writer.writerows(output_rows)

    for row in output_rows:
        target = row["target_nodes"]
        print(
            "THINKING_CURVE_SUMMARY "
            f"plies={row['plies']} target_nodes={target} "
            f"positions={row['positions']} "
            f"mean_judge_regret={row['mean_judge_regret']:.9f} "
            f"ci95=[{row['ci95_low']:.9f},{row['ci95_high']:.9f}] "
            f"judge_best_rate={row['judge_best_rate']:.4f} "
            f"final_choice_stability={row['final_choice_stability']:.4f} "
            f"mean_elapsed_seconds={row['mean_elapsed_seconds']:.6f} "
            f"mean_actual_nodes={row['mean_actual_nodes']:.1f} "
            f"median_nps={row['median_nps']:.0f} "
            f"robust_nps_cv={row['robust_nps_cv']:.4f} "
            f"nps_late_vs_early={row['nps_late_vs_early']:+.4f} "
            f"mean_scheduled_cores={row['mean_scheduled_cores']:.3f} "
            f"robust_scheduled_cores_cv="
            f"{row['robust_scheduled_cores_cv']:.4f}"
        )


if __name__ == "__main__":
    main()

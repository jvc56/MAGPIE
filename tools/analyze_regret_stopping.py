#!/ usr / bin / env python3
"""Summarize paired regret-stopped and full-budget thinking-curve arms."""

from __future__ import annotations

import argparse
import math
import statistics
from dataclasses import dataclass
from pathlib import Path


def parse_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


@dataclass(frozen=True)
class Point:
    source_index: int
    bag: int
    control: bool
    nodes: int
    elapsed_seconds: float
    selected_rank: int
    judge_regret: float
    estimated_regret: float
    status: int


def load_points(path: Path) -> dict[int, dict[bool, Point]]:
    points: dict[int, dict[bool, Point]] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            fields = parse_fields(line)
            if fields.get("final") != "0":
                continue
            point = Point(
                source_index=int(fields["source_index"]),
                bag=int(fields["bag"]),
                control=fields.get("control", "0") == "1",
                nodes=int(fields["nodes"]),
                elapsed_seconds=int(fields["elapsed_ns"]) / 1e9,
                selected_rank=int(fields["selected_rank"]),
                judge_regret=float(fields["judge_regret"]),
                estimated_regret=float(fields["estimated_regret"]),
                status=int(fields["bai_status"]),
            )
            by_mode = points.setdefault(point.source_index, {})
            if point.control in by_mode:
                raise ValueError(
                    f"duplicate {'control' if point.control else 'stopped'} "
                    f"point for source {point.source_index}"
                )
            by_mode[point.control] = point
    return points


def mean_ci95(values: list[float]) -> tuple[float, float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, math.nan, math.nan
    sem = statistics.stdev(values) / math.sqrt(len(values))
    radius = 1.96 * sem
    return mean, mean - radius, mean + radius


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = fraction * (len(ordered) - 1)
    low = math.floor(index)
    high = math.ceil(index)
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def summarize(label: str, pairs: list[tuple[Point, Point]]) -> None:
    stopped = [pair[0] for pair in pairs]
    controls = [pair[1] for pair in pairs]
    node_fractions = [a.nodes / b.nodes for a, b in pairs]
    node_savings = [1.0 - value for value in node_fractions]
    stopped_regrets = [point.judge_regret for point in stopped]
    control_regrets = [point.judge_regret for point in controls]
    regret_deltas = [
        a.judge_regret - b.judge_regret for a, b in pairs
    ]
    estimate_errors = [
        point.estimated_regret - point.judge_regret for point in stopped
    ]
    stopped_mean, stopped_low, stopped_high = mean_ci95(stopped_regrets)
    control_mean, control_low, control_high = mean_ci95(control_regrets)
    delta_mean, delta_low, delta_high = mean_ci95(regret_deltas)
    savings_mean, savings_low, savings_high = mean_ci95(node_savings)
    estimate_error_mean, estimate_error_low, estimate_error_high = mean_ci95(
        estimate_errors
    )
    agreement = statistics.fmean(
        float(a.selected_rank == b.selected_rank) for a, b in pairs
    )
    print(
        f"REGRET_STOP_SUMMARY stratum={label} positions={len(pairs)} "
        f"stopped_regret={stopped_mean:.9f} "
        f"stopped_ci95=[{stopped_low:.9f},{stopped_high:.9f}] "
        f"control_regret={control_mean:.9f} "
        f"control_ci95=[{control_low:.9f},{control_high:.9f}] "
        f"paired_regret_delta={delta_mean:+.9f} "
        f"delta_ci95=[{delta_low:+.9f},{delta_high:+.9f}] "
        f"node_savings={savings_mean:.4%} "
        f"savings_ci95=[{savings_low:.4%},{savings_high:.4%}] "
        f"median_node_fraction={statistics.median(node_fractions):.6f} "
        f"p90_node_fraction={percentile(node_fractions, 0.90):.6f} "
        f"choice_agreement={agreement:.4%} "
        f"estimate_minus_judge={estimate_error_mean:+.9f} "
        f"estimate_error_ci95=[{estimate_error_low:+.9f},"
        f"{estimate_error_high:+.9f}]"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()

    all_pairs: list[tuple[Point, Point]] = []
    for path in args.logs:
        loaded = load_points(path)
        complete = [
            (by_mode[False], by_mode[True])
            for by_mode in loaded.values()
            if False in by_mode and True in by_mode
        ]
        if not complete:
            raise ValueError(f"{path}: no complete stopped/control pairs")
        print(f"REGRET_STOP_FILE path={path} complete_pairs={len(complete)}")
        summarize("all", complete)
        all_pairs.extend(complete)

    if len(args.logs) > 1:
        summarize("pooled", all_pairs)
    for label, low, high in (
        ("opening", 61, 100),
        ("midgame", 25, 60),
        ("preendgame", 1, 24),
    ):
        stratum = [
            pair for pair in all_pairs if low <= pair[0].bag <= high
        ]
        if len(stratum) >= 2:
            summarize(label, stratum)


if __name__ == "__main__":
    main()

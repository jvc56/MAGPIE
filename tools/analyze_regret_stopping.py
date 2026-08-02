#!/usr/bin/env python3
"""Audit paired regret-stopped and same-seed fixed-budget SIM arms."""

from __future__ import annotations

import argparse
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

try:
    from tools.clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
except ModuleNotFoundError:  # Direct execution from tools/.
    from clustered_inference import (  # type: ignore[no-redef]
        student_t_critical,
        student_t_two_sided_p_value,
    )


def parse_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def optional_float(fields: dict[str, str], key: str) -> float:
    value = fields.get(key)
    return float(value) if value is not None else math.nan


def optional_int(fields: dict[str, str], key: str, default: int = -1) -> int:
    value = fields.get(key)
    return int(value) if value is not None else default


@dataclass(frozen=True)
class Point:
    source_index: int
    position: int
    game: str
    bag: int
    plies: int
    target_nodes: int
    control: bool
    nodes: int
    elapsed_seconds: float
    selected_rank: int
    judge_regret: float
    estimated_regret: float
    joint_estimated_regret: float
    regret_at_stop: float
    joint_regret_at_stop: float
    near_tie_challengers: int
    near_tie_challengers_at_stop: int
    status: int

    @property
    def legacy_stop_estimate(self) -> float:
        if math.isfinite(self.regret_at_stop):
            return self.regret_at_stop
        return self.estimated_regret

    @property
    def joint_stop_estimate(self) -> float:
        if math.isfinite(self.joint_regret_at_stop):
            return self.joint_regret_at_stop
        return self.joint_estimated_regret

    @property
    def near_ties_at_stop(self) -> int:
        if self.near_tie_challengers_at_stop >= 0:
            return self.near_tie_challengers_at_stop
        return self.near_tie_challengers


PointKey = tuple[int, int, int]


def load_points(path: Path) -> dict[PointKey, dict[bool, Point]]:
    points: dict[PointKey, dict[bool, Point]] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            fields = parse_fields(line)
            if fields.get("final") != "0":
                continue
            point = Point(
                source_index=int(fields["source_index"]),
                position=optional_int(
                    fields, "position", int(fields["source_index"])
                ),
                game=fields.get("game", fields["source_index"]),
                bag=int(fields["bag"]),
                plies=int(fields["plies"]),
                target_nodes=int(fields["target_nodes"]),
                control=fields.get("control", "0") == "1",
                nodes=int(fields["nodes"]),
                elapsed_seconds=int(fields["elapsed_ns"]) / 1e9,
                selected_rank=int(fields["selected_rank"]),
                judge_regret=float(fields["judge_regret"]),
                estimated_regret=float(fields["estimated_regret"]),
                joint_estimated_regret=optional_float(
                    fields, "joint_estimated_regret"
                ),
                regret_at_stop=optional_float(fields, "regret_at_stop"),
                joint_regret_at_stop=optional_float(
                    fields, "joint_regret_at_stop"
                ),
                near_tie_challengers=optional_int(
                    fields, "near_tie_challengers"
                ),
                near_tie_challengers_at_stop=optional_int(
                    fields, "near_tie_challengers_at_stop"
                ),
                status=int(fields["bai_status"]),
            )
            key = (point.source_index, point.plies, point.target_nodes)
            by_mode = points.setdefault(key, {})
            if point.control in by_mode:
                raise ValueError(
                    f"duplicate {'control' if point.control else 'stopped'} "
                    f"point for {key}"
                )
            by_mode[point.control] = point
    return points


def pair_points(
    points: dict[PointKey, dict[bool, Point]],
) -> list[tuple[Point, Point]]:
    return [
        (by_mode[False], by_mode[True])
        for by_mode in points.values()
        if False in by_mode and True in by_mode
    ]


@dataclass(frozen=True)
class Summary:
    mean: float
    low: float
    high: float
    p_value: float
    clusters: int


def summarize_values(values: list[float]) -> Summary:
    center = statistics.fmean(values)
    if len(values) < 2:
        return Summary(center, math.nan, math.nan, math.nan, len(values))
    standard_error = statistics.stdev(values) / math.sqrt(len(values))
    if standard_error == 0.0:
        p_value = 0.0 if center != 0.0 else 1.0
    else:
        p_value = student_t_two_sided_p_value(
            center / standard_error, len(values) - 1
        )
    radius = student_t_critical(0.95, len(values) - 1) * standard_error
    return Summary(
        center, center - radius, center + radius, p_value, len(values)
    )


def clustered_values(
    pairs: list[tuple[Point, Point]],
    metric: Callable[[Point, Point], float],
) -> list[float]:
    by_game: dict[str, list[float]] = {}
    for stopped, control in pairs:
        by_game.setdefault(stopped.game, []).append(metric(stopped, control))
    return [statistics.fmean(values) for values in by_game.values()]


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = fraction * (len(ordered) - 1)
    low = math.floor(index)
    high = math.ceil(index)
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def format_summary(summary: Summary, signed: bool = False) -> str:
    sign = "+" if signed else ""
    return (
        f"{summary.mean:{sign}.9f} "
        f"ci95=[{summary.low:{sign}.9f},{summary.high:{sign}.9f}] "
        f"p={summary.p_value:.6g}"
    )


def summarize(label: str, pairs: list[tuple[Point, Point]], margin: float) -> None:
    node_fractions = [a.nodes / b.nodes for a, b in pairs]
    stopped_regret = summarize_values(
        clustered_values(pairs, lambda a, _b: a.judge_regret)
    )
    control_regret = summarize_values(
        clustered_values(pairs, lambda _a, b: b.judge_regret)
    )
    regret_delta = summarize_values(
        clustered_values(pairs, lambda a, b: a.judge_regret - b.judge_regret)
    )
    node_savings = summarize_values(
        clustered_values(pairs, lambda a, b: 1.0 - a.nodes / b.nodes)
    )
    agreement = statistics.fmean(
        float(a.selected_rank == b.selected_rank) for a, b in pairs
    )
    gate = (
        "pass"
        if regret_delta.clusters >= 2 and regret_delta.high <= margin
        else "fail"
    )
    print(
        f"REGRET_STOP_SUMMARY stratum={label} positions={len(pairs)} "
        f"games={regret_delta.clusters} "
        f"stopped_regret={format_summary(stopped_regret)} "
        f"control_regret={format_summary(control_regret)} "
        f"paired_regret_delta={format_summary(regret_delta, signed=True)} "
        f"node_savings={node_savings.mean:.4%} "
        f"savings_ci95=[{node_savings.low:.4%},{node_savings.high:.4%}] "
        f"median_node_fraction={statistics.median(node_fractions):.6f} "
        f"p90_node_fraction={percentile(node_fractions, 0.90):.6f} "
        f"choice_agreement={agreement:.4%} margin={margin:.9f} "
        f"optional_stopping_gate={gate}"
    )


def reliability(label: str, pairs: list[tuple[Point, Point]]) -> None:
    estimator: Callable[[Point], float]
    estimator = (
        (lambda point: point.legacy_stop_estimate)
        if label == "legacy"
        else (lambda point: point.joint_stop_estimate)
    )
    observations = [
        (estimator(stopped), stopped.judge_regret, stopped.game)
        for stopped, _control in pairs
        if math.isfinite(estimator(stopped))
        and estimator(stopped) >= 0.0
        and math.isfinite(stopped.judge_regret)
    ]
    if not observations:
        print(f"REGRET_RELIABILITY estimator={label} positions=0")
        return
    denominator = sum(predicted * predicted for predicted, _actual, _ in observations)
    slope = (
        sum(predicted * actual for predicted, actual, _ in observations)
        / denominator
        if denominator > 0.0
        else math.nan
    )
    errors_by_game: dict[str, list[float]] = {}
    for predicted, actual, game in observations:
        errors_by_game.setdefault(game, []).append(predicted - actual)
    error = summarize_values(
        [statistics.fmean(values) for values in errors_by_game.values()]
    )

    ordered = sorted(observations)
    decile_count = min(10, len(ordered))
    underpredicting_deciles = 0
    worst_ratio = 0.0
    for decile in range(decile_count):
        start = decile * len(ordered) // decile_count
        end = (decile + 1) * len(ordered) // decile_count
        group = ordered[start:end]
        predicted_mean = statistics.fmean(row[0] for row in group)
        actual_mean = statistics.fmean(row[1] for row in group)
        ratio = (
            actual_mean / predicted_mean
            if predicted_mean > 0.0
            else math.inf if actual_mean > 0.0 else 1.0
        )
        worst_ratio = max(worst_ratio, ratio)
        if ratio > 2.0:
            underpredicting_deciles += 1
    slope_gate = math.isfinite(slope) and 0.7 <= slope <= 1.4
    decile_gate = underpredicting_deciles == 0
    print(
        f"REGRET_RELIABILITY estimator={label} positions={len(observations)} "
        f"games={len(errors_by_game)} calibration_slope={slope:.6f} "
        f"estimate_minus_judge={format_summary(error, signed=True)} "
        f"deciles={decile_count} underpredicting_deciles="
        f"{underpredicting_deciles} worst_actual_over_predicted={worst_ratio:.6g} "
        f"slope_gate={'pass' if slope_gate else 'fail'} "
        f"decile_gate={'pass' if decile_gate else 'fail'}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--noninferiority-margin",
        type=float,
        default=0.0,
        help="maximum allowed stopped-minus-control utility regret",
    )
    args = parser.parse_args()

    all_pairs: list[tuple[Point, Point]] = []
    for path in args.logs:
        complete = pair_points(load_points(path))
        if not complete:
            raise ValueError(f"{path}: no exact-target stopped/control pairs")
        print(f"REGRET_STOP_FILE path={path} complete_pairs={len(complete)}")
        summarize("file", complete, args.noninferiority_margin)
        all_pairs.extend(complete)

    summarize("all", all_pairs, args.noninferiority_margin)
    reliability("legacy", all_pairs)
    reliability("joint", all_pairs)
    for label, low, high in (
        ("opening", 61, 100),
        ("midgame", 25, 60),
        ("preendgame", 1, 24),
    ):
        stratum = [pair for pair in all_pairs if low <= pair[0].bag <= high]
        if len({pair[0].game for pair in stratum}) >= 2:
            summarize(label, stratum, args.noninferiority_margin)
    for label, predicate in (
        ("ties0", lambda value: value == 0),
        ("ties1", lambda value: value == 1),
        ("ties2_3", lambda value: 2 <= value <= 3),
        ("ties4plus", lambda value: value >= 4),
    ):
        stratum = [
            pair
            for pair in all_pairs
            if predicate(pair[0].near_ties_at_stop)
        ]
        if len({pair[0].game for pair in stratum}) >= 2:
            summarize(label, stratum, args.noninferiority_margin)


if __name__ == "__main__":
    main()

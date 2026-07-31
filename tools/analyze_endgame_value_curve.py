#!/usr/bin/env python3
"""Analyze endgame oracle regret at completed iterative-deepening depths."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Point:
    corpus: int
    position: int
    rack_tiles: int
    depth: int
    nodes: int
    elapsed_seconds: float
    cpu_seconds: float
    root_completed: int
    root_total: int
    ply2_completed: int
    ply2_total: int
    tiny_move: int
    search_value: float
    exact_value: float
    oracle_value: float
    scored: bool

    @property
    def scheduled_cores(self) -> float:
        return self.cpu_seconds / self.elapsed_seconds

    @staticmethod
    def win_utility(value: float) -> float:
        if value > 0.0:
            return 1.0
        if value < 0.0:
            return 0.0
        return 0.5

    @property
    def spread_regret(self) -> float:
        return self.oracle_value - self.exact_value

    @property
    def win_regret(self) -> float:
        return self.win_utility(self.oracle_value) - self.win_utility(
            self.exact_value
        )

    @property
    def utility_regret(self) -> float:
        return self.win_regret + 1.0e-4 * self.spread_regret


@dataclass(frozen=True)
class RunSummary:
    source: int
    position: int
    rack_tiles: int
    completed: bool
    depth: int
    nodes: int
    elapsed_seconds: float
    cpu_seconds: float

    @property
    def rate(self) -> float:
        return self.nodes / self.elapsed_seconds

    @property
    def scheduled_cores(self) -> float:
        return self.cpu_seconds / self.elapsed_seconds


@dataclass(frozen=True)
class JudgeSummary:
    source: int
    position: int
    rack_tiles: int
    completed: bool
    target_depth: int
    actual_depth: int
    nodes: int
    elapsed_seconds: float
    cpu_seconds: float

    @property
    def rate(self) -> float:
        return self.nodes / self.elapsed_seconds

    @property
    def scheduled_cores(self) -> float:
        return self.cpu_seconds / self.elapsed_seconds


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def fraction(value: str) -> tuple[int, int]:
    numerator, denominator = value.split("/", 1)
    return int(numerator), int(denominator)


def load_points(corpus: int, paths: list[Path]) -> list[Point]:
    positions: dict[int, int] = {}
    raw_points: dict[tuple[int, int], tuple[dict[str, str], bool]] = {}
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("EGCURVEPOS "):
                    fields = parse_fields(line)
                    positions[int(fields["pos"])] = int(fields["rack_tiles"])
                elif line.startswith(("EGCURVEPOINT ", "EGCURVECONTEXT ")):
                    fields = parse_fields(line)
                    raw_points[(int(fields["pos"]), int(fields["depth"]))] = (
                        fields,
                        line.startswith("EGCURVEPOINT "),
                    )

    points: list[Point] = []
    for (position, depth), (fields, scored) in raw_points.items():
        if position not in positions:
            continue
        root_completed, root_total = fraction(fields["root"])
        ply2_completed, ply2_total = fraction(fields["ply2"])
        elapsed = int(fields["elapsed_ns"]) / 1.0e9
        cpu = int(fields["cpu_ns"]) / 1.0e9
        if elapsed <= 0.0:
            continue
        points.append(
            Point(
                corpus=corpus,
                position=position,
                rack_tiles=positions[position],
                depth=depth,
                nodes=int(fields["nodes"]),
                elapsed_seconds=elapsed,
                cpu_seconds=cpu,
                root_completed=root_completed,
                root_total=root_total,
                ply2_completed=ply2_completed,
                ply2_total=ply2_total,
                tiny_move=int(fields["tiny"]),
                search_value=float(fields["search"]),
                exact_value=float(fields["exact"]) if scored else math.nan,
                oracle_value=(
                    float(fields["exact"]) + float(fields["regret"])
                    if scored
                    else math.nan
                ),
                scored=scored,
            )
        )
    return points


def load_run_summaries(paths: list[Path]) -> list[RunSummary]:
    summaries: list[RunSummary] = []
    for source, path in enumerate(paths):
        # Resumed output can repeat a position; keep the final terminal row.
        by_position: dict[int, RunSummary] = {}
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                completed = line.startswith("EGCURVEPOS ")
                truncated = line.startswith("EGCURVETRUNC ")
                if not completed and not truncated:
                    continue
                fields = parse_fields(line)
                elapsed = int(fields["elapsed_ns"]) / 1.0e9
                cpu = int(fields["cpu_ns"]) / 1.0e9
                if completed:
                    nodes = int(
                        fields["search_nodes"]
                        if "search_nodes" in fields
                        else fields["oracle_nodes"]
                    )
                else:
                    nodes = int(fields["nodes"])
                if elapsed <= 0.0 or nodes <= 0:
                    continue
                position = int(fields["pos"])
                by_position[position] = RunSummary(
                    source=source,
                    position=position,
                    rack_tiles=int(fields["rack_tiles"]),
                    completed=completed,
                    depth=(
                        int(
                            fields["search_depth"]
                            if "search_depth" in fields
                            else fields["oracle_depth"]
                        )
                        if completed
                        else int(fields["depth"])
                    ),
                    nodes=nodes,
                    elapsed_seconds=elapsed,
                    cpu_seconds=cpu,
                )
        summaries.extend(by_position.values())
    return summaries


def load_judge_summaries(paths: list[Path]) -> list[JudgeSummary]:
    summaries: list[JudgeSummary] = []
    for source, path in enumerate(paths):
        # A later resumed completion supersedes an earlier censored judge for
        # quality accounting, while the raw log still preserves both attempts.
        by_position: dict[int, JudgeSummary] = {}
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                completed = line.startswith("EGCURVEPOS ")
                truncated = line.startswith("EGCURVEJUDGETRUNC ")
                if not completed and not truncated:
                    continue
                fields = parse_fields(line)
                if completed:
                    elapsed = int(fields["oracle_elapsed_ns"]) / 1.0e9
                    cpu = int(fields["oracle_cpu_ns"]) / 1.0e9
                    target_depth = int(fields["oracle_depth"])
                    actual_depth = target_depth
                    nodes = int(fields["oracle_nodes"])
                else:
                    elapsed = int(fields["judge_elapsed_ns"]) / 1.0e9
                    cpu = int(fields.get("judge_cpu_ns", "0")) / 1.0e9
                    target_depth = int(fields["judge_depth"])
                    actual_depth = int(fields["actual_depth"])
                    nodes = int(fields["judge_nodes"])
                if elapsed <= 0.0 or nodes <= 0:
                    continue
                position = int(fields["pos"])
                by_position[position] = JudgeSummary(
                    source=source,
                    position=position,
                    rack_tiles=int(fields["rack_tiles"]),
                    completed=completed,
                    target_depth=target_depth,
                    actual_depth=actual_depth,
                    nodes=nodes,
                    elapsed_seconds=elapsed,
                    cpu_seconds=cpu,
                )
        summaries.extend(by_position.values())
    return summaries


def mean_ci(values: list[float]) -> tuple[float, float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, math.nan, math.nan
    sem = statistics.stdev(values) / math.sqrt(len(values))
    return mean, mean - 1.96 * sem, mean + 1.96 * sem


def robust_cv(values: list[float]) -> float:
    if not values:
        return math.nan
    median = statistics.median(values)
    if median == 0.0:
        return math.nan
    mad = statistics.median(abs(value - median) for value in values)
    return 1.4826 * mad / abs(median)


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    index = probability * (len(ordered) - 1)
    low = int(math.floor(index))
    high = int(math.ceil(index))
    if low == high:
        return ordered[low]
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def half_drift(values: list[float]) -> float:
    half = len(values) // 2
    if half == 0:
        return math.nan
    early = statistics.median(values[:half])
    if early == 0.0:
        return math.nan
    return statistics.median(values[-half:]) / early - 1.0


def summarize(
    label: str, points: list[Point]
) -> list[dict[str, object]]:
    points = [point for point in points if point.scored]
    by_depth: dict[int, list[Point]] = {}
    by_position: dict[tuple[int, int], list[Point]] = {}
    for point in points:
        by_depth.setdefault(point.depth, []).append(point)
        by_position.setdefault((point.corpus, point.position), []).append(
            point
        )
    for sequence in by_position.values():
        sequence.sort(key=lambda point: point.depth)

    output: list[dict[str, object]] = []
    for depth, group in sorted(by_depth.items()):
        regrets = [point.spread_regret for point in group]
        utility_regrets = [point.utility_regret for point in group]
        win_regrets = [point.win_regret for point in group]
        mean, low, high = mean_ci(regrets)
        utility_mean, utility_low, utility_high = mean_ci(utility_regrets)
        paired_gain: list[float] = []
        paired_utility_gain: list[float] = []
        incremental_nodes: list[int] = []
        incremental_seconds: list[float] = []
        ebfs: list[float] = []
        for point in group:
            sequence = by_position[(point.corpus, point.position)]
            index = sequence.index(point)
            if index == 0:
                continue
            previous = sequence[index - 1]
            if previous.depth != point.depth - 1:
                continue
            paired_gain.append(
                previous.spread_regret - point.spread_regret
            )
            paired_utility_gain.append(
                previous.utility_regret - point.utility_regret
            )
            current_increment = point.nodes - previous.nodes
            incremental_nodes.append(current_increment)
            incremental_seconds.append(
                point.elapsed_seconds - previous.elapsed_seconds
            )
            if (
                index >= 2
                and sequence[index - 2].depth == previous.depth - 1
            ):
                prior_increment = (
                    previous.nodes - sequence[index - 2].nodes
                )
                if prior_increment > 0:
                    ebfs.append(current_increment / prior_increment)
        gain, gain_low, gain_high = (
            mean_ci(paired_gain)
            if paired_gain
            else (math.nan, math.nan, math.nan)
        )
        utility_gain, utility_gain_low, utility_gain_high = (
            mean_ci(paired_utility_gain)
            if paired_utility_gain
            else (math.nan, math.nan, math.nan)
        )
        row: dict[str, object] = {
            "group": label,
            "depth": depth,
            "positions": len(group),
            "mean_regret_points": mean,
            "ci95_low": low,
            "ci95_high": high,
            "mean_win_regret": statistics.fmean(win_regrets),
            "mean_utility_regret": utility_mean,
            "utility_ci95_low": utility_low,
            "utility_ci95_high": utility_high,
            "exact_rate": statistics.fmean(
                point.spread_regret == 0.0 for point in group
            ),
            "median_nodes": statistics.median(point.nodes for point in group),
            "median_seconds": statistics.median(
                point.elapsed_seconds for point in group
            ),
            "mean_scheduled_cores": statistics.fmean(
                point.scheduled_cores for point in group
            ),
            "scheduled_core_cv": robust_cv(
                [point.scheduled_cores for point in group]
            ),
            "marginal_regret_gain": gain,
            "marginal_positions": len(paired_gain),
            "marginal_ci95_low": gain_low,
            "marginal_ci95_high": gain_high,
            "marginal_utility_gain": utility_gain,
            "marginal_utility_ci95_low": utility_gain_low,
            "marginal_utility_ci95_high": utility_gain_high,
            "median_incremental_nodes": (
                statistics.median(incremental_nodes)
                if incremental_nodes
                else math.nan
            ),
            "median_incremental_seconds": (
                statistics.median(incremental_seconds)
                if incremental_seconds
                else math.nan
            ),
            "median_ebf": statistics.median(ebfs) if ebfs else math.nan,
            "p75_ebf": percentile(ebfs, 0.75),
            "p90_ebf": percentile(ebfs, 0.90),
            "p95_ebf": percentile(ebfs, 0.95),
            "p99_ebf": percentile(ebfs, 0.99),
        }
        output.append(row)
        print(
            "ENDGAME_VALUE "
            f"group={label} depth={depth} n={len(group)} "
            f"regret_points={mean:.5f} ci95=[{low:.5f},{high:.5f}] "
            f"win_regret={row['mean_win_regret']:.5f} "
            f"utility_regret={utility_mean:.6f} "
            f"utility_ci95=[{utility_low:.6f},{utility_high:.6f}] "
            f"exact={row['exact_rate']:.4f} "
            f"median_nodes={row['median_nodes']:.0f} "
            f"median_seconds={row['median_seconds']:.5f} "
            f"marginal_n={row['marginal_positions']} "
            f"marginal_gain={gain:+.5f} "
            f"marginal_ci95=[{gain_low:+.5f},{gain_high:+.5f}] "
            f"marginal_utility={utility_gain:+.6f} "
            f"incremental_nodes={row['median_incremental_nodes']:.0f} "
            f"ebf50/75/90/95/99={row['median_ebf']:.3f}/"
            f"{row['p75_ebf']:.3f}/{row['p90_ebf']:.3f}/"
            f"{row['p95_ebf']:.3f}/{row['p99_ebf']:.3f}"
        )
    return output


def report_stability_value(points: list[Point]) -> None:
    """Condition the next completed depth's value on observable stability."""

    by_position: dict[tuple[int, int], list[Point]] = {}
    for point in points:
        by_position.setdefault((point.corpus, point.position), []).append(
            point
        )
    observations: dict[tuple[int, str, bool], list[tuple[float, float, bool, int]]] = {}
    settling: dict[tuple[int, bool], list[tuple[float, float, bool, int]]] = {}
    for sequence in by_position.values():
        sequence.sort(key=lambda point: point.depth)
        for index in range(1, len(sequence) - 1):
            previous = sequence[index - 1]
            current = sequence[index]
            following = sequence[index + 1]
            # Missing depths occur after forced-pass shortcuts. Do not call
            # either side of a non-adjacent jump a one-depth stability signal.
            if (
                current.depth != previous.depth + 1
                or following.depth != current.depth + 1
                or not current.scored
                or not following.scored
            ):
                continue
            if current.tiny_move != previous.tiny_move:
                move_state = "changed"
            elif (
                index >= 2
                and previous.tiny_move == sequence[index - 2].tiny_move
            ):
                move_state = "stable2plus"
            else:
                move_state = "stable1"
            value_stable = current.search_value == previous.search_value
            observation = (
                current.utility_regret,
                current.utility_regret - following.utility_regret,
                current.tiny_move != following.tiny_move,
                following.nodes - current.nodes,
            )
            observations.setdefault(
                (current.depth, move_state, value_stable), []
            ).append(observation)
            settled = move_state != "changed" and value_stable
            settling.setdefault((current.depth, settled), []).append(
                observation
            )

    for (depth, move_state, value_stable), group in sorted(
        observations.items()
    ):
        current_regret = [item[0] for item in group]
        next_gain = [item[1] for item in group]
        gain, low, high = mean_ci(next_gain)
        print(
            "ENDGAME_STABILITY "
            f"depth={depth} move_state={move_state} "
            f"value_stable={int(value_stable)} n={len(group)} "
            f"current_utility_regret={statistics.fmean(current_regret):.6f} "
            f"next_depth_gain={gain:+.6f} "
            f"next_gain_ci95=[{low:+.6f},{high:+.6f}] "
            f"next_move_change_rate="
            f"{statistics.fmean(item[2] for item in group):.4f} "
            f"next_nodes50={statistics.median(item[3] for item in group):.0f}"
        )

    for (depth, settled), group in sorted(settling.items()):
        current_regret = [item[0] for item in group]
        next_gain = [item[1] for item in group]
        next_nodes = [item[3] for item in group]
        gain, low, high = mean_ci(next_gain)
        print(
            "ENDGAME_SETTLING "
            f"depth={depth} settled={int(settled)} n={len(group)} "
            f"current_utility_regret={statistics.fmean(current_regret):.6f} "
            f"next_depth_gain={gain:+.6f} "
            f"next_gain_ci95=[{low:+.6f},{high:+.6f}] "
            f"next_move_change_rate="
            f"{statistics.fmean(item[2] for item in group):.4f} "
            f"next_nodes50/75/90={statistics.median(next_nodes):.0f}/"
            f"{percentile(next_nodes, 0.75):.0f}/"
            f"{percentile(next_nodes, 0.90):.0f}"
        )


def report_interval_quietness(points: list[Point]) -> None:
    by_position: dict[tuple[int, int], list[Point]] = {}
    for point in points:
        by_position.setdefault((point.corpus, point.position), []).append(
            point
        )
    interval_cores: list[float] = []
    interval_nps: list[float] = []
    for sequence in by_position.values():
        sequence.sort(key=lambda point: point.depth)
        previous_nodes = 0
        previous_elapsed = 0.0
        previous_cpu = 0.0
        for point in sequence:
            elapsed = point.elapsed_seconds - previous_elapsed
            cpu = point.cpu_seconds - previous_cpu
            nodes = point.nodes - previous_nodes
            if elapsed > 0.0 and nodes > 0 and cpu >= 0.0:
                interval_cores.append(cpu / elapsed)
                interval_nps.append(nodes / elapsed)
            previous_nodes = point.nodes
            previous_elapsed = point.elapsed_seconds
            previous_cpu = point.cpu_seconds
    print(
        "ENDGAME_RATE_DIAGNOSTIC "
        f"intervals={len(interval_cores)} "
        f"median_nps={statistics.median(interval_nps):.1f} "
        f"nps_cv={robust_cv(interval_nps):.4f} "
        f"median_scheduled_cores={statistics.median(interval_cores):.3f} "
        f"scheduled_core_cv={robust_cv(interval_cores):.4f} "
        "note=nps_cv_includes_depth_and_position_work_mix"
    )


def report_admission_value_prior(
    points: list[Point],
    target_depth: int,
    settled_prior: float,
    unsettled_prior: float,
    missing_prior: float,
) -> None:
    """Audit a frozen next-depth value prior without refitting it."""

    priors = {
        "settled": settled_prior,
        "unsettled": unsettled_prior,
        "missing": missing_prior,
    }
    if target_depth < 3 or any(
        not math.isfinite(value) or value < 0.0 for value in priors.values()
    ):
        raise ValueError("admission value priors must be finite and nonnegative")

    by_position: dict[tuple[int, int], dict[int, Point]] = {}
    for point in points:
        by_position.setdefault((point.corpus, point.position), {})[
            point.depth
        ] = point
    records: list[tuple[str, float, float]] = []
    for depths in by_position.values():
        current = depths.get(target_depth - 1)
        following = depths.get(target_depth)
        if (
            current is None
            or following is None
            or not current.scored
            or not following.scored
        ):
            continue
        previous = depths.get(target_depth - 2)
        if previous is None:
            state = "missing"
        elif (
            current.tiny_move == previous.tiny_move
            and current.search_value == previous.search_value
        ):
            state = "settled"
        else:
            state = "unsettled"
        actual_gain = current.utility_regret - following.utility_regret
        records.append((state, priors[state], actual_gain))

    if not records:
        print(
            f"ENDGAME_PRIOR_AUDIT depth={target_depth} n=0 "
            "status=no_observations"
        )
        return

    for state in ("all", "settled", "unsettled", "missing"):
        group = (
            records
            if state == "all"
            else [record for record in records if record[0] == state]
        )
        if not group:
            continue
        predictions = [record[1] for record in group]
        actuals = [record[2] for record in group]
        errors = [
            prediction - actual
            for prediction, actual in zip(predictions, actuals, strict=True)
        ]
        actual_mean, actual_low, actual_high = mean_ci(actuals)
        print(
            "ENDGAME_PRIOR_AUDIT "
            f"depth={target_depth} state={state} n={len(group)} "
            f"predicted_mean={statistics.fmean(predictions):.6f} "
            f"actual_mean={actual_mean:.6f} "
            f"actual_ci95=[{actual_low:.6f},{actual_high:.6f}] "
            f"bias={statistics.fmean(errors):+.6f} "
            f"mae={statistics.fmean(abs(error) for error in errors):.6f} "
            f"rmse={math.sqrt(statistics.fmean(error * error for error in errors)):.6f} "
            f"gain_events={sum(actual > 0.0 for actual in actuals)} "
            f"spread_or_larger_events="
            f"{sum(actual >= 1.0e-4 for actual in actuals)} "
            f"max_gain={max(actuals):.6f}"
        )


def report_capped_tail(summaries: list[RunSummary]) -> None:
    if not summaries:
        return
    complete = [summary for summary in summaries if summary.completed]
    truncated = [summary for summary in summaries if not summary.completed]
    depths = [summary.depth for summary in truncated]
    print(
        "ENDGAME_CAP "
        f"attempted={len(summaries)} complete={len(complete)} "
        f"truncated={len(truncated)} "
        f"completion_rate={len(complete) / len(summaries):.4f} "
        f"truncated_depth50={percentile(depths, 0.50):.1f} "
        f"truncated_depth90={percentile(depths, 0.90):.1f} "
        f"truncated_nodes50={percentile([s.nodes for s in truncated], 0.50):.0f} "
        f"truncated_nps50="
        f"{percentile([s.rate for s in truncated], 0.50):.1f} "
        f"truncated_cores50="
        f"{percentile([s.scheduled_cores for s in truncated], 0.50):.3f}"
    )
    for rack_tiles in sorted({summary.rack_tiles for summary in summaries}):
        group = [
            summary for summary in summaries if summary.rack_tiles == rack_tiles
        ]
        group_complete = sum(summary.completed for summary in group)
        print(
            "ENDGAME_CAP_STRATUM "
            f"rack_tiles={rack_tiles} n={len(group)} "
            f"complete={group_complete} "
            f"completion_rate={group_complete / len(group):.4f}"
        )


def report_judge_capped_tail(summaries: list[JudgeSummary]) -> None:
    if not summaries:
        return
    complete = [summary for summary in summaries if summary.completed]
    truncated = [summary for summary in summaries if not summary.completed]
    print(
        "ENDGAME_JUDGE_CAP "
        f"attempted={len(summaries)} complete={len(complete)} "
        f"truncated={len(truncated)} "
        f"completion_rate={len(complete) / len(summaries):.4f} "
        f"truncated_actual_depth50="
        f"{percentile([s.actual_depth for s in truncated], 0.50):.1f} "
        f"truncated_actual_depth90="
        f"{percentile([s.actual_depth for s in truncated], 0.90):.1f} "
        f"truncated_nodes50="
        f"{percentile([s.nodes for s in truncated], 0.50):.0f} "
        f"truncated_seconds50="
        f"{percentile([s.elapsed_seconds for s in truncated], 0.50):.3f} "
        f"truncated_nps50="
        f"{percentile([s.rate for s in truncated], 0.50):.1f} "
        f"truncated_cores50="
        f"{percentile([s.scheduled_cores for s in truncated if s.cpu_seconds > 0.0], 0.50):.3f}"
    )
    for rack_tiles in sorted({summary.rack_tiles for summary in summaries}):
        group = [
            summary for summary in summaries if summary.rack_tiles == rack_tiles
        ]
        group_complete = sum(summary.completed for summary in group)
        print(
            "ENDGAME_JUDGE_CAP_STRATUM "
            f"rack_tiles={rack_tiles} n={len(group)} "
            f"complete={group_complete} "
            f"completion_rate={group_complete / len(group):.4f}"
        )


def report_repeated_run_quietness(
    summaries: list[RunSummary],
    max_cv: float,
    max_drift: float,
    max_core_cv: float,
) -> None:
    by_position: dict[int, list[RunSummary]] = {}
    for summary in summaries:
        by_position.setdefault(summary.position, []).append(summary)
    normalized_in_order: list[tuple[tuple[int, int], float]] = []
    scheduled_cores: list[float] = []
    matched_positions = 0
    for position, group in by_position.items():
        if len({summary.source for summary in group}) < 2:
            continue
        # A capped search and a completed search are not identical workloads.
        # Only compare a position when every repeat reached the same terminal
        # state and completed depth.
        signatures = {
            (summary.completed, summary.depth) for summary in group
        }
        if len(signatures) != 1:
            continue
        matched_positions += 1
        center = statistics.median(summary.rate for summary in group)
        core_center = statistics.median(
            summary.scheduled_cores for summary in group
        )
        for summary in group:
            normalized_in_order.append(
                ((summary.source, position), summary.rate / center)
            )
            if core_center > 0.0:
                scheduled_cores.append(
                    summary.scheduled_cores / core_center
                )
    normalized_in_order.sort()
    normalized = [value for _, value in normalized_in_order]
    rate_cv = robust_cv(normalized)
    drift = half_drift(normalized)
    core_cv = robust_cv(scheduled_cores)
    source_count = len({summary.source for summary in summaries})
    enough = (
        source_count >= 3 and matched_positions >= 3 and len(normalized) >= 9
    )
    quiet = (
        enough
        and rate_cv <= max_cv
        and abs(drift) <= max_drift
        and core_cv <= max_core_cv
    )
    status = "yes" if quiet else ("unknown" if not enough else "no")
    print(
        "ENDGAME_SENTINEL "
        f"runs={source_count} matched_positions={matched_positions} "
        f"probes={len(normalized)} normalized_rate_cv={rate_cv:.4f} "
        f"late_vs_early={drift:+.4f} normalized_core_cv={core_cv:.4f} "
        f"quiet={status}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--quality-log",
        action="append",
        type=Path,
        help=(
            "quality log fragment; repeat for resumed runs. When omitted, "
            "the first positional log supplies quality"
        ),
    )
    parser.add_argument(
        "--quality-corpus",
        action="append",
        help=(
            "comma-separated fragments for one independent quality corpus; "
            "repeat for independent corpora whose position indices may "
            "overlap"
        ),
    )
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--max-sentinel-cv", type=float, default=0.10)
    parser.add_argument("--max-sentinel-drift", type=float, default=0.10)
    parser.add_argument("--max-core-cv", type=float, default=0.10)
    parser.add_argument(
        "--audit-admission-prior",
        action="store_true",
        help="audit a frozen settled/unsettled next-depth value prior",
    )
    parser.add_argument("--prior-target-depth", type=int, default=5)
    parser.add_argument(
        "--prior-settled", type=float, default=0.000005649717514124294
    )
    parser.add_argument(
        "--prior-unsettled", type=float, default=0.000026966292134831465
    )
    parser.add_argument("--prior-missing", type=float, default=1.0e-6)
    args = parser.parse_args()
    if args.quality_log and args.quality_corpus:
        raise ValueError(
            "--quality-log and --quality-corpus are mutually exclusive"
        )

    # Quality fragments are merged by source position, so a safely resumed run
    # remains one independent observation rather than pseudoreplication.
    # Independent corpora retain a separate identity even when they both use
    # positions 0..N. Positional logs are timing repeats; when no quality
    # option is supplied, the first positional log serves both roles for
    # backward compatibility.
    if args.quality_corpus:
        quality_corpora = [
            [Path(item) for item in value.split(",") if item]
            for value in args.quality_corpus
        ]
        if any(not corpus for corpus in quality_corpora):
            raise ValueError("--quality-corpus cannot be empty")
    else:
        quality_corpora = [args.quality_log or [args.logs[0]]]
    points = [
        point
        for corpus_index, quality_logs in enumerate(quality_corpora)
        for point in load_points(corpus_index, quality_logs)
    ]
    summaries = load_run_summaries(args.logs)
    judge_summaries = load_judge_summaries(args.logs)
    first_run_summaries: list[RunSummary] = []
    for quality_logs in quality_corpora:
        quality_summary_by_position: dict[int, RunSummary] = {}
        for summary in load_run_summaries(quality_logs):
            quality_summary_by_position[summary.position] = summary
        first_run_summaries.extend(quality_summary_by_position.values())
    if not points and not first_run_summaries:
        raise ValueError("no EGCURVE observations")

    output: list[dict[str, object]] = []
    if points:
        output = summarize("all", points)
        for rack_tiles in sorted({point.rack_tiles for point in points}):
            output.extend(
                summarize(
                    f"tiles{rack_tiles}",
                    [
                        point
                        for point in points
                        if point.rack_tiles == rack_tiles
                    ],
                )
            )
        report_stability_value(points)
        if args.audit_admission_prior:
            report_admission_value_prior(
                points,
                args.prior_target_depth,
                args.prior_settled,
                args.prior_unsettled,
                args.prior_missing,
            )
        report_interval_quietness(points)
    report_capped_tail(first_run_summaries)
    report_judge_capped_tail(judge_summaries)
    report_repeated_run_quietness(
        summaries,
        args.max_sentinel_cv,
        args.max_sentinel_drift,
        args.max_core_cv,
    )

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            fieldnames = list(output[0]) if output else []
            writer = csv.DictWriter(stream, fieldnames=fieldnames)
            if fieldnames:
                writer.writeheader()
                writer.writerows(output)


if __name__ == "__main__":
    main()

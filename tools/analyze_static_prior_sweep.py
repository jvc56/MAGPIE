#!/usr/bin/env python3
"""Analyze paired static-prior oracle logs emitted by klv3oracle."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
from pathlib import Path
import re
import statistics
import sys


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")


def betacf(a: float, b: float, x: float) -> float:
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < 3e-14:
        d = 3e-14
    d = 1.0 / d
    h = d
    for m in range(1, 201):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < 3e-14:
            d = 3e-14
        c = 1.0 + aa / c
        if abs(c) < 3e-14:
            c = 3e-14
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < 3e-14:
            d = 3e-14
        c = 1.0 + aa / c
        if abs(c) < 3e-14:
            c = 3e-14
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 3e-14:
            return h
    raise RuntimeError("incomplete beta continued fraction did not converge")


def regularized_beta(x: float, a: float, b: float) -> float:
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    front = math.exp(
        math.lgamma(a + b)
        - math.lgamma(a)
        - math.lgamma(b)
        + a * math.log(x)
        + b * math.log1p(-x)
    )
    if x < (a + 1.0) / (a + b + 2.0):
        return front * betacf(a, b, x) / a
    return 1.0 - front * betacf(b, a, 1.0 - x) / b


def student_two_sided_p(t_value: float, degrees_freedom: int) -> float:
    if degrees_freedom <= 0:
        return 1.0
    x = degrees_freedom / (degrees_freedom + t_value * t_value)
    return min(1.0, regularized_beta(x, degrees_freedom / 2.0, 0.5))


def student_quantile_975(degrees_freedom: int) -> float:
    if degrees_freedom <= 0:
        return math.nan
    low, high = 0.0, 20.0
    for _ in range(80):
        middle = (low + high) / 2.0
        if student_two_sided_p(middle, degrees_freedom) > 0.05:
            low = middle
        else:
            high = middle
    return (low + high) / 2.0


@dataclass
class Estimate:
    n: int
    mean: float
    sem: float
    low: float
    high: float
    p: float


def estimate(values: list[float]) -> Estimate:
    n = len(values)
    if n == 0:
        return Estimate(0, math.nan, math.nan, math.nan, math.nan, math.nan)
    mean = statistics.fmean(values)
    if n == 1:
        return Estimate(1, mean, math.nan, math.nan, math.nan, math.nan)
    sem = statistics.stdev(values) / math.sqrt(n)
    if sem == 0:
        p = 1.0 if mean == 0 else 0.0
        return Estimate(n, mean, 0.0, mean, mean, p)
    critical = student_quantile_975(n - 1)
    p = student_two_sided_p(abs(mean / sem), n - 1)
    return Estimate(n, mean, sem, mean - critical * sem, mean + critical * sem, p)


def exact_sign_p(wins: int, losses: int) -> float:
    n = wins + losses
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, k) for k in range(0, min(wins, losses) + 1))
    return min(1.0, 2.0 * tail / (2**n))


def cluster_estimate(values: list[float], clusters: list[int]) -> Estimate:
    if len(values) != len(clusters):
        raise ValueError("cluster labels and values differ in length")
    n = len(values)
    unique_clusters = sorted(set(clusters))
    cluster_count = len(unique_clusters)
    if n < 2 or cluster_count < 2:
        return estimate(values)
    mean = statistics.fmean(values)
    score_sums = {
        cluster: sum(
            value - mean
            for value, label in zip(values, clusters, strict=True)
            if label == cluster
        )
        for cluster in unique_clusters
    }
    variance = (
        cluster_count
        / (cluster_count - 1)
        * sum(score * score for score in score_sums.values())
        / (n * n)
    )
    sem = math.sqrt(variance)
    if sem == 0:
        p = 1.0 if mean == 0 else 0.0
        return Estimate(n, mean, 0.0, mean, mean, p)
    critical = student_quantile_975(cluster_count - 1)
    p = student_two_sided_p(abs(mean / sem), cluster_count - 1)
    return Estimate(n, mean, sem, mean - critical * sem, mean + critical * sem, p)


def cluster_aggregate_estimate(sums: list[float], counts: list[int]) -> Estimate:
    if len(sums) != len(counts):
        raise ValueError("cluster sums and counts differ in length")
    cluster_count = len(sums)
    n = sum(counts)
    if cluster_count < 2 or n < 2:
        expanded = [
            cluster_sum / count
            for cluster_sum, count in zip(sums, counts, strict=True)
            if count > 0
        ]
        return estimate(expanded)
    mean = sum(sums) / n
    scores = [
        cluster_sum - count * mean
        for cluster_sum, count in zip(sums, counts, strict=True)
    ]
    variance = (
        cluster_count
        / (cluster_count - 1)
        * sum(score * score for score in scores)
        / (n * n)
    )
    sem = math.sqrt(variance)
    if sem == 0:
        p = 1.0 if mean == 0 else 0.0
        return Estimate(n, mean, 0.0, mean, mean, p)
    critical = student_quantile_975(cluster_count - 1)
    p = student_two_sided_p(abs(mean / sem), cluster_count - 1)
    return Estimate(n, mean, sem, mean - critical * sem, mean + critical * sem, p)


def parse_fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


@dataclass
class Run:
    path: Path
    weight: float
    source_positions: int
    agreements: int
    utility: list[float]
    oracle_sem: list[float]
    position: list[int]
    win: list[float]
    spread: list[float]
    bag: list[int]
    game: list[int]
    static_equity_delta: list[float]
    game_summary_ids: list[int]
    game_source_counts: list[int]
    game_utility_sums: list[float]
    game_win_sums: list[float]
    game_spread_sums: list[float]
    baseline_nomination_iterations: int
    prior_nomination_iterations: int
    baseline_nomination_seconds: float
    prior_nomination_seconds: float
    protocol: tuple[int, int, int, int, int, int]
    model_protocol: tuple[str, str, str, str]
    accounting_ok: bool
    complete: bool
    replacement_outer_samples: int | None


def raw_static_equity(encoded: str) -> float:
    fields = [int(value) for value in encoded.split(",")]
    if len(fields) < 8:
        raise ValueError(f"malformed raw move: {encoded}")
    # Pass equity is an ordering sentinel, not a numeric static contribution.
    return 0.0 if fields[0] == 3 else fields[7] / 1000.0


def read_run(path: Path, allow_partial: bool = False) -> Run:
    weight: float | None = None
    source_positions = 0
    agreements = 0
    utility: list[float] = []
    oracle_sem: list[float] = []
    position: list[int] = []
    win: list[float] = []
    spread: list[float] = []
    bag: list[int] = []
    game: list[int] = []
    static_equity_delta: list[float] = []
    game_summary_ids: list[int] = []
    game_source_counts: list[int] = []
    game_utility_sums: list[float] = []
    game_win_sums: list[float] = []
    game_spread_sums: list[float] = []
    baseline_nomination_iterations = 0
    prior_nomination_iterations = 0
    baseline_nomination_seconds = 0.0
    prior_nomination_seconds = 0.0
    protocol: tuple[int, int, int, int, int, int] | None = None
    model_protocol: tuple[str, str, str, str] | None = None
    collect_from_position: int | None = None
    last_position: int | None = None
    complete = False
    accounting_ok = True
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("KLV3_STATIC_PRIOR_CONFIG "):
                fields = parse_fields(line)
                weight = float(fields["candidate_prior"])
                model_protocol = (
                    fields["spread_forecast"],
                    fields["nomination_klv"],
                    fields["judge_klv2"],
                    fields["judge_klv3"],
                )
            elif line.startswith("KLV3_EQUAL_CONFIG "):
                fields = parse_fields(line)
                collect_from_position = int(fields["collect_from_position"])
                protocol = (
                    int(fields["samples_per_arm"]),
                    int(fields["min_samples_per_arm"]),
                    int(fields["outer_samples_per_candidate"]),
                    int(fields["outer_plies"]),
                    int(fields["nested_samples_per_candidate"]),
                    int(fields["nomination_threads"]),
                )
            elif line.startswith("KLV3_EQUAL_POSITION "):
                fields = parse_fields(line)
                last_position = int(fields["position"])
                position.append(last_position)
                utility.append(float(fields["klv3_minus_klv2"]))
                oracle_sem.append(float(fields["sem"]))
                win.append(float(fields["win_delta"]))
                spread.append(float(fields["spread_delta"]))
                bag.append(int(fields["bag"]))
                game.append(int(fields["game"]))
                static_equity_delta.append(
                    raw_static_equity(fields["klv3_move_raw"])
                    - raw_static_equity(fields["klv2_move_raw"])
                )
                accounting_ok &= fields["klv2_iterations"] == fields["klv3_iterations"]
            elif line.startswith("KLV3_EQUAL_GAME "):
                fields = parse_fields(line)
                game_summary_ids.append(int(fields["game"]))
                game_source_counts.append(int(fields["source_positions"]))
                game_utility_sums.append(float(fields["utility_delta_sum"]))
                game_win_sums.append(float(fields["win_delta_sum"]))
                game_spread_sums.append(float(fields["spread_delta_sum"]))
            elif line.startswith("KLV3_EQUAL_DONE "):
                fields = parse_fields(line)
                source_positions = int(fields["source_positions"])
                agreements = int(fields["agreements"])
                complete = True
            elif line.startswith("KLV3_EQUAL_SUMMARY "):
                fields = parse_fields(line)
                accounting_ok &= fields["klv2_iterations"] == fields["klv3_iterations"]
                baseline_nomination_iterations = int(fields["klv2_iterations"])
                prior_nomination_iterations = int(fields["klv3_iterations"])
                baseline_nomination_seconds = (
                    baseline_nomination_iterations
                    / float(fields["klv2_iters_per_second"])
                )
                prior_nomination_seconds = (
                    prior_nomination_iterations
                    / float(fields["klv3_iters_per_second"])
                )
                if source_positions == 0:
                    source_positions = int(fields["source_positions"])
                    agreements = int(fields["agreements"])
    if weight is None:
        raise ValueError(f"{path}: missing KLV3_STATIC_PRIOR_CONFIG")
    if protocol is None:
        raise ValueError(f"{path}: missing KLV3_EQUAL_CONFIG")
    if model_protocol is None:
        raise ValueError(f"{path}: missing static-prior model protocol")
    if allow_partial and not complete:
        if collect_from_position is None or last_position is None:
            raise ValueError(f"{path}: no flushed disagreement to recover")
        source_positions = max(
            last_position - collect_from_position + 1,
            sum(game_source_counts),
        )
        agreements = source_positions - len(utility)
        # The current game summary is emitted only when a game finishes. A
        # failed mid-game prefix therefore cannot support clustered marginal
        # inference, though its flushed disagreements remain usable.
        if sum(game_source_counts) != source_positions:
            game_source_counts = []
            game_summary_ids = []
            game_utility_sums = []
            game_win_sums = []
            game_spread_sums = []
    elif not complete:
        raise ValueError(f"{path}: run is incomplete; pass --allow-partial to recover it")
    if source_positions == 0:
        raise ValueError(f"{path}: no completed source positions")
    if source_positions != agreements + len(utility):
        raise ValueError(
            f"{path}: source accounting mismatch: {source_positions} != "
            f"{agreements} + {len(utility)}"
        )
    if game_source_counts and sum(game_source_counts) != source_positions:
        raise ValueError(
            f"{path}: game/source accounting mismatch: "
            f"{sum(game_source_counts)} != {source_positions}"
        )
    return Run(
        path,
        weight,
        source_positions,
        agreements,
        utility,
        oracle_sem,
        position,
        win,
        spread,
        bag,
        game,
        static_equity_delta,
        game_summary_ids,
        game_source_counts,
        game_utility_sums,
        game_win_sums,
        game_spread_sums,
        baseline_nomination_iterations,
        prior_nomination_iterations,
        baseline_nomination_seconds,
        prior_nomination_seconds,
        protocol,
        model_protocol,
        accounting_ok,
        complete,
        None,
    )


def pool_runs(runs: list[Run]) -> list[Run]:
    by_weight: dict[float, list[Run]] = {}
    for run in runs:
        by_weight.setdefault(run.weight, []).append(run)
    pooled: list[Run] = []
    for weight, group in by_weight.items():
        protocols = {run.protocol for run in group}
        if len(protocols) != 1:
            raise ValueError(
                f"weight {weight:g}: refusing to pool different protocols: "
                f"{sorted(protocols)}"
            )
        model_protocols = {run.model_protocol for run in group}
        if len(model_protocols) != 1:
            raise ValueError(
                f"weight {weight:g}: refusing to pool different model "
                f"protocols: {sorted(model_protocols)}"
            )
        source_keys = [
            (game, position)
            for run in group
            for game, position in zip(run.game, run.position, strict=True)
        ]
        if len(source_keys) != len(set(source_keys)):
            raise ValueError(
                f"weight {weight:g}: refusing to pool overlapping source corpora"
            )
        complete_game_accounting = all(
            run.game_source_counts
            and sum(run.game_source_counts) == run.source_positions
            for run in group
        )
        pooled.append(
            Run(
                path=Path("+".join(str(run.path) for run in group)),
                weight=weight,
                source_positions=sum(run.source_positions for run in group),
                agreements=sum(run.agreements for run in group),
                utility=[value for run in group for value in run.utility],
                oracle_sem=[value for run in group for value in run.oracle_sem],
                position=[value for run in group for value in run.position],
                win=[value for run in group for value in run.win],
                spread=[value for run in group for value in run.spread],
                bag=[value for run in group for value in run.bag],
                game=[value for run in group for value in run.game],
                static_equity_delta=[
                    value for run in group for value in run.static_equity_delta
                ],
                game_summary_ids=(
                    [value for run in group for value in run.game_summary_ids]
                    if complete_game_accounting
                    else []
                ),
                game_source_counts=(
                    [value for run in group for value in run.game_source_counts]
                    if complete_game_accounting
                    else []
                ),
                game_utility_sums=(
                    [value for run in group for value in run.game_utility_sums]
                    if complete_game_accounting
                    else []
                ),
                game_win_sums=(
                    [value for run in group for value in run.game_win_sums]
                    if complete_game_accounting
                    else []
                ),
                game_spread_sums=(
                    [value for run in group for value in run.game_spread_sums]
                    if complete_game_accounting
                    else []
                ),
                baseline_nomination_iterations=sum(
                    run.baseline_nomination_iterations for run in group
                ),
                prior_nomination_iterations=sum(
                    run.prior_nomination_iterations for run in group
                ),
                baseline_nomination_seconds=sum(
                    run.baseline_nomination_seconds for run in group
                ),
                prior_nomination_seconds=sum(
                    run.prior_nomination_seconds for run in group
                ),
                protocol=group[0].protocol,
                model_protocol=group[0].model_protocol,
                accounting_ok=all(run.accounting_ok for run in group),
                complete=all(run.complete for run in group),
                replacement_outer_samples=(
                    group[0].replacement_outer_samples
                    if len(
                        {
                            run.replacement_outer_samples
                            for run in group
                        }
                    )
                    == 1
                    else None
                ),
            )
        )
    return sorted(pooled, key=lambda run: run.weight)


def all_position(values: list[float], source_positions: int) -> list[float]:
    return values + [0.0] * (source_positions - len(values))


def replace_with_nested_oracle(run: Run, nested_path: Path) -> None:
    replacements: dict[int, tuple[float, float, float, float]] = {}
    outer_samples: int | None = None
    spread_forecast: str | None = None
    complete = False
    with nested_path.open(encoding="utf-8", errors="strict") as stream:
        for line in stream:
            if line.startswith("KLV3_NESTED_CONFIG "):
                fields = parse_fields(line)
                outer_samples = int(fields["outer_samples_per_candidate"])
                spread_forecast = fields.get("judge_spread_forecast")
            elif line.startswith("KLV3_NESTED_POSITION "):
                fields = parse_fields(line)
                position = int(fields["position"])
                klv2_root = int(fields["klv2_root"])
                klv3_root = int(fields["klv3_root"])
                replacements[position] = (
                    float(fields["klv3_minus_klv2"]),
                    float(fields["klv3_minus_klv2_sem"]),
                    float(fields[f"root{klv3_root}_win"])
                    - float(fields[f"root{klv2_root}_win"]),
                    float(fields[f"root{klv3_root}_spread"])
                    - float(fields[f"root{klv2_root}_spread"]),
                )
            elif line.startswith("KLV3_NESTED_DONE "):
                complete = True
    if outer_samples is None:
        raise ValueError(f"{nested_path}: missing KLV3_NESTED_CONFIG")
    if spread_forecast is not None and spread_forecast != run.model_protocol[0]:
        raise ValueError(
            f"{nested_path}: judge spread forecast {spread_forecast} does not "
            f"match nomination forecast {run.model_protocol[0]}"
        )
    if not complete:
        raise ValueError(f"{nested_path}: nested replacement run is incomplete")
    expected = set(run.position)
    actual = set(replacements)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise ValueError(
            f"{nested_path}: replacement positions do not match {run.path}; "
            f"missing={missing[:8]}, unexpected={unexpected[:8]}"
        )
    for index, position in enumerate(run.position):
        utility, sem, win, spread = replacements[position]
        run.utility[index] = utility
        run.oracle_sem[index] = sem
        run.win[index] = win
        run.spread[index] = spread
    if run.game_source_counts:
        utility_by_game = {
            game: sum(
                value
                for value, value_game in zip(run.utility, run.game, strict=True)
                if value_game == game
            )
            for game in run.game_summary_ids
        }
        win_by_game = {
            game: sum(
                value
                for value, value_game in zip(run.win, run.game, strict=True)
                if value_game == game
            )
            for game in run.game_summary_ids
        }
        spread_by_game = {
            game: sum(
                value
                for value, value_game in zip(run.spread, run.game, strict=True)
                if value_game == game
            )
            for game in run.game_summary_ids
        }
        run.game_utility_sums = [
            utility_by_game[game] for game in run.game_summary_ids
        ]
        run.game_win_sums = [win_by_game[game] for game in run.game_summary_ids]
        run.game_spread_sums = [
            spread_by_game[game] for game in run.game_summary_ids
        ]
    run.replacement_outer_samples = outer_samples


def holm_adjust(p_values: list[float]) -> list[float]:
    finite_p_values = [
        value if math.isfinite(value) else 1.0 for value in p_values
    ]
    order = sorted(range(len(finite_p_values)), key=finite_p_values.__getitem__)
    adjusted = [1.0] * len(p_values)
    running = 0.0
    total = len(p_values)
    for rank, index in enumerate(order):
        running = max(
            running,
            min(1.0, (total - rank) * finite_p_values[index]),
        )
        adjusted[index] = running
    return adjusted


def fmt_est(value: Estimate, digits: int = 6) -> str:
    if value.n < 2:
        return "insufficient"
    return (
        f"{value.mean:+.{digits}f} "
        f"[{value.low:+.{digits}f}, {value.high:+.{digits}f}]"
    )


def bag_band(value: int) -> str:
    if value >= 70:
        return "70+"
    if value >= 50:
        return "50–69"
    return "29–49"


def correlation(xs: list[float], ys: list[float]) -> float:
    if len(xs) < 2:
        return math.nan
    x_mean = statistics.fmean(xs)
    y_mean = statistics.fmean(ys)
    numerator = sum(
        (x - x_mean) * (y - y_mean) for x, y in zip(xs, ys, strict=True)
    )
    x_ss = sum((x - x_mean) ** 2 for x in xs)
    y_ss = sum((y - y_mean) ** 2 for y in ys)
    if x_ss == 0 or y_ss == 0:
        return math.nan
    return numerator / math.sqrt(x_ss * y_ss)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pool-by-weight", action="store_true")
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument(
        "--replacement-oracle",
        action="append",
        default=[],
        metavar="SOURCE_LOG=NESTED_LOG",
    )
    parser.add_argument(
        "--aa-reference",
        type=Path,
        help=(
            "weight-0 run over the same corpus; report a game-paired "
            "all-source difference-in-differences diagnostic"
        ),
    )
    parser.add_argument("logs", type=Path, nargs="+")
    args = parser.parse_args()
    runs = sorted(
        (read_run(path, allow_partial=args.allow_partial) for path in args.logs),
        key=lambda run: run.weight,
    )
    runs_by_path = {str(run.path): run for run in runs}
    for replacement in args.replacement_oracle:
        if "=" not in replacement:
            parser.error(
                "--replacement-oracle must be SOURCE_LOG=NESTED_LOG"
            )
        source_name, nested_name = replacement.split("=", 1)
        run = runs_by_path.get(source_name)
        if run is None:
            parser.error(
                f"replacement source {source_name} is not one of the input logs"
            )
        replace_with_nested_oracle(run, Path(nested_name))
    if args.pool_by_weight:
        runs = pool_runs(runs)
    all_utility = [
        cluster_aggregate_estimate(run.game_utility_sums, run.game_source_counts)
        if run.game_source_counts
        else estimate(all_position(run.utility, run.source_positions))
        for run in runs
    ]
    conditional_utility = [
        cluster_estimate(run.utility, run.game) for run in runs
    ]
    adjusted = holm_adjust([item.p for item in conditional_utility])

    print(
        "| prior weight | source positions | disagreements | rate | "
        "utility / disagreement (game-clustered 95% CI) | "
        "utility / source position (95% CI) | "
        "cluster p / Holm | root wins |"
    )
    print("|---:|---:|---:|---:|---:|---:|---:|---:|")
    for run, conditional, marginal, holm_p in zip(
        runs, conditional_utility, all_utility, adjusted
    ):
        wins = sum(value > 0 for value in run.utility)
        losses = sum(value < 0 for value in run.utility)
        ties = len(run.utility) - wins - losses
        print(
            f"| {run.weight:g} | {run.source_positions} | {len(run.utility)} | "
            f"{len(run.utility) / run.source_positions:.2%} | "
            f"{fmt_est(conditional)} | {fmt_est(marginal)} | "
            f"{conditional.p:.4g} / {holm_p:.4g} | "
            f"{wins}–{losses}–{ties} (p={exact_sign_p(wins, losses):.4g}) |"
        )
        if not run.accounting_ok:
            print(f"ERROR: unequal iteration accounting in {run.path}", file=sys.stderr)
            return 2
    if any(not run.game_source_counts for run in runs):
        print(
            "\nMarginal intervals for older logs are naive: those logs predate "
            "per-game source accounting."
        )
    if any(not run.complete for run in runs):
        print(
            "Incomplete logs were truncated at their last flushed disagreement; "
            "later unflushed agreements are excluded."
        )

    print("\nPair-conditioned secondary metrics (candidate prior minus weight 0):")
    for run_index, run in enumerate(runs):
        utility = cluster_estimate(run.utility, run.game)
        win = cluster_estimate(run.win, run.game)
        spread = cluster_estimate(run.spread, run.game)
        baseline_regret = estimate([max(value, 0.0) for value in run.utility])
        candidate_regret = estimate([max(-value, 0.0) for value in run.utility])
        print(
            f"- weight {run.weight:g}: utility {fmt_est(utility)} "
            f"(p={utility.p:.4g}); win {fmt_est(win)}; spread {fmt_est(spread, 3)}; "
            f"mean regret baseline={baseline_regret.mean:.6f}, "
            f"prior={candidate_regret.mean:.6f}"
        )
        if run.replacement_outer_samples is not None:
            print(
                f"  - quality estimates replaced by the matched "
                f"{run.replacement_outer_samples}-sample nested oracle"
            )
        if len(run.utility) > 1:
            observed_variance = statistics.variance(run.utility)
            oracle_mean_variance = (
                sum(value * value for value in run.oracle_sem)
                / (len(run.oracle_sem) * len(run.oracle_sem))
            )
            total_mean_variance = observed_variance / len(run.utility)
            oracle_noise_share = (
                oracle_mean_variance / total_mean_variance
                if total_mean_variance > 0.0
                else math.nan
            )
            print(
                f"  - oracle MC contribution to conditional-mean variance: "
                f"approximately {oracle_noise_share:.1%} "
                f"(MC-only SEM {math.sqrt(oracle_mean_variance):.6f})"
            )
        all_win = (
            cluster_aggregate_estimate(run.game_win_sums, run.game_source_counts)
            if run.game_source_counts
            else estimate(all_position(run.win, run.source_positions))
        )
        all_spread = (
            cluster_aggregate_estimate(run.game_spread_sums, run.game_source_counts)
            if run.game_source_counts
            else estimate(all_position(run.spread, run.source_positions))
        )
        print(
            f"  - per source position: utility {fmt_est(all_utility[run_index])}; "
            f"win {fmt_est(all_win)}; spread {fmt_est(all_spread, 3)}"
        )
        if (
            run.baseline_nomination_iterations > 0
            and run.prior_nomination_iterations > 0
        ):
            baseline_rate = (
                run.baseline_nomination_iterations
                / run.baseline_nomination_seconds
            )
            prior_rate = (
                run.prior_nomination_iterations
                / run.prior_nomination_seconds
            )
            print(
                f"  - nomination throughput: baseline={baseline_rate:.0f}, "
                f"prior={prior_rate:.0f} iterations/s "
                f"({prior_rate / baseline_rate - 1:+.2%})"
            )
        if run.static_equity_delta:
            higher_static = [
                value
                for value, static_delta in zip(
                    run.utility, run.static_equity_delta, strict=True
                )
                if static_delta > 0
            ]
            lower_static = sum(
                static_delta < 0 for static_delta in run.static_equity_delta
            )
            equal_static = (
                len(run.static_equity_delta) - len(higher_static) - lower_static
            )
            print(
                f"  - candidate static equity delta: "
                f"mean={statistics.fmean(run.static_equity_delta):+.3f} points, "
                f"higher/lower/equal={len(higher_static)}/{lower_static}/"
                f"{equal_static}, "
                f"oracle correlation="
                f"{correlation(run.static_equity_delta, run.utility):+.3f}; "
                f"oracle utility when higher={estimate(higher_static).mean:+.6f}"
            )
        else:
            print("  - candidate static equity delta: no differing moves")
        for band in ("70+", "50–69", "29–49"):
            values_and_games = [
                (value, game)
                for value, bag, game in zip(
                    run.utility, run.bag, run.game, strict=True
                )
                if bag_band(bag) == band
            ]
            values = [value for value, _ in values_and_games]
            games = [game for _, game in values_and_games]
            band_estimate = cluster_estimate(values, games)
            print(f"  - bag {band}: n={len(values)}, {fmt_est(band_estimate)}")

    if args.aa_reference is not None:
        aa = read_run(args.aa_reference, allow_partial=args.allow_partial)
        if aa.weight != 0.0:
            parser.error("--aa-reference must have candidate prior weight 0")
        if not aa.game_source_counts:
            parser.error("--aa-reference lacks complete per-game accounting")
        aa_games = {
            game: (count, utility, win, spread)
            for game, count, utility, win, spread in zip(
                aa.game_summary_ids,
                aa.game_source_counts,
                aa.game_utility_sums,
                aa.game_win_sums,
                aa.game_spread_sums,
                strict=True,
            )
        }
        adjusted_rows = []
        for run in runs:
            if run.weight == 0.0:
                continue
            if (
                run.protocol != aa.protocol
                or run.model_protocol != aa.model_protocol
            ):
                parser.error(
                    f"weight {run.weight:g} does not match the A/A protocol"
                )
            if not run.game_source_counts:
                parser.error(
                    f"weight {run.weight:g} lacks complete per-game accounting"
                )
            run_games = {
                game: (count, utility, win, spread)
                for game, count, utility, win, spread in zip(
                    run.game_summary_ids,
                    run.game_source_counts,
                    run.game_utility_sums,
                    run.game_win_sums,
                    run.game_spread_sums,
                    strict=True,
                )
            }
            common = sorted(
                game
                for game in set(aa_games) & set(run_games)
                if aa_games[game][0] == run_games[game][0]
            )
            if len(common) < 2:
                parser.error(
                    f"weight {run.weight:g} has fewer than two complete "
                    "games in common with the A/A run"
                )
            counts = [run_games[game][0] for game in common]
            utility = cluster_aggregate_estimate(
                [
                    run_games[game][1] - aa_games[game][1]
                    for game in common
                ],
                counts,
            )
            win = cluster_aggregate_estimate(
                [
                    run_games[game][2] - aa_games[game][2]
                    for game in common
                ],
                counts,
            )
            spread = cluster_aggregate_estimate(
                [
                    run_games[game][3] - aa_games[game][3]
                    for game in common
                ],
                counts,
            )
            adjusted_rows.append(
                (run.weight, len(common), sum(counts), utility, win, spread)
            )
        adjusted_holm = holm_adjust(
            [row[3].p for row in adjusted_rows]
        )
        print(
            "\nA/A-adjusted all-source diagnostic "
            "(policy delta minus weight-0 scheduler delta on matched games):"
        )
        for row, holm_p in zip(adjusted_rows, adjusted_holm, strict=True):
            weight, games, positions, utility, win, spread = row
            print(
                f"- weight {weight:g}: games={games}, positions={positions}; "
                f"utility {fmt_est(utility)} (p={utility.p:.4g}, "
                f"Holm={holm_p:.4g}); win {fmt_est(win)}; "
                f"spread {fmt_est(spread, 3)}"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())

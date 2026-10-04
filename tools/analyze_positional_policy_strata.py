#!/usr/bin/env python3
"""Stratify positional-rollout results by root positional signal strength."""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class Candidate:
    rank: int
    base_equity: float
    adjustment: float


@dataclass(frozen=True)
class Result:
    position: int
    game: int
    delta: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--policy-log", required=True, type=Path)
    return parser.parse_args()


def parse_fields(line: str, stop_at_move: bool = False) -> dict[str, str]:
    if stop_at_move:
        line = line.split(" move=", 1)[0]
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def load_candidates(path: Path) -> dict[int, list[Candidate]]:
    candidates: dict[int, list[Candidate]] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("POSITIONAL_CANDIDATE "):
                continue
            fields = parse_fields(line, stop_at_move=True)
            position = int(fields["position"])
            candidates.setdefault(position, []).append(
                Candidate(
                    rank=int(fields["rank"]),
                    base_equity=float(fields["base_equity"]),
                    adjustment=float(fields["compact_adjustment"]),
                )
            )
    for rows in candidates.values():
        rows.sort(key=lambda row: row.rank)
    return candidates


def load_results(path: Path) -> list[Result]:
    results: list[Result] = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("POSITIONAL_POLICY_POSITION "):
                continue
            fields = parse_fields(line)
            results.append(
                Result(
                    position=int(fields["position"]),
                    game=int(fields["game"]),
                    delta=float(fields["oracle_spread_delta"]),
                )
            )
    if not results:
        raise ValueError(f"{path}: no positional policy positions")
    return results


def root_signals(rows: list[Candidate]) -> dict[str, float]:
    # The deployed selector sees at most the top three moves within three
    # equity points of the static leader.
    leader = rows[0].base_equity
    eligible = [
        row for row in rows[:3] if row.base_equity >= leader - 3.0 - 1e-9
    ]
    values = np.asarray(
        [row.adjustment * 0.75 for row in eligible], dtype=np.float64
    )
    return {
        "signed_mean": float(values.mean()),
        "mean_abs": float(np.abs(values).mean()),
        "range": float(values.max() - values.min()),
        "max_abs": float(np.abs(values).max()),
    }


def clustered_stats(
    values: np.ndarray, games: np.ndarray
) -> tuple[float, float, float, float, int]:
    mean = float(values.mean())
    unique_games = np.unique(games)
    if len(unique_games) < 2:
        return mean, math.nan, math.nan, math.nan, len(unique_games)
    cluster_sums = np.asarray(
        [np.sum(values[games == game] - mean) for game in unique_games]
    )
    sem = math.sqrt(
        len(unique_games)
        / (len(unique_games) - 1)
        * float(np.sum(cluster_sums**2))
        / len(values) ** 2
    )
    p_value = (
        math.erfc(abs(mean / sem) / math.sqrt(2.0)) if sem > 0 else 1.0
    )
    return (
        mean,
        mean - 1.96 * sem,
        mean + 1.96 * sem,
        p_value,
        len(unique_games),
    )


def print_group(
    label: str, mask: np.ndarray, deltas: np.ndarray, games: np.ndarray
) -> None:
    selected = deltas[mask]
    if len(selected) == 0:
        print(f"{label:24s} n=   0")
        return
    mean, low, high, p_value, game_count = clustered_stats(
        selected, games[mask]
    )
    nonzero = selected[selected != 0]
    wins = int(np.sum(nonzero > 0))
    losses = int(np.sum(nonzero < 0))
    print(
        f"{label:24s} n={len(selected):4d} disagree={len(nonzero):3d} "
        f"W-L={wins:2d}-{losses:2d} delta={mean:+.6f} "
        f"CI=[{low:+.6f},{high:+.6f}] p={p_value:.4f} games={game_count}"
    )


def analyze_metric(
    name: str,
    signal: np.ndarray,
    deltas: np.ndarray,
    games: np.ndarray,
) -> None:
    print(f"\n{name}")
    cutoffs = np.quantile(signal, [0.25, 0.5, 0.75, 0.9])
    lower = -math.inf
    labels = ("Q1", "Q2", "Q3", "Q4")
    for index, upper in enumerate((*cutoffs[:3], math.inf)):
        mask = (signal > lower) & (signal <= upper)
        print_group(
            f"{labels[index]} ({lower:.3f},{upper:.3f}]",
            mask,
            deltas,
            games,
        )
        lower = upper
    print_group(
        f"top 25% >= {cutoffs[2]:.3f}",
        signal >= cutoffs[2],
        deltas,
        games,
    )
    print_group(
        f"top 10% >= {cutoffs[3]:.3f}",
        signal >= cutoffs[3],
        deltas,
        games,
    )
    correlation = float(np.corrcoef(signal, deltas)[0, 1])
    print(
        "cutoffs="
        + ",".join(f"{value:.6f}" for value in cutoffs)
        + f" correlation_with_delta={correlation:+.5f}"
    )


def main() -> None:
    args = parse_args()
    candidates = load_candidates(args.corpus)
    results = load_results(args.policy_log)
    missing = [
        result.position
        for result in results
        if result.position not in candidates
    ]
    if missing:
        raise ValueError(f"missing candidate rows for positions {missing[:5]}")
    signal_rows = [root_signals(candidates[result.position]) for result in results]
    deltas = np.asarray([result.delta for result in results])
    games = np.asarray([result.game for result in results])
    print(f"log={args.policy_log} positions={len(results)}")
    print_group("all", np.ones(len(results), dtype=bool), deltas, games)
    for metric in ("signed_mean", "mean_abs", "range", "max_abs"):
        analyze_metric(
            metric,
            np.asarray([row[metric] for row in signal_rows]),
            deltas,
            games,
        )


if __name__ == "__main__":
    main()

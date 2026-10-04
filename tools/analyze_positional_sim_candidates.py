#!/usr/bin/env python3
"""Sweep positional nomination of an oracle-labelled static candidate pool.

For each position, ordinary nomination keeps the first K static candidates.
The experimental selector overnominates the first M static candidates,
reranks them by static equity plus a scaled positional adjustment, and keeps
K.  The primary result is the change in the oracle ceiling of the candidate
set: the best oracle-labelled move available to each selector.
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class Row:
    position: int
    game: int
    rank: int
    candidates: int
    base_equity: float
    adjustment: float
    oracle_utility: float
    oracle_win: float
    oracle_spread: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--oracle-log", required=True, type=Path, action="append"
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--top-k", default="5,10,15")
    parser.add_argument("--overnominate", default="10,15,20,25,30")
    parser.add_argument("--scales", default="0.5,0.75,1,1.25")
    return parser.parse_args()


def parse_fields(line: str) -> dict[str, str]:
    prefix = line.split(" move=", 1)[0]
    fields: dict[str, str] = {}
    for token in prefix.split()[1:]:
        name, value = token.split("=", 1)
        fields[name] = value
    return fields


def parse_log(path: Path) -> list[Row]:
    rows: list[Row] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("POSITIONAL_CANDIDATE "):
            continue
        fields = parse_fields(line)
        rows.append(
            Row(
                position=int(fields["position"]),
                game=int(fields["game"]),
                rank=int(fields["rank"]),
                candidates=int(fields["candidates"]),
                base_equity=float(fields["base_equity"]),
                adjustment=float(fields["compact_adjustment"]),
                oracle_utility=float(fields["oracle_utility"]),
                oracle_win=float(fields["oracle_win"]),
                oracle_spread=float(fields["oracle_spread"]),
            )
        )
    if not rows:
        raise ValueError(f"{path}: no positional candidate rows")
    return rows


def mean_ci_p(values: np.ndarray) -> tuple[float, float, float, float]:
    mean = float(values.mean())
    sem = float(values.std(ddof=1) / math.sqrt(len(values)))
    return (
        mean,
        mean - 1.96 * sem,
        mean + 1.96 * sem,
        math.erfc(abs(mean / sem) / math.sqrt(2.0)) if sem else 1.0,
    )


def evaluate(
    groups: list[list[Row]], top_k: int, overnominate: int, scale: float
) -> dict[str, float | int]:
    utility_deltas: list[float] = []
    win_deltas: list[float] = []
    spread_deltas: list[float] = []
    game_utility_deltas: dict[int, list[float]] = {}
    conditional_utility_deltas: list[float] = []
    overlap_total = 0
    changed_positions = 0
    baseline_oracle_hits = 0
    positional_oracle_hits = 0
    for group in groups:
        pool = group[:overnominate]
        baseline = set(range(top_k))
        positional = set(
            sorted(
                range(len(pool)),
                key=lambda index: (
                    pool[index].base_equity + scale * pool[index].adjustment,
                    -pool[index].rank,
                ),
                reverse=True,
            )[:top_k]
        )
        overlap = len(baseline & positional)
        overlap_total += overlap
        changed = positional != baseline
        changed_positions += changed

        oracle_best_pool = max(
            range(len(pool)), key=lambda index: pool[index].oracle_utility
        )
        baseline_oracle_hits += oracle_best_pool in baseline
        positional_oracle_hits += oracle_best_pool in positional
        baseline_best = max(
            baseline, key=lambda index: pool[index].oracle_utility
        )
        positional_best = max(
            positional, key=lambda index: pool[index].oracle_utility
        )
        utility_delta = (
            pool[positional_best].oracle_utility
            - pool[baseline_best].oracle_utility
        )
        utility_deltas.append(utility_delta)
        win_deltas.append(
            pool[positional_best].oracle_win - pool[baseline_best].oracle_win
        )
        spread_deltas.append(
            pool[positional_best].oracle_spread
            - pool[baseline_best].oracle_spread
        )
        game_utility_deltas.setdefault(group[0].game, []).append(utility_delta)
        if changed:
            conditional_utility_deltas.append(utility_delta)

    clustered = np.asarray(
        [np.mean(values) for values in game_utility_deltas.values()],
        dtype=np.float64,
    )
    utility_mean, ci_low, ci_high, p_value = mean_ci_p(clustered)
    # Report the position-weighted effect as the point estimate while using
    # source-game clustering for its uncertainty.
    position_utility_mean = float(np.mean(utility_deltas))
    ci_shift = position_utility_mean - utility_mean
    return {
        "top_k": top_k,
        "overnominate": overnominate,
        "scale": scale,
        "positions": len(groups),
        "changed_positions": changed_positions,
        "changed_rate": changed_positions / len(groups),
        "mean_overlap": overlap_total / len(groups),
        "mean_unique_per_selector": top_k - overlap_total / len(groups),
        "baseline_oracle_pool_hit_rate": baseline_oracle_hits / len(groups),
        "positional_oracle_pool_hit_rate": positional_oracle_hits / len(groups),
        "oracle_utility_ceiling_delta": position_utility_mean,
        "clustered_ci95_low": ci_low + ci_shift,
        "clustered_ci95_high": ci_high + ci_shift,
        "clustered_p_two_sided_normal": p_value,
        "conditional_utility_ceiling_delta": float(
            np.mean(conditional_utility_deltas)
        )
        if conditional_utility_deltas
        else 0.0,
        "oracle_win_ceiling_delta": float(np.mean(win_deltas)),
        "oracle_spread_ceiling_delta": float(np.mean(spread_deltas)),
    }


def main() -> None:
    args = parse_args()
    by_position: dict[int, list[Row]] = {}
    for oracle_log in args.oracle_log:
        for row in parse_log(oracle_log):
            by_position.setdefault(row.position, []).append(row)
    groups: list[list[Row]] = []
    for position, rows in sorted(by_position.items()):
        rows.sort(key=lambda row: row.rank)
        if len(rows) != rows[0].candidates or any(
            row.rank != index for index, row in enumerate(rows)
        ):
            raise ValueError(f"incomplete candidate group at position {position}")
        groups.append(rows)

    top_values = [int(value) for value in args.top_k.split(",")]
    over_values = [int(value) for value in args.overnominate.split(",")]
    scales = [float(value) for value in args.scales.split(",")]
    available = min(len(group) for group in groups)
    results = [
        evaluate(groups, top_k, overnominate, scale)
        for top_k in top_values
        for overnominate in over_values
        for scale in scales
        if top_k <= overnominate <= available
    ]
    results.sort(
        key=lambda result: (
            -float(result["oracle_utility_ceiling_delta"]),
            float(result["mean_unique_per_selector"]),
        )
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    for result in results[:20]:
        print(
            f"k={result['top_k']} over={result['overnominate']} "
            f"scale={result['scale']:.2f} "
            f"changed={result['changed_rate']:.3f} "
            f"unique={result['mean_unique_per_selector']:.3f} "
            f"utility={result['oracle_utility_ceiling_delta']:+.6f} "
            f"CI=[{result['clustered_ci95_low']:+.6f},"
            f"{result['clustered_ci95_high']:+.6f}] "
            f"spread={result['oracle_spread_ceiling_delta']:+.4f}"
        )


if __name__ == "__main__":
    main()

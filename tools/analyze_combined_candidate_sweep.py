#!/usr/bin/env python3
"""Analyze constant-node KLV3-plus-positional candidate sweeps."""

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
    num_plies: int
    num_plays: int
    min_iterations: int
    selected_rank: int
    oracle_best_rank: int
    candidate_regret: float
    sampling_regret: float
    total_regret: float
    spread_delta: float
    iterations: int
    nodes: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path, action="append")
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def load_rows(paths: list[Path]) -> list[Row]:
    rows: list[Row] = []
    seen: set[tuple[int, int, int]] = set()
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.startswith("COMBINED_SWEEP_POSITION "):
                    continue
                fields = parse_fields(line)
                row = Row(
                    position=int(fields["position"]),
                    game=int(fields["game"]),
                    num_plies=int(fields.get("num_plies", "2")),
                    num_plays=int(fields["num_plays"]),
                    min_iterations=int(fields["min_play_iterations"]),
                    selected_rank=int(fields["selected_source_rank"]),
                    oracle_best_rank=int(fields["oracle_best_source_rank"]),
                    candidate_regret=float(fields["candidate_regret"]),
                    sampling_regret=float(fields["sampling_regret"]),
                    total_regret=float(fields["total_regret"]),
                    spread_delta=float(fields["spread_delta"]),
                    iterations=int(fields["iterations"]),
                    nodes=int(fields["nodes"]),
                )
                key = (
                    row.position,
                    row.num_plies,
                    row.num_plays,
                    row.min_iterations,
                )
                if key in seen:
                    raise ValueError(f"duplicate sweep row {key}")
                seen.add(key)
                rows.append(row)
    if not rows:
        raise ValueError("no combined sweep rows")
    return rows


def clustered_sem(values: np.ndarray, games: np.ndarray) -> float:
    mean = float(values.mean())
    unique_games = np.unique(games)
    if len(unique_games) < 2:
        return math.nan
    cluster_sums = np.asarray(
        [np.sum(values[games == game] - mean) for game in unique_games]
    )
    return math.sqrt(
        len(unique_games)
        / (len(unique_games) - 1)
        * float(np.sum(cluster_sums**2))
        / len(values) ** 2
    )


def summarize(rows: list[Row]) -> list[dict[str, float | int]]:
    by_cell: dict[tuple[int, int, int], list[Row]] = {}
    for row in rows:
        by_cell.setdefault(
            (row.num_plies, row.num_plays, row.min_iterations), []
        ).append(row)
    position_sets = [
        {row.position for row in cell_rows}
        for cell_rows in by_cell.values()
    ]
    if any(positions != position_sets[0] for positions in position_sets[1:]):
        raise ValueError("sweep cells do not cover identical positions")

    results: list[dict[str, float | int]] = []
    for (
        num_plies,
        num_plays,
        min_iterations,
    ), cell_rows in sorted(by_cell.items()):
        cell_rows.sort(key=lambda row: row.position)
        games = np.asarray([row.game for row in cell_rows])
        total = np.asarray([row.total_regret for row in cell_rows])
        candidate = np.asarray([row.candidate_regret for row in cell_rows])
        sampling = np.asarray([row.sampling_regret for row in cell_rows])
        spread = np.asarray([row.spread_delta for row in cell_rows])
        total_sem = clustered_sem(total, games)
        results.append(
            {
                "num_plies": num_plies,
                "num_plays": num_plays,
                "min_iterations": min_iterations,
                "positions": len(cell_rows),
                "games": len(np.unique(games)),
                "node_budget": cell_rows[0].nodes,
                "oracle_hit_rate": float(
                    np.mean(
                        [
                            row.selected_rank == row.oracle_best_rank
                            for row in cell_rows
                        ]
                    )
                ),
                "mean_total_regret": float(total.mean()),
                "total_regret_clustered_sem": total_sem,
                "total_regret_ci95_low": float(total.mean() - 1.96 * total_sem),
                "total_regret_ci95_high": float(
                    total.mean() + 1.96 * total_sem
                ),
                "mean_candidate_regret": float(candidate.mean()),
                "mean_sampling_regret": float(sampling.mean()),
                "mean_spread_delta": float(spread.mean()),
                "iterations": sum(row.iterations for row in cell_rows),
                "nodes": sum(row.nodes for row in cell_rows),
            }
        )
    results.sort(
        key=lambda result: (
            float(result["mean_total_regret"]),
            int(result["num_plies"]),
            int(result["num_plays"]),
            int(result["min_iterations"]),
        )
    )
    return results


def paired_comparisons(
    rows: list[Row], results: list[dict[str, float | int]]
) -> list[dict[str, float | int]]:
    winner = (
        int(results[0]["num_plies"]),
        int(results[0]["num_plays"]),
        int(results[0]["min_iterations"]),
    )
    by_cell: dict[tuple[int, int, int], dict[int, Row]] = {}
    for row in rows:
        by_cell.setdefault(
            (row.num_plies, row.num_plays, row.min_iterations), {}
        )[row.position] = row
    winner_rows = by_cell[winner]
    comparisons: list[dict[str, float | int]] = []
    for cell, cell_rows in by_cell.items():
        if cell == winner:
            continue
        positions = sorted(winner_rows)
        differences = np.asarray(
            [
                cell_rows[position].total_regret
                - winner_rows[position].total_regret
                for position in positions
            ]
        )
        games = np.asarray([cell_rows[position].game for position in positions])
        sem = clustered_sem(differences, games)
        mean = float(differences.mean())
        if sem == 0.0:
            p_value = 1.0 if mean == 0.0 else 0.0
        else:
            p_value = math.erfc(abs(mean / sem) / math.sqrt(2.0))
        comparisons.append(
            {
                "num_plies": cell[0],
                "num_plays": cell[1],
                "min_iterations": cell[2],
                "mean_excess_regret": mean,
                "clustered_sem": sem,
                "ci95_low": mean - 1.96 * sem,
                "ci95_high": mean + 1.96 * sem,
                "p_value": p_value,
            }
        )
    comparisons.sort(key=lambda result: float(result["mean_excess_regret"]))
    return comparisons


def main() -> None:
    args = parse_args()
    rows = load_rows(args.log)
    results = summarize(rows)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(results[0]))
            writer.writeheader()
            writer.writerows(results)
    for result in results[:20]:
        print(
            f"p={result['num_plies']:2d} n={result['num_plays']:2d} "
            f"min={result['min_iterations']:3d} "
            f"regret={result['mean_total_regret']:.7f} "
            f"CI=[{result['total_regret_ci95_low']:.7f},"
            f"{result['total_regret_ci95_high']:.7f}] "
            f"candidate={result['mean_candidate_regret']:.7f} "
            f"sampling={result['mean_sampling_regret']:.7f} "
            f"hit={result['oracle_hit_rate']:.3f} "
            f"spread={result['mean_spread_delta']:+.4f}"
        )
    winner = results[0]
    print(
        "paired excess regret versus "
        f"p={winner['num_plies']} n={winner['num_plays']} "
        f"min={winner['min_iterations']}:"
    )
    for comparison in paired_comparisons(rows, results):
        print(
            f"p={comparison['num_plies']:2d} "
            f"n={comparison['num_plays']:2d} "
            f"min={comparison['min_iterations']:3d} "
            f"delta={comparison['mean_excess_regret']:+.7f} "
            f"CI=[{comparison['ci95_low']:+.7f},"
            f"{comparison['ci95_high']:+.7f}] "
            f"p={comparison['p_value']:.4f}"
        )


if __name__ == "__main__":
    main()

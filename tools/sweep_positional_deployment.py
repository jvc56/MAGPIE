#!/usr/bin/env python3
"""Sweep cheap deployment restrictions for the compact positional evaluator.

The model coefficients stay frozen.  This script varies only which static
candidates are eligible for post-generation reranking, the adjustment scale,
and an optional symmetric adjustment cap.  It is therefore useful for finding
a speed/strength frontier without retraining on the evaluation corpus.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np

from train_positional_adjustment import read_distribution, read_layout
from train_positional_candidates import Candidate, parse_log


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--oracle-log",
        required=True,
        type=Path,
        action="append",
        help="evaluation corpus; repeat to pool independent corpora",
    )
    parser.add_argument("--coefficient-json", required=True, type=Path)
    parser.add_argument("--letter-distribution", required=True, type=Path)
    parser.add_argument("--layout", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--tops", default="1,2,3,4,5,6,8", help="comma-separated candidate counts"
    )
    parser.add_argument(
        "--margins",
        default="0,0.25,0.5,1,1.5,2,2.5,3,4,5,6,8,12,inf",
        help="comma-separated static-equity margins",
    )
    parser.add_argument(
        "--scales",
        default="0.5,0.75,1,1.25,1.5",
        help="comma-separated adjustment multipliers",
    )
    parser.add_argument(
        "--caps",
        default="1,1.5,2,2.5,3,4,inf",
        help="comma-separated absolute adjustment caps",
    )
    parser.add_argument(
        "--min-bags",
        default="29",
        help="comma-separated minimum bag sizes at which reranking is active",
    )
    return parser.parse_args()


def comma_ints(value: str) -> list[int]:
    return [int(item) for item in value.split(",")]


def comma_floats(value: str) -> list[float]:
    return [
        math.inf if item.strip().lower() == "inf" else float(item)
        for item in value.split(",")
    ]


def compact_coefficients(path: Path) -> dict[str, float]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {
        str(name): float(value)
        for name, value in data["independent_test"]["models"]["compact"][
            "raw_coefficients"
        ].items()
    }


def raw_adjustments(
    candidates: list[Candidate], coefficients: dict[str, float]
) -> np.ndarray:
    return np.asarray(
        [
            sum(
                coefficients.get(name, 0.0) * value
                for name, value in candidate.features.items()
            )
            for candidate in candidates
        ],
        dtype=np.float64,
    )


def clustered_summary(
    candidates: list[Candidate],
    raw_adjustment: np.ndarray,
    top: int,
    margin: float,
    scale: float,
    cap: float,
    min_bag: int,
) -> dict[str, float | int]:
    by_position: dict[int, list[int]] = {}
    for index, candidate in enumerate(candidates):
        by_position.setdefault(candidate.position, []).append(index)

    gains: list[float] = []
    game_gains: dict[int, list[float]] = {}
    changed = 0
    model_wins = 0
    baseline_wins = 0
    eligible_total = 0
    active_positions = 0
    for indices in by_position.values():
        indices.sort(key=lambda index: candidates[index].rank)
        baseline = max(indices, key=lambda index: candidates[index].base_equity)
        best_equity = candidates[baseline].base_equity
        if candidates[baseline].bag < min_bag or top == 1 or margin == 0:
            model = baseline
            eligible = [baseline]
        else:
            eligible = [
                index
                for index in indices
                if candidates[index].rank < top
                and best_equity - candidates[index].base_equity <= margin
            ]
            if not eligible:
                eligible = [baseline]
            active_positions += 1
            model = max(
                eligible,
                key=lambda index: candidates[index].base_equity
                + float(
                    np.clip(
                        raw_adjustment[index] * scale,
                        -cap,
                        cap,
                    )
                ),
            )
        eligible_total += len(eligible)
        gain = candidates[model].oracle_spread - candidates[baseline].oracle_spread
        gains.append(gain)
        game_gains.setdefault(candidates[baseline].game, []).append(gain)
        if model != baseline:
            changed += 1
            if gain > 0:
                model_wins += 1
            elif gain < 0:
                baseline_wins += 1

    game_means = np.asarray(
        [np.mean(per_game) for per_game in game_gains.values()], dtype=np.float64
    )
    mean = float(np.mean(gains))
    sem = float(game_means.std(ddof=1) / math.sqrt(len(game_means)))
    return {
        "top": top,
        "margin": margin,
        "scale": scale,
        "cap": cap,
        "min_bag": min_bag,
        "positions": len(gains),
        "active_positions": active_positions,
        "mean_candidates_reranked": eligible_total / len(gains),
        "changed_positions": changed,
        "changed_rate": changed / len(gains),
        "model_wins_on_changes": model_wins,
        "baseline_wins_on_changes": baseline_wins,
        "gain": mean,
        "clustered_sem": sem,
        "ci95_low": mean - 1.96 * sem,
        "ci95_high": mean + 1.96 * sem,
        "p_two_sided_normal": math.erfc(
            abs(mean / sem) / math.sqrt(2.0)
        )
        if sem > 0
        else 1.0,
    }


def main() -> None:
    args = parse_args()
    machine_letters, scores = read_distribution(args.letter_distribution)
    layout = read_layout(args.layout)
    candidates = [
        candidate
        for oracle_log in args.oracle_log
        for candidate in parse_log(
            oracle_log, layout, scores, machine_letters
        )
    ]
    adjustment = raw_adjustments(
        candidates, compact_coefficients(args.coefficient_json)
    )
    rows = [
        clustered_summary(
            candidates,
            adjustment,
            top,
            margin,
            scale,
            cap,
            min_bag,
        )
        for min_bag in comma_ints(args.min_bags)
        for top in comma_ints(args.tops)
        for margin in comma_floats(args.margins)
        for scale in comma_floats(args.scales)
        for cap in comma_floats(args.caps)
    ]
    rows.sort(
        key=lambda row: (
            -float(row["gain"]),
            float(row["mean_candidates_reranked"]),
        )
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(rows[:30], indent=2))


if __name__ == "__main__":
    main()

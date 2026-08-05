#!/usr/bin/env python3
"""Audit complete-game coverage and the pre-turn future-turn heuristic."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics

try:
    from tools.extract_time_value_positions import fields
    from tools.merge_time_value_positions import bag_band
except ModuleNotFoundError:
    from extract_time_value_positions import fields
    from merge_time_value_positions import bag_band


def percentile(values: list[float], probability: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = probability * (len(ordered) - 1)
    lower = int(math.floor(rank))
    upper = int(math.ceil(rank))
    fraction = rank - lower
    return ordered[lower] + fraction * (ordered[upper] - ordered[lower])


def analyze(path: Path) -> dict[str, object]:
    games: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("TIME_VALUE_POSITION "):
                raise ValueError("corpus contains a non-position row")
            row = fields(line)
            games[(row["source_seed"], row["start"])].append(row)
    if not games:
        raise ValueError("empty corpus")

    policies: Counter[str] = Counter()
    bands: Counter[str] = Counter()
    game_policies: Counter[str] = Counter()
    errors: list[float] = []
    absolute_errors: list[float] = []
    by_band: dict[str, list[float]] = defaultdict(list)
    game_lengths: list[int] = []
    for identity, rows in games.items():
        rows.sort(key=lambda row: int(row["turn"]))
        turns = [int(row["turn"]) for row in rows]
        if turns != list(range(1, len(rows) + 1)):
            raise ValueError(f"source game {identity} has nonconsecutive turns")
        policy_set = {row["trajectory_policy"] for row in rows}
        if len(policy_set) != 1:
            raise ValueError(f"source game {identity} mixes trajectory policies")
        policy = next(iter(policy_set))
        game_policies[policy] += 1
        game_lengths.append(len(rows))
        later_by_player = Counter()
        for row in reversed(rows):
            player = int(row["player"])
            actual = later_by_player[player]
            predicted = int(row["predicted_future_turns"])
            error = float(predicted - actual)
            errors.append(error)
            absolute_errors.append(abs(error))
            band = bag_band(int(row["bag"]))
            by_band[band].append(error)
            policies[policy] += 1
            bands[band] += 1
            later_by_player[player] += 1

    def error_summary(values: list[float]) -> dict[str, float | int]:
        absolute = [abs(value) for value in values]
        return {
            "positions": len(values),
            "mean_error": statistics.fmean(values),
            "mae": statistics.fmean(absolute),
            "median_error": statistics.median(values),
            "p95_absolute_error": percentile(absolute, 0.95),
            "maximum_absolute_error": max(absolute),
        }

    return {
        "artifact_kind": "time_value_corpus_audit",
        "games": len(games),
        "positions": sum(game_lengths),
        "games_by_trajectory_policy": dict(sorted(game_policies.items())),
        "positions_by_trajectory_policy": dict(sorted(policies.items())),
        "positions_by_bag_band": dict(sorted(bands.items())),
        "game_turns": {
            "mean": statistics.fmean(game_lengths),
            "median": statistics.median(game_lengths),
            "minimum": min(game_lengths),
            "maximum": max(game_lengths),
        },
        "future_turn_prediction": error_summary(errors),
        "future_turn_prediction_by_bag_band": {
            band: error_summary(values) for band, values in sorted(by_band.items())
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    document = analyze(args.corpus)
    rendered = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        print(rendered, end="")
    else:
        args.output.write_text(rendered, encoding="utf-8")


if __name__ == "__main__":
    main()

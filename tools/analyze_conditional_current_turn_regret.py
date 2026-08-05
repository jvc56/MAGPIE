#!/usr/bin/env python3
"""Analyze cross-fitted conditional current-turn regret predictions.

The game is the inference unit. Reliability bins remain checkpoint-level
because that is the preregistered stopping gate, while confidence intervals
and the stability ablation comparison cluster all repeated checkpoints by
complete source game.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import random
import statistics
from typing import Callable, Iterable

try:
    from tools.clustered_inference import student_t_critical
except ModuleNotFoundError:  # Direct execution from tools/.
    from clustered_inference import student_t_critical  # type: ignore[no-redef]


Row = dict[str, str]
POSITIVE_EPSILON = 1.0e-15


def read_rows(path: Path) -> list[Row]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"prediction file is empty: {path}")
    return rows


def numeric(row: Row, key: str) -> float:
    value = float(row[key])
    if not math.isfinite(value):
        raise ValueError(f"{key} is not finite")
    return value


def clustered_summary(
    rows: Iterable[Row], value: Callable[[Row], float]
) -> dict[str, object]:
    by_game: dict[str, list[float]] = defaultdict(list)
    row_values: list[float] = []
    for row in rows:
        measured = value(row)
        if not math.isfinite(measured):
            raise ValueError("clustered value is not finite")
        by_game[row["game"]].append(measured)
        row_values.append(measured)
    if not row_values:
        raise ValueError("clustered summary has no rows")
    game_values = [statistics.fmean(values) for values in by_game.values()]
    mean = statistics.fmean(game_values)
    if len(game_values) == 1:
        radius = 0.0
    else:
        standard_error = statistics.stdev(game_values) / math.sqrt(
            len(game_values)
        )
        radius = (
            student_t_critical(0.95, len(game_values) - 1) * standard_error
        )
    return {
        "rows": len(row_values),
        "games": len(game_values),
        "row_weighted_mean": statistics.fmean(row_values),
        "game_weighted_mean": mean,
        "ci95": [mean - radius, mean + radius],
    }


def reliability_bins(rows: list[Row]) -> list[dict[str, object]]:
    observations = sorted(
        (
            numeric(row, "predicted_total_current_turn_regret"),
            numeric(row, "total_current_turn_regret"),
        )
        for row in rows
    )
    bins: list[dict[str, object]] = []
    start = 0
    while start < len(observations) and len(bins) < 10:
        bins_left = 10 - len(bins)
        target = math.ceil((len(observations) - start) / bins_left)
        end = min(len(observations), start + target)
        while (
            end < len(observations)
            and observations[end][0] == observations[end - 1][0]
        ):
            end += 1
        group = observations[start:end]
        predicted = statistics.fmean(item[0] for item in group)
        actual = statistics.fmean(item[1] for item in group)
        ratio = actual / predicted if predicted > 0.0 else None
        bins.append(
            {
                "rows": len(group),
                "predicted_mean": predicted,
                "actual_mean": actual,
                "actual_to_predicted": ratio,
                "underprediction_over_2x": (
                    (ratio is None and actual > 0.0)
                    or (ratio is not None and ratio > 2.0)
                ),
            }
        )
        start = end
    return bins


def slope(rows: Iterable[Row]) -> float:
    numerator = 0.0
    denominator = 0.0
    for row in rows:
        predicted = numeric(row, "predicted_total_current_turn_regret")
        actual = numeric(row, "total_current_turn_regret")
        numerator += predicted * actual
        denominator += predicted * predicted
    if denominator <= 0.0:
        raise ValueError("reliability slope has zero denominator")
    return numerator / denominator


def clustered_slope_interval(
    rows: list[Row], *, samples: int, seed: int
) -> list[float]:
    by_game: dict[str, list[Row]] = defaultdict(list)
    for row in rows:
        by_game[row["game"]].append(row)
    games = sorted(by_game)
    generator = random.Random(seed)
    bootstrapped: list[float] = []
    for _ in range(samples):
        draw: list[Row] = []
        for _ in games:
            draw.extend(by_game[generator.choice(games)])
        bootstrapped.append(slope(draw))
    bootstrapped.sort()
    lower = bootstrapped[math.floor(0.025 * (samples - 1))]
    upper = bootstrapped[math.ceil(0.975 * (samples - 1))]
    return [lower, upper]


def reliability(rows: list[Row], bootstrap_samples: int) -> dict[str, object]:
    bins = reliability_bins(rows)
    ratios = [
        float(item["actual_to_predicted"])
        for item in bins
        if item["actual_to_predicted"] is not None
    ]
    return {
        "rows": len(rows),
        "games": len({row["game"] for row in rows}),
        "slope_through_origin": slope(rows),
        "game_clustered_bootstrap_ci95": clustered_slope_interval(
            rows, samples=bootstrap_samples, seed=82629
        ),
        "bins": bins,
        "underprediction_bins_over_2x": sum(
            bool(item["underprediction_over_2x"]) for item in bins
        ),
        "worst_finite_actual_to_predicted": max(ratios),
    }


def error_summary(rows: list[Row]) -> dict[str, object]:
    return {
        "signed_error": clustered_summary(
            rows,
            lambda row: numeric(
                row, "predicted_total_current_turn_regret"
            )
            - numeric(row, "total_current_turn_regret"),
        ),
        "absolute_error": clustered_summary(
            rows,
            lambda row: abs(
                numeric(row, "predicted_total_current_turn_regret")
                - numeric(row, "total_current_turn_regret")
            ),
        ),
        "squared_error": clustered_summary(
            rows,
            lambda row: (
                numeric(row, "predicted_total_current_turn_regret")
                - numeric(row, "total_current_turn_regret")
            )
            ** 2,
        ),
    }


def event_summary(rows: list[Row]) -> dict[str, object]:
    positive = [
        row
        for row in rows
        if numeric(row, "total_current_turn_regret") > POSITIVE_EPSILON
    ]
    return {
        "rows": len(rows),
        "games": len({row["game"] for row in rows}),
        "positive_rows": len(positive),
        "games_with_positive_event": len({row["game"] for row in positive}),
        "prevalence": clustered_summary(
            rows,
            lambda row: float(
                numeric(row, "total_current_turn_regret")
                > POSITIVE_EPSILON
            ),
        ),
        "actual_regret": clustered_summary(
            rows, lambda row: numeric(row, "total_current_turn_regret")
        ),
        "predicted_regret": clustered_summary(
            rows,
            lambda row: numeric(
                row, "predicted_total_current_turn_regret"
            ),
        ),
        "positive_severity": (
            clustered_summary(
                positive,
                lambda row: numeric(row, "total_current_turn_regret"),
            )
            if positive
            else None
        ),
    }


def bound_summary(rows: list[Row]) -> dict[str, object]:
    by_game: dict[str, list[bool]] = defaultdict(list)
    for row in rows:
        covered = numeric(
            row, "total_current_turn_regret"
        ) <= numeric(row, "upper_total_current_turn_regret") + 1.0e-15
        by_game[row["game"]].append(covered)
    return {
        "row_coverage": clustered_summary(
            rows,
            lambda row: float(
                numeric(row, "total_current_turn_regret")
                <= numeric(row, "upper_total_current_turn_regret")
                + 1.0e-15
            ),
        ),
        "simultaneous_game_coverage": clustered_summary(
            [
                {"game": game, "covered": str(int(all(values)))}
                for game, values in by_game.items()
            ],
            lambda row: float(row["covered"]),
        ),
        "upper_regret": clustered_summary(
            rows,
            lambda row: numeric(row, "upper_total_current_turn_regret"),
        ),
    }


def work_stratum(row: Row) -> str:
    nodes = int(row["target_nodes"])
    for label, upper in (
        ("300k", 300_000),
        ("600k", 600_000),
        ("1m", 1_000_000),
        ("1.5m", 1_500_000),
        ("2m", 2_000_000),
        ("2.5m", 2_500_000),
        ("3m", 3_000_000),
    ):
        if nodes <= upper:
            return label
    return ">3m"


def stability_stratum(row: Row) -> str:
    stable = int(row["stable_checkpoints"])
    for label, upper in (
        ("1", 1),
        ("2", 2),
        ("3-4", 4),
        ("5-8", 8),
        ("9-16", 16),
        ("17-32", 32),
    ):
        if stable <= upper:
            return label
    return "33+"


def near_tie_stratum(row: Row) -> str:
    count = int(row["near_tie_challengers"])
    if count <= 1:
        return str(count)
    if count <= 5:
        return "2-5"
    return "6+"


def stratified(
    rows: list[Row], key: Callable[[Row], str]
) -> dict[str, object]:
    groups: dict[str, list[Row]] = defaultdict(list)
    for row in rows:
        groups[key(row)].append(row)
    return {name: event_summary(group) for name, group in sorted(groups.items())}


def matched_ablation(
    main: list[Row], ablated: list[Row], bootstrap_samples: int
) -> dict[str, object]:
    if len(main) != len(ablated):
        raise ValueError("stability and ablation rows differ in length")
    join_fields = ("source_index", "game", "width", "target_nodes")
    ablated_by_key = {
        tuple(row[name] for name in join_fields): row for row in ablated
    }
    if len(ablated_by_key) != len(ablated):
        raise ValueError("stability ablation has duplicate join keys")
    comparison: list[Row] = []
    for left in main:
        row_key = tuple(left[name] for name in join_fields)
        right = ablated_by_key.get(row_key)
        if right is None:
            raise ValueError("stability and ablation rows do not join")
        if left["total_current_turn_regret"] != right["total_current_turn_regret"]:
            raise ValueError("stability and ablation labels differ")
        main_error = abs(
            numeric(left, "predicted_total_current_turn_regret")
            - numeric(left, "total_current_turn_regret")
        )
        ablated_error = abs(
            numeric(right, "predicted_total_current_turn_regret")
            - numeric(right, "total_current_turn_regret")
        )
        comparison.append(
            {
                "game": left["game"],
                "absolute_error_delta_stability_minus_ablation": str(
                    main_error - ablated_error
                ),
            }
        )
    return {
        "stability": {
            "reliability": reliability(main, bootstrap_samples),
            "errors": error_summary(main),
        },
        "ablation": {
            "reliability": reliability(ablated, bootstrap_samples),
            "errors": error_summary(ablated),
        },
        "paired_absolute_error_delta_stability_minus_ablation": clustered_summary(
            comparison,
            lambda row: numeric(
                row, "absolute_error_delta_stability_minus_ablation"
            ),
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("predictions", type=Path)
    parser.add_argument("--ablation-predictions", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--deployment-width", type=int, default=60)
    parser.add_argument("--bootstrap-samples", type=int, default=5000)
    args = parser.parse_args()
    if args.bootstrap_samples < 100:
        parser.error("bootstrap-samples must be at least 100")

    rows = read_rows(args.predictions)
    ablated = read_rows(args.ablation_predictions)
    deployed = [row for row in rows if int(row["width"]) == args.deployment_width]
    ablated_deployed = [
        row for row in ablated if int(row["width"]) == args.deployment_width
    ]
    checkpoints = [row for row in deployed if int(row["stable_checkpoints"]) > 0]
    result = {
        "artifact_kind": "conditional_current_turn_regret_analysis",
        "deployment_width": args.deployment_width,
        "label_scope": "common_checkpoint_observable_top60_union",
        "beyond_top60_regret": "unmeasured",
        "live_allocation": "disabled",
        "deployment": {
            "events": event_summary(deployed),
            "bound": bound_summary(deployed),
        },
        "checkpoint_rows": {
            "events": event_summary(checkpoints),
            "bound": bound_summary(checkpoints),
            "strata": {
                "work": stratified(checkpoints, work_stratum),
                "stability": stratified(checkpoints, stability_stratum),
                "bag": stratified(checkpoints, lambda row: row["bag_band"]),
                "policy": stratified(checkpoints, lambda row: row["policy"]),
                "near_tie": stratified(checkpoints, near_tie_stratum),
            },
        },
        "matched_stability_ablation": matched_ablation(
            deployed, ablated_deployed, args.bootstrap_samples
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

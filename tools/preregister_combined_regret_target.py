#!/usr/bin/env python3
"""Freeze a prospective combined-regret stopping target.

The target is chosen from cross-fitted *predictions*, never from the judge
outcomes of the prospective stopping panel.  At the requested full-budget
candidate width, the selected prediction quantile is snapped to a declared
grid in log space.  The resulting JSON records the input hash and complete
selection rule so a later panel cannot tune its stopping target after seeing
quality labels.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


DEFAULT_GRID = (
    1.0e-5,
    2.5e-5,
    5.0e-5,
    1.0e-4,
    2.5e-4,
    5.0e-4,
    1.0e-3,
    2.5e-3,
    5.0e-3,
)


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("cannot take a percentile of no predictions")
    if not 0.0 <= fraction <= 1.0:
        raise ValueError("quantile must be between zero and one")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = fraction * (len(ordered) - 1)
    low = math.floor(index)
    high = math.ceil(index)
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def parse_grid(raw: str) -> tuple[float, ...]:
    values = tuple(float(value) for value in raw.split(","))
    if (
        not values
        or any(not math.isfinite(value) or value <= 0.0 for value in values)
        or tuple(sorted(set(values))) != values
    ):
        raise ValueError("target grid must be unique, increasing, and positive")
    return values


def nearest_grid(value: float, grid: tuple[float, ...]) -> float:
    if not math.isfinite(value) or value < 0.0:
        raise ValueError("prediction quantile must be finite and nonnegative")
    if value == 0.0:
        return grid[0]
    return min(grid, key=lambda candidate: abs(math.log(candidate / value)))


def preregister(
    predictions: Path,
    output: Path,
    width: int,
    quantile: float,
    grid: tuple[float, ...],
) -> dict[str, object]:
    if width <= 0:
        raise ValueError("width must be positive")
    values: list[float] = []
    games: set[str] = set()
    with predictions.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {
            "width",
            "game",
            "predicted_total_current_turn_regret",
        }
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(
                "prediction CSV is missing " + ",".join(sorted(missing))
            )
        for row in reader:
            if int(row["width"]) != width:
                continue
            value = float(row["predicted_total_current_turn_regret"])
            if not math.isfinite(value) or value < 0.0:
                raise ValueError("predictions must be finite and nonnegative")
            values.append(value)
            games.add(row["game"])
    if not values:
        raise ValueError(f"prediction CSV has no width-{width} rows")

    selected_quantile = percentile(values, quantile)
    target = nearest_grid(selected_quantile, grid)
    document: dict[str, object] = {
        "artifact_kind": "prospective_combined_regret_stop_target",
        "prediction_source": str(predictions),
        "prediction_sha256": hashlib.sha256(predictions.read_bytes()).hexdigest(),
        "label_source": "cross_fitted_training_panel_predictions_only",
        "prospective_judge_outcomes_used": False,
        "width": width,
        "rows": len(values),
        "games": len(games),
        "quantile": quantile,
        "prediction_quantile": selected_quantile,
        "prediction_minimum": min(values),
        "prediction_maximum": max(values),
        "target_grid": list(grid),
        "selection_rule": "nearest target-grid value in natural-log distance",
        "combined_regret_stop_target": target,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("predictions", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--width", type=int, default=60)
    parser.add_argument("--quantile", type=float, default=0.5)
    parser.add_argument(
        "--grid",
        default=",".join(f"{value:g}" for value in DEFAULT_GRID),
    )
    args = parser.parse_args()
    try:
        grid = parse_grid(args.grid)
        document = preregister(
            args.predictions,
            args.output,
            args.width,
            args.quantile,
            grid,
        )
    except ValueError as error:
        parser.error(str(error))
    print(
        f"width={document['width']} rows={document['rows']} "
        f"games={document['games']} "
        f"prediction_quantile={document['prediction_quantile']:.12g} "
        f"target={document['combined_regret_stop_target']:.12g} "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()

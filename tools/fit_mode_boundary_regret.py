#!/usr/bin/env python3
"""Cross-fit expected regret for endgame and PEG work boundaries.

The input is a mode-native turn-curve CSV. ``regret`` is the held-out oracle
label; ``expected_regret`` is replaced with a prediction from a model that did
not train on any root from that source game. The output otherwise preserves
the input schema and can be joined with SIM curves for an oracle-independent
allocation replay.

This first model is intentionally auditable: hierarchical shrunken tables over
mode, boundary, phase, rack size, spread, and candidate count. Expected regret
is forced nonincreasing over successively deeper stages/depths. Oracle labels
never enter a feature or a tie-break at prediction time.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
from typing import Callable

try:
    from tools.clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
except ModuleNotFoundError:
    from clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )


BAG_UPPER_BOUNDS = (0, 1, 2, 3, 4, 15, 35, 60, 100)
RACK_UPPER_BOUNDS = (2, 4, 6, 8, 10, 12, 14)
SPREAD_UPPER_BOUNDS = (-100, -30, 30, 100, 10000)
CANDIDATE_UPPER_BOUNDS = (1, 5, 10, 20, 40, 80, 160, 1000000)


@dataclass(frozen=True)
class Boundary:
    row_index: int
    game: str
    turn: int
    player: int
    mode: str
    option: int
    bag: int
    spread: int
    own_rack: int
    opp_rack: int
    candidates: int
    actual_regret: float

    @property
    def bag_band(self) -> int:
        return bucket(self.bag, BAG_UPPER_BOUNDS)

    @property
    def rack_band(self) -> int:
        return bucket(self.own_rack + self.opp_rack, RACK_UPPER_BOUNDS)

    @property
    def spread_band(self) -> int:
        return bucket(self.spread, SPREAD_UPPER_BOUNDS)

    @property
    def candidate_band(self) -> int:
        return bucket(self.candidates, CANDIDATE_UPPER_BOUNDS)


@dataclass(frozen=True)
class Cell:
    mean: float
    observations: int


@dataclass(frozen=True)
class Model:
    cells: dict[tuple[str, tuple[object, ...]], Cell]


@dataclass(frozen=True)
class Prediction:
    boundary: Boundary
    fold: int
    expected_regret: float


def bucket(value: int, upper_bounds: tuple[int, ...]) -> int:
    for index, upper in enumerate(upper_bounds):
        if value <= upper:
            return index
    return len(upper_bounds) - 1


def finite_nonnegative(raw: str, name: str) -> float:
    value = float(raw)
    if not math.isfinite(value) or value < 0.0:
        raise ValueError(f"{name} must be finite and nonnegative")
    return value


def pava_nonincreasing(values: list[float], weights: list[float]) -> list[float]:
    blocks: list[list[float | int]] = []
    for index, (value, weight) in enumerate(zip(values, weights)):
        if not math.isfinite(value) or not math.isfinite(weight) or weight <= 0.0:
            raise ValueError("PAVA requires finite values and positive weights")
        blocks.append([index, index, value * weight, weight])
        while len(blocks) >= 2:
            previous, current = blocks[-2:]
            if float(previous[2]) / float(previous[3]) >= float(
                current[2]
            ) / float(current[3]):
                break
            blocks[-2:] = [
                [
                    int(previous[0]),
                    int(current[1]),
                    float(previous[2]) + float(current[2]),
                    float(previous[3]) + float(current[3]),
                ]
            ]
    fitted = [0.0] * len(values)
    for start, end, total, weight in blocks:
        mean = float(total) / float(weight)
        for index in range(int(start), int(end) + 1):
            fitted[index] = mean
    return fitted


LEVELS: tuple[
    tuple[str, Callable[[Boundary], tuple[object, ...]]], ...
] = (
    ("global", lambda point: (point.mode, point.option)),
    (
        "phase",
        lambda point: (point.mode, point.bag_band, point.option),
    ),
    (
        "rack",
        lambda point: (
            point.mode,
            point.bag_band,
            point.rack_band,
            point.option,
        ),
    ),
    (
        "spread",
        lambda point: (
            point.mode,
            point.bag_band,
            point.rack_band,
            point.spread_band,
            point.option,
        ),
    ),
    (
        "candidates",
        lambda point: (
            point.mode,
            point.bag_band,
            point.rack_band,
            point.spread_band,
            point.candidate_band,
            point.option,
        ),
    ),
)


def parent_key(key: tuple[object, ...]) -> tuple[object, ...]:
    return (*key[:-2], key[-1])


def fit_model(boundaries: list[Boundary], prior_strength: float) -> Model:
    if not boundaries:
        raise ValueError("cannot fit an empty boundary corpus")
    cells: dict[tuple[str, tuple[object, ...]], Cell] = {}
    for level_index, (level, key_function) in enumerate(LEVELS):
        grouped: dict[tuple[object, ...], list[float]] = defaultdict(list)
        for point in boundaries:
            grouped[key_function(point)].append(point.actual_regret)
        raw: dict[tuple[object, ...], tuple[float, float, int]] = {}
        for key, values in grouped.items():
            observations = len(values)
            if level_index == 0:
                fitted = sum(values) / observations
                weight = float(observations)
            else:
                parent_level = LEVELS[level_index - 1][0]
                parent = cells[(parent_level, parent_key(key))]
                weight = observations + prior_strength
                fitted = (sum(values) + prior_strength * parent.mean) / weight
            raw[key] = fitted, weight, observations

        by_state: dict[
            tuple[object, ...], list[tuple[int, tuple[object, ...]]]
        ] = defaultdict(list)
        for key in raw:
            by_state[key[:-1]].append((int(key[-1]), key))
        for boundary_cells in by_state.values():
            boundary_cells.sort()
            fitted = pava_nonincreasing(
                [raw[key][0] for _, key in boundary_cells],
                [raw[key][1] for _, key in boundary_cells],
            )
            for (_, key), mean in zip(boundary_cells, fitted):
                cells[(level, key)] = Cell(
                    max(0.0, mean), raw[key][2]
                )
    return Model(cells)


def predict(model: Model, boundary: Boundary) -> float:
    for level, key_function in reversed(LEVELS):
        cell = model.cells.get((level, key_function(boundary)))
        if cell is not None:
            return cell.mean
    # A held-out game can contain a boundary absent from the training folds.
    # Fall back to the closest observed boundary in the same mode, preferring
    # a shallower boundary so an unseen deeper search is not overcredited.
    candidates = [
        (key, cell)
        for (level, key), cell in model.cells.items()
        if level == "global" and key[0] == boundary.mode
    ]
    if not candidates:
        raise ValueError(f"model has no observations for mode={boundary.mode}")
    key, cell = min(
        candidates,
        key=lambda item: (
            abs(int(item[0][1]) - boundary.option),
            int(item[0][1]) > boundary.option,
        ),
    )
    del key
    return cell.mean


def read_boundaries(path: Path) -> tuple[list[dict[str, str]], list[Boundary]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fieldnames = set(reader.fieldnames or [])
        required = {
            "game",
            "mode",
            "turn",
            "player",
            "option",
            "bag",
            "spread",
            "own_rack",
            "opp_rack",
            "candidates",
            "regret",
        }
        missing = required - fieldnames
        if missing:
            raise ValueError(
                f"curve CSV missing fields: {','.join(sorted(missing))}"
            )
        rows = [dict(row) for row in reader]
    boundaries: list[Boundary] = []
    for row_index, row in enumerate(rows):
        mode = row["mode"].lower()
        if mode not in {"endgame", "peg"}:
            raise ValueError(f"unsupported boundary mode {mode!r}")
        boundaries.append(
            Boundary(
                row_index=row_index,
                game=row["game"],
                turn=int(row["turn"]),
                player=int(row["player"]),
                mode=mode,
                option=int(row["option"]),
                bag=int(row["bag"]),
                spread=int(row["spread"]),
                own_rack=int(row["own_rack"]),
                opp_rack=int(row["opp_rack"]),
                candidates=int(row["candidates"]),
                actual_regret=finite_nonnegative(row["regret"], "regret"),
            )
        )
    if not boundaries:
        raise ValueError("curve CSV contains no boundaries")
    return rows, boundaries


def fold_for_game(game: str, folds: int, seed: int) -> int:
    digest = hashlib.sha256(f"{seed}\0{game}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") % folds


def cross_fit(
    boundaries: list[Boundary], folds: int, seed: int, prior_strength: float
) -> list[Prediction]:
    games = {point.game for point in boundaries}
    if folds < 2 or folds > len(games):
        raise ValueError("fold count must be between 2 and the number of games")
    assignments = {game: fold_for_game(game, folds, seed) for game in games}
    if len(set(assignments.values())) != folds:
        raise ValueError("at least one cross-fitting fold is empty")
    predictions: list[Prediction] = []
    for fold in range(folds):
        training = [
            point for point in boundaries if assignments[point.game] != fold
        ]
        held_out = [
            point for point in boundaries if assignments[point.game] == fold
        ]
        model = fit_model(training, prior_strength)
        predictions.extend(
            Prediction(point, fold, predict(model, point))
            for point in held_out
        )
    predictions.sort(key=lambda item: item.boundary.row_index)
    return predictions


def metrics(predictions: list[Prediction]) -> dict[str, object]:
    by_game: dict[str, list[float]] = defaultdict(list)
    errors: list[float] = []
    for prediction in predictions:
        error = (
            prediction.expected_regret - prediction.boundary.actual_regret
        )
        errors.append(error)
        by_game[prediction.boundary.game].append(error)
    game_errors = [sum(group) / len(group) for group in by_game.values()]
    mean = sum(game_errors) / len(game_errors)
    if len(game_errors) >= 2:
        variance = sum((value - mean) ** 2 for value in game_errors) / (
            len(game_errors) - 1
        )
        half_width = student_t_critical(
            0.95, len(game_errors) - 1
        ) * math.sqrt(variance / len(game_errors))
        ci95: list[float] | None = [mean - half_width, mean + half_width]
    else:
        ci95 = None
    return {
        "rows": len(errors),
        "games": len(by_game),
        "mae": sum(abs(error) for error in errors) / len(errors),
        "rmse": math.sqrt(sum(error * error for error in errors) / len(errors)),
        "bias": sum(errors) / len(errors),
        "game_clustered_bias": mean,
        "game_clustered_bias_ci95": ci95,
    }


def clustered_summary(values: list[tuple[str, float]]) -> dict[str, object]:
    by_game: dict[str, list[float]] = defaultdict(list)
    for game, value in values:
        if math.isfinite(value):
            by_game[game].append(value)
    clustered = [sum(group) / len(group) for group in by_game.values()]
    if not clustered:
        return {
            "rows": 0,
            "games": 0,
            "mean": None,
            "ci95": None,
            "p_value": None,
        }
    mean = sum(clustered) / len(clustered)
    if len(clustered) >= 2:
        variance = sum((value - mean) ** 2 for value in clustered) / (
            len(clustered) - 1
        )
        degrees_freedom = len(clustered) - 1
        standard_error = math.sqrt(variance / len(clustered))
        half_width = student_t_critical(0.95, degrees_freedom) * standard_error
        ci95: list[float] | None = [mean - half_width, mean + half_width]
        p_value: float | None = (
            student_t_two_sided_p_value(mean / standard_error, degrees_freedom)
            if standard_error > 0.0
            else 0.0 if mean != 0.0 else 1.0
        )
    else:
        ci95 = None
        p_value = None
    return {
        "rows": len(values),
        "games": len(clustered),
        "mean": mean,
        "ci95": ci95,
        "p_value": p_value,
    }


def boundary_summaries(predictions: list[Prediction]) -> dict[str, object]:
    grouped: dict[tuple[str, int], list[Prediction]] = defaultdict(list)
    by_root: dict[
        tuple[str, int, int, str], dict[int, Prediction]
    ] = defaultdict(dict)
    for prediction in predictions:
        point = prediction.boundary
        grouped[(point.mode, point.option)].append(prediction)
        by_root[(point.game, point.turn, point.player, point.mode)][
            point.option
        ] = prediction

    levels: list[dict[str, object]] = []
    for (mode, option), group in sorted(grouped.items()):
        levels.append(
            {
                "mode": mode,
                "option": option,
                "actual_regret": clustered_summary(
                    [
                        (item.boundary.game, item.boundary.actual_regret)
                        for item in group
                    ]
                ),
                "expected_regret": clustered_summary(
                    [
                        (item.boundary.game, item.expected_regret)
                        for item in group
                    ]
                ),
                "prediction_error": clustered_summary(
                    [
                        (
                            item.boundary.game,
                            item.expected_regret
                            - item.boundary.actual_regret,
                        )
                        for item in group
                    ]
                ),
            }
        )

    marginal: list[dict[str, object]] = []
    modes = sorted({key[3] for key in by_root})
    for mode in modes:
        options = sorted(
            {
                option
                for key, values in by_root.items()
                if key[3] == mode
                for option in values
            }
        )
        for before_option, after_option in zip(options, options[1:]):
            pairs = [
                (values[before_option], values[after_option])
                for key, values in by_root.items()
                if key[3] == mode
                and before_option in values
                and after_option in values
            ]
            marginal.append(
                {
                    "mode": mode,
                    "before_option": before_option,
                    "after_option": after_option,
                    "actual_regret_reduction": clustered_summary(
                        [
                            (
                                before.boundary.game,
                                before.boundary.actual_regret
                                - after.boundary.actual_regret,
                            )
                            for before, after in pairs
                        ]
                    ),
                    "expected_regret_reduction": clustered_summary(
                        [
                            (
                                before.boundary.game,
                                before.expected_regret
                                - after.expected_regret,
                            )
                            for before, after in pairs
                        ]
                    ),
                }
            )
    return {"levels": levels, "marginal": marginal}


def write_curves(
    path: Path, rows: list[dict[str, str]], predictions: list[Prediction]
) -> None:
    if len(rows) != len(predictions):
        raise ValueError("prediction count does not match curve rows")
    fieldnames = list(rows[0])
    if "expected_regret" not in fieldnames:
        fieldnames.insert(fieldnames.index("regret") + 1, "expected_regret")
    output_rows: list[dict[str, str]] = []
    for row, prediction in zip(rows, predictions):
        if prediction.boundary.row_index != len(output_rows):
            raise ValueError("cross-fit predictions lost source row order")
        output = dict(row)
        output["expected_regret"] = f"{prediction.expected_regret:.12g}"
        output_rows.append(output)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(output_rows)


def cells_json(model: Model) -> list[dict[str, object]]:
    return [
        {
            "level": level,
            "key": list(key),
            "mean": cell.mean,
            "observations": cell.observations,
        }
        for (level, key), cell in sorted(model.cells.items())
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--split-seed", type=int, default=82405)
    parser.add_argument("--prior-strength", type=float, default=10.0)
    args = parser.parse_args()
    if not math.isfinite(args.prior_strength) or args.prior_strength < 0.0:
        parser.error("prior-strength must be finite and nonnegative")
    rows, boundaries = read_boundaries(args.curves)
    predictions = cross_fit(
        boundaries, args.folds, args.split_seed, args.prior_strength
    )
    write_curves(args.output, rows, predictions)
    model = fit_model(boundaries, args.prior_strength)
    document = {
        "artifact_kind": "mode_boundary_regret_shadow",
        "artifact_version": 1,
        "source": str(args.curves),
        "source_sha256": hashlib.sha256(args.curves.read_bytes()).hexdigest(),
        "split_unit": "complete_source_game",
        "folds": args.folds,
        "split_seed": args.split_seed,
        "prior_strength": args.prior_strength,
        "oracle_role": "held_out_scoring_only",
        "metrics": metrics(predictions),
        "boundary_summaries": boundary_summaries(predictions),
        "live_enabled": False,
        "gate_reason": "terminal mirrored-game gate not supplied",
        "cells": cells_json(model),
        "bucket_contract": {
            "bag_upper_bounds": list(BAG_UPPER_BOUNDS),
            "combined_rack_upper_bounds": list(RACK_UPPER_BOUNDS),
            "spread_upper_bounds": list(SPREAD_UPPER_BOUNDS),
            "candidate_upper_bounds": list(CANDIDATE_UPPER_BOUNDS),
        },
    }
    args.artifact.parent.mkdir(parents=True, exist_ok=True)
    args.artifact.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"rows={len(boundaries)} games={len({row.game for row in boundaries})} "
        f"rmse={document['metrics']['rmse']:.9f} live_enabled=0 "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()

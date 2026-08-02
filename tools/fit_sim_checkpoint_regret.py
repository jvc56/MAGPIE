#!/usr/bin/env python3
"""Cross-fit SIM regret using only information available at a checkpoint.

The oracle is a label, never a feature. Two predictions are emitted:

* ``state_expected_regret`` prices a fixed (plies, node-budget) arm before it
  runs, using only root state and candidate count. Its fitted curves are forced
  nonincreasing in node budget and are suitable for rest-of-game value labels.
* ``checkpoint_expected_regret`` adds a shrunken calibration correction from
  the BAI's contemporaneous estimated regret and best/challenger gap. It is the
  candidate for deciding whether to continue the current turn.

Complete source games, not rows or roots, are assigned to folds. Predictions
for every row therefore come from a model that never saw that game's oracle
labels.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
import hashlib
from itertools import combinations
import json
import math
from pathlib import Path
from typing import Callable, Iterable

try:
    from tools.clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
    from extract_time_value_positions import fields


BAG_UPPER_BOUNDS = (15, 35, 60, 100)
SPREAD_UPPER_BOUNDS = (-100, -30, 30, 100, 10000)
CANDIDATE_UPPER_BOUNDS = (5, 10, 15, 30, 128)
ESTIMATED_REGRET_UPPER_BOUNDS = (
    0.0,
    1.0e-8,
    1.0e-6,
    1.0e-5,
    1.0e-4,
    2.5e-4,
    5.0e-4,
    1.0e-3,
    2.0e-3,
    5.0e-3,
    1.0e-2,
    1.0,
)
GAP_UPPER_BOUNDS = (0.0, 1.0e-3, 2.5e-3, 5.0e-3, 1.0e-2, 2.0e-2, 4.0e-2, 1.0)


@dataclass(frozen=True)
class RootMetadata:
    game: str
    bag: int
    spread: int
    policy: str


@dataclass(frozen=True)
class Point:
    game: str
    position: int
    bag: int
    spread: int
    policy: str
    plies: int
    target_nodes: int
    actual_nodes: int
    candidates: int
    selected_rank: int
    estimated_regret: float
    estimated_gap: float
    challenger_valid: bool
    actual_regret: float
    actual_win_regret: float | None
    actual_spread_regret: float | None

    @property
    def bag_band(self) -> int:
        return bucket(self.bag, BAG_UPPER_BOUNDS)

    @property
    def spread_band(self) -> int:
        return bucket(self.spread, SPREAD_UPPER_BOUNDS)

    @property
    def candidate_band(self) -> int:
        return bucket(self.candidates, CANDIDATE_UPPER_BOUNDS)

    @property
    def estimated_regret_band(self) -> int:
        return bucket(self.estimated_regret, ESTIMATED_REGRET_UPPER_BOUNDS)

    @property
    def gap_band(self) -> int:
        return bucket(self.estimated_gap, GAP_UPPER_BOUNDS)


@dataclass(frozen=True)
class Cell:
    mean: float
    observations: int


@dataclass(frozen=True)
class StateModel:
    cells: dict[tuple[str, tuple[int, ...]], Cell]


@dataclass(frozen=True)
class CorrectionModel:
    cells: dict[tuple[str, tuple[int, ...]], Cell]


@dataclass(frozen=True)
class Prediction:
    point: Point
    fold: int
    state_expected_regret: float
    checkpoint_expected_regret: float


def bucket(value: float, upper_bounds: tuple[float, ...] | tuple[int, ...]) -> int:
    for index, upper in enumerate(upper_bounds):
        if value <= upper:
            return index
    return len(upper_bounds) - 1


def pava_nonincreasing(values: list[float], weights: list[float]) -> list[float]:
    blocks: list[list[float | int]] = []
    for index, (value, weight) in enumerate(zip(values, weights)):
        if not math.isfinite(value) or not math.isfinite(weight) or weight <= 0.0:
            raise ValueError("PAVA requires finite values and positive weights")
        blocks.append([index, index, value * weight, weight])
        while len(blocks) >= 2:
            previous, current = blocks[-2:]
            if float(previous[2]) / float(previous[3]) >= float(current[2]) / float(current[3]):
                break
            blocks[-2:] = [[
                int(previous[0]),
                int(current[1]),
                float(previous[2]) + float(current[2]),
                float(previous[3]) + float(current[3]),
            ]]
    result = [0.0] * len(values)
    for start, end, total, weight in blocks:
        fitted = float(total) / float(weight)
        for index in range(int(start), int(end) + 1):
            result[index] = fitted
    return result


def read_metadata(corpus: Path) -> dict[int, RootMetadata]:
    metadata: dict[int, RootMetadata] = {}
    with corpus.open(encoding="utf-8") as stream:
        for expected_position, line in enumerate(stream):
            if not line.startswith("TIME_VALUE_POSITION "):
                raise ValueError("corpus contains a non-position row")
            row = fields(line)
            position = int(row["position"])
            if position != expected_position:
                raise ValueError("corpus positions must be globally consecutive")
            metadata[position] = RootMetadata(
                game=f"{row['source_seed']}:{row['start']}",
                bag=int(row["bag"]),
                spread=int(row["spread"]),
                policy=row["trajectory_policy"],
            )
    return metadata


def _finite_nonnegative(raw: str, name: str) -> float:
    value = float(raw)
    if not math.isfinite(value) or value < 0.0:
        raise ValueError(f"{name} must be finite and nonnegative")
    return value


def _finite(raw: str, name: str) -> float:
    value = float(raw)
    if not math.isfinite(value):
        raise ValueError(f"{name} must be finite")
    return value


def read_points(log: Path, metadata: dict[int, RootMetadata]) -> list[Point]:
    completed: set[tuple[int, int]] = set()
    raw_points: dict[tuple[int, int, int], Point] = {}
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION_DONE "):
                row = fields(line)
                if int(row.get("dropped", "-1")) != 0:
                    raise ValueError("thinking-curve log contains dropped events")
                completed.add((int(row["position"]), int(row["plies"])))
                continue
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            row = fields(line)
            if (
                int(row["final"]) != 0
                or int(row.get("control", "0")) != 0
                or int(row["target_nodes"]) <= 0
            ):
                continue
            position = int(row["position"])
            meta = metadata.get(position)
            if meta is None:
                raise ValueError(f"position {position} is absent from corpus")
            best = float(row["estimated_best"])
            challenger = float(row["estimated_challenger"])
            estimated_regret = _finite_nonnegative(
                row["estimated_regret"], "estimated_regret"
            )
            if not math.isfinite(best):
                raise ValueError("estimated best value must be finite")
            challenger_valid = math.isfinite(challenger)
            point = Point(
                game=meta.game,
                position=position,
                bag=meta.bag,
                spread=meta.spread,
                policy=meta.policy,
                plies=int(row["plies"]),
                target_nodes=int(row["target_nodes"]),
                actual_nodes=int(row["nodes"]),
                candidates=int(row["candidates"]),
                # Older nominee-only panels did not preserve the selected
                # source rank. Retain read compatibility, but mark it unknown
                # so those panels cannot claim a measured choice change.
                selected_rank=int(row.get("selected_rank", "-1")),
                estimated_regret=estimated_regret,
                # A missing challenger means BAI collapsed every play into one
                # similarity class. Keep that condition observable instead of
                # rejecting it or pretending the gap is zero.
                estimated_gap=max(0.0, best - challenger)
                if challenger_valid
                else GAP_UPPER_BOUNDS[-1],
                challenger_valid=challenger_valid,
                actual_regret=_finite_nonnegative(row["judge_regret"], "judge_regret"),
                actual_win_regret=_finite(
                    row["judge_win_regret"], "judge_win_regret"
                )
                if "judge_win_regret" in row
                else None,
                actual_spread_regret=_finite(
                    row["judge_spread_regret"], "judge_spread_regret"
                )
                if "judge_spread_regret" in row
                else None,
            )
            key = (position, point.plies, point.target_nodes)
            previous = raw_points.setdefault(key, point)
            if previous != point:
                raise ValueError(f"conflicting duplicate point {key}")
    points = [
        point
        for key, point in raw_points.items()
        if (key[0], key[1]) in completed
    ]
    points.sort(key=lambda point: (point.game, point.position, point.plies, point.target_nodes))
    if not points:
        raise ValueError("thinking-curve log has no completed fixed-budget points")
    return points


STATE_LEVELS: tuple[tuple[str, Callable[[Point], tuple[int, ...]]], ...] = (
    ("nodes", lambda point: (point.target_nodes,)),
    ("plies", lambda point: (point.plies, point.target_nodes)),
    ("bag", lambda point: (point.plies, point.bag_band, point.target_nodes)),
    (
        "spread",
        lambda point: (
            point.plies,
            point.bag_band,
            point.spread_band,
            point.target_nodes,
        ),
    ),
    (
        "candidates",
        lambda point: (
            point.plies,
            point.bag_band,
            point.spread_band,
            point.candidate_band,
            point.target_nodes,
        ),
    ),
)


def _state_parent_key(level_index: int, key: tuple[int, ...]) -> tuple[int, ...]:
    if level_index == 0:
        raise ValueError("root state level has no parent")
    # Every key ends in target_nodes. Remove the newest state coordinate while
    # retaining that budget coordinate.
    return (*key[:-2], key[-1])


def fit_state_model(points: list[Point], prior_strength: float) -> StateModel:
    cells: dict[tuple[str, tuple[int, ...]], Cell] = {}
    for level_index, (level, key_function) in enumerate(STATE_LEVELS):
        grouped: dict[tuple[int, ...], list[float]] = defaultdict(list)
        for point in points:
            grouped[key_function(point)].append(point.actual_regret)
        raw: dict[tuple[int, ...], tuple[float, float, int]] = {}
        for key, values in grouped.items():
            observations = len(values)
            if level_index == 0:
                weight = float(observations)
                fitted = sum(values) / observations
            else:
                parent_level = STATE_LEVELS[level_index - 1][0]
                parent = cells[(parent_level, _state_parent_key(level_index, key))]
                weight = observations + prior_strength
                fitted = (sum(values) + prior_strength * parent.mean) / weight
            raw[key] = (fitted, weight, observations)

        by_state: dict[tuple[int, ...], list[tuple[int, tuple[int, ...]]]] = defaultdict(list)
        for key in raw:
            by_state[key[:-1]].append((key[-1], key))
        for budget_cells in by_state.values():
            budget_cells.sort()
            values = [raw[key][0] for _, key in budget_cells]
            weights = [raw[key][1] for _, key in budget_cells]
            fitted_values = pava_nonincreasing(values, weights)
            for (_, key), fitted in zip(budget_cells, fitted_values):
                cells[(level, key)] = Cell(
                    mean=max(0.0, fitted), observations=raw[key][2]
                )
    return StateModel(cells)


def predict_state(model: StateModel, point: Point) -> float:
    for level, key_function in reversed(STATE_LEVELS):
        cell = model.cells.get((level, key_function(point)))
        if cell is not None:
            return cell.mean
    raise ValueError("state model lacks a target-node cell")


CORRECTION_LEVELS: tuple[tuple[str, Callable[[Point], tuple[int, ...]]], ...] = (
    ("global", lambda point: ()),
    (
        "estimated",
        lambda point: (
            int(point.challenger_valid),
            point.estimated_regret_band,
        ),
    ),
    (
        "plies",
        lambda point: (
            point.plies,
            int(point.challenger_valid),
            point.estimated_regret_band,
        ),
    ),
    (
        "nodes",
        lambda point: (
            point.plies,
            point.target_nodes,
            int(point.challenger_valid),
            point.estimated_regret_band,
        ),
    ),
    (
        "gap",
        lambda point: (
            point.plies,
            point.target_nodes,
            int(point.challenger_valid),
            point.estimated_regret_band,
            point.gap_band,
        ),
    ),
)


def fit_correction_model(
    points: list[Point], state_model: StateModel, prior_strength: float
) -> CorrectionModel:
    residuals = {
        point: point.actual_regret - predict_state(state_model, point)
        for point in points
    }
    cells: dict[tuple[str, tuple[int, ...]], Cell] = {}
    for level_index, (level, key_function) in enumerate(CORRECTION_LEVELS):
        grouped: dict[tuple[int, ...], list[float]] = defaultdict(list)
        for point in points:
            grouped[key_function(point)].append(residuals[point])
        for key, values in grouped.items():
            observations = len(values)
            if level_index == 0:
                fitted = sum(values) / observations
            else:
                parent_level, parent_function = CORRECTION_LEVELS[level_index - 1]
                exemplar = next(point for point in points if key_function(point) == key)
                parent = cells[(parent_level, parent_function(exemplar))]
                fitted = (sum(values) + prior_strength * parent.mean) / (
                    observations + prior_strength
                )
            cells[(level, key)] = Cell(fitted, observations)
    return CorrectionModel(cells)


def predict_checkpoint(
    state_model: StateModel, correction_model: CorrectionModel, point: Point
) -> float:
    correction = 0.0
    for level, key_function in reversed(CORRECTION_LEVELS):
        cell = correction_model.cells.get((level, key_function(point)))
        if cell is not None:
            correction = cell.mean
            break
    return max(0.0, predict_state(state_model, point) + correction)


def fold_for_game(game: str, folds: int, seed: int) -> int:
    digest = hashlib.sha256(f"{seed}\0{game}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") % folds


def cross_fit(
    points: list[Point], folds: int, seed: int, prior_strength: float
) -> list[Prediction]:
    games = {point.game for point in points}
    if folds < 2 or folds > len(games):
        raise ValueError("fold count must be between 2 and the number of games")
    assignments = {game: fold_for_game(game, folds, seed) for game in games}
    if len(set(assignments.values())) < folds:
        raise ValueError("at least one cross-fitting fold is empty")
    predictions: list[Prediction] = []
    for fold in range(folds):
        training = [point for point in points if assignments[point.game] != fold]
        held_out = [point for point in points if assignments[point.game] == fold]
        state_model = fit_state_model(training, prior_strength)
        correction_model = fit_correction_model(training, state_model, prior_strength)
        for point in held_out:
            predictions.append(
                Prediction(
                    point=point,
                    fold=fold,
                    state_expected_regret=predict_state(state_model, point),
                    checkpoint_expected_regret=predict_checkpoint(
                        state_model, correction_model, point
                    ),
                )
            )
    predictions.sort(
        key=lambda prediction: (
            prediction.point.game,
            prediction.point.position,
            prediction.point.plies,
            prediction.point.target_nodes,
        )
    )
    return predictions


def metrics(predictions: list[Prediction], attribute: str) -> dict[str, object]:
    errors = [
        getattr(prediction, attribute) - prediction.point.actual_regret
        for prediction in predictions
    ]
    by_game: dict[str, list[float]] = defaultdict(list)
    for prediction, error in zip(predictions, errors):
        by_game[prediction.point.game].append(error)
    game_errors = [sum(values) / len(values) for values in by_game.values()]
    mean_game_error = sum(game_errors) / len(game_errors)
    if len(game_errors) >= 2:
        variance = sum((value - mean_game_error) ** 2 for value in game_errors) / (
            len(game_errors) - 1
        )
        half_width = student_t_critical(
            0.95, len(game_errors) - 1
        ) * math.sqrt(variance / len(game_errors))
    else:
        half_width = math.nan
    return {
        "rows": len(errors),
        "games": len(by_game),
        "mae": sum(abs(error) for error in errors) / len(errors),
        "rmse": math.sqrt(sum(error * error for error in errors) / len(errors)),
        "bias": sum(errors) / len(errors),
        "game_clustered_bias": mean_game_error,
        "game_clustered_bias_ci95": [
            mean_game_error - half_width,
            mean_game_error + half_width,
        ],
    }


def clustered_summary(values: Iterable[tuple[str, float]]) -> dict[str, object]:
    by_game: dict[str, list[float]] = defaultdict(list)
    for game, value in values:
        if math.isfinite(value):
            by_game[game].append(value)
    clustered = [sum(group) / len(group) for group in by_game.values()]
    if not clustered:
        return {"rows": 0, "games": 0, "mean": None, "ci95": None, "p_value": None}
    mean = sum(clustered) / len(clustered)
    if len(clustered) < 2:
        return {
            "rows": sum(len(group) for group in by_game.values()),
            "games": len(clustered),
            "mean": mean,
            "ci95": None,
            "p_value": None,
        }
    variance = sum((value - mean) ** 2 for value in clustered) / (
        len(clustered) - 1
    )
    standard_error = math.sqrt(variance / len(clustered))
    degrees_freedom = len(clustered) - 1
    half_width = student_t_critical(0.95, degrees_freedom) * standard_error
    p_value = (
        student_t_two_sided_p_value(mean / standard_error, degrees_freedom)
        if standard_error > 0.0
        else 0.0 if mean != 0.0 else 1.0
    )
    return {
        "rows": sum(len(group) for group in by_game.values()),
        "games": len(clustered),
        "mean": mean,
        "ci95": [mean - half_width, mean + half_width],
        "p_value": p_value,
    }


def nonzero_regret_summary(points: Iterable[Point]) -> dict[str, object]:
    """Summarize whether a source game contains any observed regret event.

    The exact zero-event upper bound uses source games rather than positions,
    so repeated roots from one game cannot create artificial precision. The
    Wilson interval is descriptive for nonzero panels; inference on regret
    magnitude remains in :func:`clustered_summary`.
    """

    by_game: dict[str, list[bool]] = defaultdict(list)
    positions = 0
    nonzero_positions = 0
    for point in points:
        event = point.actual_regret > 0.0
        by_game[point.game].append(event)
        positions += 1
        nonzero_positions += int(event)
    games = len(by_game)
    nonzero_games = sum(any(events) for events in by_game.values())
    if games == 0:
        return {
            "positions": 0,
            "nonzero_positions": 0,
            "games": 0,
            "nonzero_games": 0,
            "game_event_rate": None,
            "wilson_ci95": None,
            "one_sided_upper95_if_zero": None,
        }

    rate = nonzero_games / games
    z = 1.959963984540054
    denominator = 1.0 + z * z / games
    center = (rate + z * z / (2.0 * games)) / denominator
    half_width = (
        z
        * math.sqrt(
            rate * (1.0 - rate) / games
            + z * z / (4.0 * games * games)
        )
        / denominator
    )
    return {
        "positions": positions,
        "nonzero_positions": nonzero_positions,
        "games": games,
        "nonzero_games": nonzero_games,
        "game_event_rate": rate,
        "wilson_ci95": [
            max(0.0, center - half_width),
            min(1.0, center + half_width),
        ],
        # With no event in n independent source-game clusters, solve
        # (1 - p)^n = .05 for the one-sided 95% upper bound.
        "one_sided_upper95_if_zero": (
            1.0 - math.pow(0.05, 1.0 / games)
            if nonzero_games == 0
            else None
        ),
    }


def fixed_budget_summaries(predictions: list[Prediction]) -> list[dict[str, object]]:
    grouped: dict[tuple[int, int], list[Prediction]] = defaultdict(list)
    for prediction in predictions:
        grouped[(prediction.point.plies, prediction.point.target_nodes)].append(
            prediction
        )
    summaries: list[dict[str, object]] = []
    for (plies, nodes), group in sorted(grouped.items()):
        summaries.append(
            {
                "plies": plies,
                "target_nodes": nodes,
                "actual_utility_regret": clustered_summary(
                    (item.point.game, item.point.actual_regret) for item in group
                ),
                "actual_win_regret": clustered_summary(
                    (item.point.game, item.point.actual_win_regret)
                    for item in group
                    if item.point.actual_win_regret is not None
                ),
                "actual_spread_regret": clustered_summary(
                    (item.point.game, item.point.actual_spread_regret)
                    for item in group
                    if item.point.actual_spread_regret is not None
                ),
                "nonzero_utility_regret": nonzero_regret_summary(
                    item.point for item in group
                ),
                "state_prediction": clustered_summary(
                    (item.point.game, item.state_expected_regret) for item in group
                ),
                "checkpoint_prediction": clustered_summary(
                    (item.point.game, item.checkpoint_expected_regret)
                    for item in group
                ),
            }
        )
    return summaries


def paired_tail_summaries(points: list[Point]) -> dict[str, object]:
    series: dict[tuple[str, int, int], dict[int, Point]] = defaultdict(dict)
    for point in points:
        series[(point.game, point.position, point.plies)][point.target_nodes] = point
    nodes_by_plies: dict[int, set[int]] = defaultdict(set)
    for point in points:
        nodes_by_plies[point.plies].add(point.target_nodes)

    overall: list[dict[str, object]] = []
    by_bag: list[dict[str, object]] = []
    by_policy: list[dict[str, object]] = []
    for plies, node_values in sorted(nodes_by_plies.items()):
        baseline = min(node_values)
        for target in sorted(node_values):
            if target == baseline:
                continue
            pairs = [
                (values[baseline], values[target])
                for (game, _, point_plies), values in series.items()
                if point_plies == plies and baseline in values and target in values
            ]
            utility = [
                (before.game, before.actual_regret - after.actual_regret)
                for before, after in pairs
            ]
            win = [
                (
                    before.game,
                    before.actual_win_regret - after.actual_win_regret,
                )
                for before, after in pairs
                if before.actual_win_regret is not None
                and after.actual_win_regret is not None
            ]
            spread = [
                (
                    before.game,
                    before.actual_spread_regret - after.actual_spread_regret,
                )
                for before, after in pairs
                if before.actual_spread_regret is not None
                and after.actual_spread_regret is not None
            ]
            overall.append(
                {
                    "plies": plies,
                    "baseline_nodes": baseline,
                    "target_nodes": target,
                    "utility_regret_reduction": clustered_summary(utility),
                    "win_regret_reduction": clustered_summary(win),
                    "spread_regret_reduction": clustered_summary(spread),
                    "choice_change_rate": (
                        sum(
                            before.selected_rank != after.selected_rank
                            for before, after in pairs
                            if before.selected_rank >= 0
                            and after.selected_rank >= 0
                        )
                        / sum(
                            before.selected_rank >= 0 and after.selected_rank >= 0
                            for before, after in pairs
                        )
                        if any(
                            before.selected_rank >= 0 and after.selected_rank >= 0
                            for before, after in pairs
                        )
                        else None
                    ),
                }
            )
            for bag in sorted({before.bag_band for before, _ in pairs}):
                bag_pairs = [pair for pair in pairs if pair[0].bag_band == bag]
                by_bag.append(
                    {
                        "plies": plies,
                        "baseline_nodes": baseline,
                        "target_nodes": target,
                        "bag_band": bag,
                        "positions": len(bag_pairs),
                        "utility_regret_reduction": clustered_summary(
                            (
                                before.game,
                                before.actual_regret - after.actual_regret,
                            )
                            for before, after in bag_pairs
                        ),
                    }
                )
            for policy in sorted({before.policy for before, _ in pairs}):
                policy_pairs = [pair for pair in pairs if pair[0].policy == policy]
                by_policy.append(
                    {
                        "plies": plies,
                        "baseline_nodes": baseline,
                        "target_nodes": target,
                        "policy": policy,
                        "positions": len(policy_pairs),
                        "utility_regret_reduction": clustered_summary(
                            (
                                before.game,
                                before.actual_regret - after.actual_regret,
                            )
                            for before, after in policy_pairs
                        ),
                    }
                )
    return {"overall": overall, "by_bag": by_bag, "by_policy": by_policy}


def _matched_depth_row(
    pairs: list[tuple[Point, Point]],
    shallow_plies: int,
    deep_plies: int,
    target_nodes: int,
) -> dict[str, object]:
    utility = [
        (shallow.game, shallow.actual_regret - deep.actual_regret)
        for shallow, deep in pairs
    ]
    win = [
        (
            shallow.game,
            shallow.actual_win_regret - deep.actual_win_regret,
        )
        for shallow, deep in pairs
        if shallow.actual_win_regret is not None
        and deep.actual_win_regret is not None
    ]
    spread = [
        (
            shallow.game,
            shallow.actual_spread_regret - deep.actual_spread_regret,
        )
        for shallow, deep in pairs
        if shallow.actual_spread_regret is not None
        and deep.actual_spread_regret is not None
    ]
    comparable_choices = [
        (shallow, deep)
        for shallow, deep in pairs
        if shallow.selected_rank >= 0 and deep.selected_rank >= 0
    ]
    return {
        "shallow_plies": shallow_plies,
        "deep_plies": deep_plies,
        "target_nodes": target_nodes,
        "positions": len(pairs),
        # Positive regret reduction means the deeper arm is better.
        "utility_regret_reduction": clustered_summary(utility),
        "win_regret_reduction": clustered_summary(win),
        "spread_regret_reduction": clustered_summary(spread),
        "choice_change": clustered_summary(
            (
                shallow.game,
                float(shallow.selected_rank != deep.selected_rank),
            )
            for shallow, deep in comparable_choices
        ),
        "selection_counts": {
            "deep_better": sum(
                deep.actual_regret < shallow.actual_regret
                for shallow, deep in pairs
            ),
            "shallow_better": sum(
                shallow.actual_regret < deep.actual_regret
                for shallow, deep in pairs
            ),
            "tied": sum(
                shallow.actual_regret == deep.actual_regret
                for shallow, deep in pairs
            ),
        },
        "shallow_nonzero_utility_regret": nonzero_regret_summary(
            shallow for shallow, _ in pairs
        ),
        "deep_nonzero_utility_regret": nonzero_regret_summary(
            deep for _, deep in pairs
        ),
    }


def matched_depth_summaries(points: list[Point]) -> dict[str, object]:
    """Compare depths at identical roots and fixed node budgets.

    This deliberately compares the moves selected by each fixed arm. It does
    not choose a budget or depth after looking at oracle regret.
    """

    series: dict[tuple[str, int, int], dict[int, Point]] = defaultdict(dict)
    for point in points:
        key = (point.game, point.position, point.target_nodes)
        previous = series[key].setdefault(point.plies, point)
        if previous != point:
            raise ValueError(
                "conflicting matched-depth point "
                f"game={point.game} position={point.position} "
                f"nodes={point.target_nodes} plies={point.plies}"
            )

    plies_values = sorted({point.plies for point in points})
    overall: list[dict[str, object]] = []
    by_bag: list[dict[str, object]] = []
    by_policy: list[dict[str, object]] = []
    for shallow_plies, deep_plies in combinations(plies_values, 2):
        targets = sorted(
            {
                target_nodes
                for (_, _, target_nodes), depths in series.items()
                if shallow_plies in depths and deep_plies in depths
            }
        )
        for target_nodes in targets:
            pairs = [
                (depths[shallow_plies], depths[deep_plies])
                for (_, _, nodes), depths in series.items()
                if nodes == target_nodes
                and shallow_plies in depths
                and deep_plies in depths
            ]
            overall.append(
                _matched_depth_row(
                    pairs, shallow_plies, deep_plies, target_nodes
                )
            )
            for bag_band in sorted({shallow.bag_band for shallow, _ in pairs}):
                bag_pairs = [
                    pair for pair in pairs if pair[0].bag_band == bag_band
                ]
                row = _matched_depth_row(
                    bag_pairs, shallow_plies, deep_plies, target_nodes
                )
                row["bag_band"] = bag_band
                by_bag.append(row)
            for policy in sorted({shallow.policy for shallow, _ in pairs}):
                policy_pairs = [
                    pair for pair in pairs if pair[0].policy == policy
                ]
                row = _matched_depth_row(
                    policy_pairs, shallow_plies, deep_plies, target_nodes
                )
                row["policy"] = policy
                by_policy.append(row)
    return {"overall": overall, "by_bag": by_bag, "by_policy": by_policy}


def write_predictions(path: Path, predictions: list[Prediction]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "game",
        "position",
        "policy",
        "bag",
        "spread",
        "plies",
        "target_nodes",
        "actual_nodes",
        "candidates",
        "selected_rank",
        "estimated_regret",
        "estimated_gap",
        "challenger_valid",
        "actual_regret",
        "actual_win_regret",
        "actual_spread_regret",
        "fold",
        "state_expected_regret",
        "checkpoint_expected_regret",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for prediction in predictions:
            point = prediction.point
            writer.writerow(
                {
                    "game": point.game,
                    "position": point.position,
                    "policy": point.policy,
                    "bag": point.bag,
                    "spread": point.spread,
                    "plies": point.plies,
                    "target_nodes": point.target_nodes,
                    "actual_nodes": point.actual_nodes,
                    "candidates": point.candidates,
                    "selected_rank": point.selected_rank,
                    "estimated_regret": f"{point.estimated_regret:.12g}",
                    "estimated_gap": f"{point.estimated_gap:.12g}",
                    "challenger_valid": int(point.challenger_valid),
                    "actual_regret": f"{point.actual_regret:.12g}",
                    "actual_win_regret": ""
                    if point.actual_win_regret is None
                    else f"{point.actual_win_regret:.12g}",
                    "actual_spread_regret": ""
                    if point.actual_spread_regret is None
                    else f"{point.actual_spread_regret:.12g}",
                    "fold": prediction.fold,
                    "state_expected_regret": f"{prediction.state_expected_regret:.12g}",
                    "checkpoint_expected_regret": f"{prediction.checkpoint_expected_regret:.12g}",
                }
            )


def _cells_json(cells: dict[tuple[str, tuple[int, ...]], Cell]) -> list[dict[str, object]]:
    return [
        {
            "level": level,
            "key": list(key),
            "mean": cell.mean,
            "observations": cell.observations,
        }
        for (level, key), cell in sorted(cells.items())
    ]


def build_artifact(
    points: list[Point], predictions: list[Prediction], log: Path, corpus: Path,
    folds: int, seed: int, prior_strength: float
) -> dict[str, object]:
    state_model = fit_state_model(points, prior_strength)
    correction_model = fit_correction_model(points, state_model, prior_strength)
    return {
        "artifact_kind": "sim_checkpoint_regret_shadow",
        "artifact_version": 2,
        "source_log": str(log),
        "source_log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
        "source_corpus": str(corpus),
        "source_corpus_sha256": hashlib.sha256(corpus.read_bytes()).hexdigest(),
        "oracle_role": "held_out_scoring_only",
        "split_unit": "complete_source_game",
        "folds": folds,
        "split_seed": seed,
        "prior_strength": prior_strength,
        "live_enabled": False,
        "gate_reason": "terminal mirrored-game gate not supplied",
        "state_metrics": metrics(predictions, "state_expected_regret"),
        "checkpoint_metrics": metrics(
            predictions, "checkpoint_expected_regret"
        ),
        "fixed_budget_summaries": fixed_budget_summaries(predictions),
        "paired_tail_summaries": paired_tail_summaries(points),
        "matched_depth_summaries": matched_depth_summaries(points),
        "state_cells": _cells_json(state_model.cells),
        "checkpoint_correction_cells": _cells_json(correction_model.cells),
        "bucket_contract": {
            "bag_upper_bounds": list(BAG_UPPER_BOUNDS),
            "spread_upper_bounds": list(SPREAD_UPPER_BOUNDS),
            "candidate_upper_bounds": list(CANDIDATE_UPPER_BOUNDS),
            "estimated_regret_upper_bounds": list(
                ESTIMATED_REGRET_UPPER_BOUNDS
            ),
            "gap_upper_bounds": list(GAP_UPPER_BOUNDS),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--predictions", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--split-seed", type=int, default=82404)
    parser.add_argument("--prior-strength", type=float, default=10.0)
    args = parser.parse_args()
    if not math.isfinite(args.prior_strength) or args.prior_strength < 0.0:
        parser.error("prior-strength must be finite and nonnegative")
    points = read_points(args.log, read_metadata(args.corpus))
    predictions = cross_fit(
        points, args.folds, args.split_seed, args.prior_strength
    )
    write_predictions(args.predictions, predictions)
    artifact = build_artifact(
        points,
        predictions,
        args.log,
        args.corpus,
        args.folds,
        args.split_seed,
        args.prior_strength,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(artifact, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"points={len(points)} games={len({point.game for point in points})} "
        f"state_rmse={artifact['state_metrics']['rmse']:.9f} "
        f"checkpoint_rmse={artifact['checkpoint_metrics']['rmse']:.9f} "
        f"live_enabled=0 output={args.output}"
    )


if __name__ == "__main__":
    main()

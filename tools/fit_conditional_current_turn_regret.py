#!/usr/bin/env python3
"""Fit a cross-fitted two-part current-turn regret model.

Current-turn regret is sparse: most checkpoints select a common-judge-optimal
move, while a small minority carry materially positive regret.  A single cell
mean confounds the probability of a miss with its severity.  This tool fits
those two quantities separately for candidate generation and within-set BAI,
then multiplies them to recover expected regret.

The point estimate remains the economic signal.  A separate one-sided,
game-clustered conformal residual addend is exported as a fail-closed safety
bound.  Oracle judge values are labels only, folds are assigned by complete
source game, and inputs marked ``INVALID_PROTOCOL.txt`` are rejected.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import asdict, dataclass, replace
import json
import math
from pathlib import Path
import statistics
from typing import Callable, Iterable

try:
    from tools.analyze_candidate_miss import parse_fields
    from tools.fit_total_current_turn_regret import (
        CANDIDATE_LEVELS,
        Cell,
        ComponentModel,
        KeyFunction,
        Row,
        _serialize_model,
        error_metrics,
        fit_component_model,
        fold_for_game,
        predict_component,
        read_rows,
        reliability,
    )
except ModuleNotFoundError:  # Direct execution from tools/.
    from analyze_candidate_miss import parse_fields  # type: ignore[no-redef]
    from fit_total_current_turn_regret import (  # type: ignore[no-redef]
        CANDIDATE_LEVELS,
        Cell,
        ComponentModel,
        KeyFunction,
        Row,
        _serialize_model,
        error_metrics,
        fit_component_model,
        fold_for_game,
        predict_component,
        read_rows,
        reliability,
    )


WORK_NODE_UPPER_BOUNDS = (
    300_000,
    600_000,
    1_000_000,
    1_500_000,
    2_000_000,
    2_500_000,
    3_000_000,
    10_000_000,
    1 << 62,
)

CHECKPOINT_NODE_LANDMARKS = (
    300_000,
    600_000,
    1_000_000,
    1_500_000,
    2_000_000,
    2_500_000,
)

STABLE_CHECKPOINT_UPPER_BOUNDS = (1, 2, 4, 8, 16, 32, 1 << 30)


def work_band(row: Row) -> int:
    for index, upper in enumerate(WORK_NODE_UPPER_BOUNDS):
        if row.target_nodes <= upper:
            return index
    return len(WORK_NODE_UPPER_BOUNDS) - 1


def stability_band(row: Row) -> int:
    if row.stable_checkpoints <= 0:
        return 0
    for index, upper in enumerate(STABLE_CHECKPOINT_UPPER_BOUNDS, start=1):
        if row.stable_checkpoints <= upper:
            return index
    return len(STABLE_CHECKPOINT_UPPER_BOUNDS)


CANDIDATE_SEVERITY_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.width,),
    lambda row: (row.width, row.bag_band),
)


WITHIN_EVENT_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.width,),
    lambda row: (row.width, row.estimated_regret_band),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        stability_band(row),
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        stability_band(row),
        row.bai_gap_band,
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        stability_band(row),
        row.bai_gap_band,
        work_band(row),
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        stability_band(row),
        row.bai_gap_band,
        work_band(row),
        row.bag_band,
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        stability_band(row),
        row.bai_gap_band,
        work_band(row),
        row.bag_band,
        row.policy,
    ),
)


WITHIN_SEVERITY_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.width,),
    lambda row: (row.width, row.near_tie_band),
    lambda row: (row.width, row.near_tie_band, stability_band(row)),
    lambda row: (
        row.width,
        row.near_tie_band,
        stability_band(row),
        work_band(row),
    ),
    lambda row: (
        row.width,
        row.near_tie_band,
        stability_band(row),
        work_band(row),
        row.bag_band,
    ),
    lambda row: (
        row.width,
        row.near_tie_band,
        stability_band(row),
        work_band(row),
        row.bag_band,
        row.policy,
    ),
)


@dataclass(frozen=True)
class TwoPartComponentModel:
    event_probability: ComponentModel
    positive_severity: ComponentModel
    event_levels: tuple[KeyFunction, ...]
    severity_levels: tuple[KeyFunction, ...]


@dataclass(frozen=True)
class ComponentPrediction:
    event_probability: float
    positive_severity: float

    @property
    def expected_regret(self) -> float:
        return self.event_probability * self.positive_severity


@dataclass(frozen=True)
class ConditionalPrediction:
    row: Row
    fold: int
    candidate: ComponentPrediction
    within: ComponentPrediction
    point_calibration_scale: float = 1.0
    upper_residual_addend: float = 0.0
    calibration_level: str = "none"
    calibration_games: int = 0

    @property
    def predicted_candidate_generation_regret(self) -> float:
        return self.candidate.expected_regret * self.point_calibration_scale

    @property
    def predicted_within_set_bai_regret(self) -> float:
        return self.within.expected_regret * self.point_calibration_scale

    @property
    def raw_total_current_turn_regret(self) -> float:
        return self.candidate.expected_regret + self.within.expected_regret

    @property
    def predicted_total_current_turn_regret(self) -> float:
        return (
            self.predicted_candidate_generation_regret
            + self.predicted_within_set_bai_regret
        )

    @property
    def upper_total_current_turn_regret(self) -> float:
        return self.predicted_total_current_turn_regret + max(
            0.0, self.upper_residual_addend
        )


def _positive(label: Callable[[Row], float], epsilon: float) -> Callable[[Row], float]:
    return lambda row: float(label(row) > epsilon)


def fit_two_part_component(
    rows: list[Row],
    event_levels: tuple[KeyFunction, ...],
    severity_levels: tuple[KeyFunction, ...],
    label: Callable[[Row], float],
    event_prior_strength: float,
    severity_prior_strength: float,
    positive_epsilon: float,
) -> TwoPartComponentModel:
    positive_rows = [row for row in rows if label(row) > positive_epsilon]
    if not positive_rows:
        zero = ComponentModel({(0, ()): Cell(0.0, len(rows))})
        return TwoPartComponentModel(
            event_probability=zero,
            positive_severity=zero,
            event_levels=event_levels,
            severity_levels=severity_levels,
        )
    return TwoPartComponentModel(
        event_probability=fit_component_model(
            rows,
            event_levels,
            _positive(label, positive_epsilon),
            event_prior_strength,
        ),
        positive_severity=fit_component_model(
            positive_rows,
            severity_levels,
            label,
            severity_prior_strength,
        ),
        event_levels=event_levels,
        severity_levels=severity_levels,
    )


def predict_two_part_component(
    model: TwoPartComponentModel, row: Row
) -> ComponentPrediction:
    probability = predict_component(
        model.event_probability, row, model.event_levels
    )
    severity = predict_component(
        model.positive_severity, row, model.severity_levels
    )
    if probability < 0.0 or probability > 1.0 + 1.0e-12:
        raise ValueError("two-part event probability is outside [0,1]")
    return ComponentPrediction(min(1.0, probability), severity)


def fit_models(
    rows: list[Row],
    event_prior_strength: float,
    severity_prior_strength: float,
    positive_epsilon: float,
) -> tuple[TwoPartComponentModel, TwoPartComponentModel]:
    candidate = fit_two_part_component(
        rows,
        CANDIDATE_LEVELS,
        CANDIDATE_SEVERITY_LEVELS,
        lambda row: row.candidate_generation_regret,
        event_prior_strength,
        severity_prior_strength,
        positive_epsilon,
    )
    within = fit_two_part_component(
        rows,
        WITHIN_EVENT_LEVELS,
        WITHIN_SEVERITY_LEVELS,
        lambda row: row.within_set_bai_regret,
        event_prior_strength,
        severity_prior_strength,
        positive_epsilon,
    )
    return candidate, within


def _assign_folds(rows: list[Row], folds: int, seed: int) -> dict[str, int]:
    games = {row.game for row in rows}
    if folds < 2 or folds > len(games):
        raise ValueError("fold count must be between 2 and number of games")
    assignments = {game: fold_for_game(game, folds, seed) for game in games}
    if len(set(assignments.values())) != folds:
        raise ValueError("one or more cross-fitting folds are empty")
    return assignments


def cross_fit(
    rows: list[Row],
    folds: int,
    seed: int,
    event_prior_strength: float,
    severity_prior_strength: float,
    positive_epsilon: float,
) -> list[ConditionalPrediction]:
    assignments = _assign_folds(rows, folds, seed)
    predictions: list[ConditionalPrediction] = []
    for fold in range(folds):
        training = [row for row in rows if assignments[row.game] != fold]
        held_out = [row for row in rows if assignments[row.game] == fold]
        candidate, within = fit_models(
            training,
            event_prior_strength,
            severity_prior_strength,
            positive_epsilon,
        )
        for row in held_out:
            predictions.append(
                ConditionalPrediction(
                    row=row,
                    fold=fold,
                    candidate=predict_two_part_component(candidate, row),
                    within=predict_two_part_component(within, row),
                )
            )
    return sorted(predictions, key=lambda item: (item.row.game, item.row.width))


def _predict_rows(
    training: list[Row],
    held_out: list[Row],
    fold: int,
    event_prior_strength: float,
    severity_prior_strength: float,
    positive_epsilon: float,
) -> list[ConditionalPrediction]:
    candidate, within = fit_models(
        training,
        event_prior_strength,
        severity_prior_strength,
        positive_epsilon,
    )
    return [
        ConditionalPrediction(
            row=row,
            fold=fold,
            candidate=predict_two_part_component(candidate, row),
            within=predict_two_part_component(within, row),
        )
        for row in held_out
    ]


@dataclass(frozen=True)
class CalibrationCell:
    level: str
    key: tuple[str, ...]
    addend: float
    games: int


@dataclass(frozen=True)
class IsotonicBlock:
    upper_prediction: float
    calibrated_regret: float
    rows: int


def fit_isotonic_calibrator(
    predictions: list[ConditionalPrediction],
    deployment_width: int,
    prior_strength: float,
) -> list[IsotonicBlock]:
    """Fit a monotone expected-regret calibration map.

    The prior shrinks each observation toward its raw two-part prediction.
    This is calibration of a risk score, not monotone forcing across search
    boundaries: no deeper result or oracle option is discarded.
    """

    if prior_strength < 0.0 or not math.isfinite(prior_strength):
        raise ValueError("isotonic prior strength must be finite and nonnegative")
    deployed = sorted(
        (
            item.raw_total_current_turn_regret,
            item.row.total_current_turn_regret,
        )
        for item in predictions
        if item.row.width == deployment_width
    )
    if not deployed:
        raise ValueError("isotonic calibrator has no deployment-width rows")

    # [upper x, weighted y sum, weight, raw row count]
    blocks: list[list[float]] = []
    for raw, actual in deployed:
        weight = 1.0 + prior_strength
        value_sum = actual + prior_strength * raw
        if blocks and raw == blocks[-1][0]:
            blocks[-1][1] += value_sum
            blocks[-1][2] += weight
            blocks[-1][3] += 1.0
        else:
            blocks.append([raw, value_sum, weight, 1.0])
        while (
            len(blocks) >= 2
            and blocks[-2][1] / blocks[-2][2]
            > blocks[-1][1] / blocks[-1][2]
        ):
            right = blocks.pop()
            left = blocks.pop()
            blocks.append(
                [
                    right[0],
                    left[1] + right[1],
                    left[2] + right[2],
                    left[3] + right[3],
                ]
            )
    return [
        IsotonicBlock(
            upper_prediction=block[0],
            calibrated_regret=max(0.0, block[1] / block[2]),
            rows=int(block[3]),
        )
        for block in blocks
    ]


def apply_isotonic_calibrator(
    predictions: list[ConditionalPrediction],
    blocks: list[IsotonicBlock],
    deployment_width: int,
) -> list[ConditionalPrediction]:
    if not blocks:
        raise ValueError("isotonic calibrator has no blocks")
    result: list[ConditionalPrediction] = []
    for item in predictions:
        if item.row.width != deployment_width:
            result.append(item)
            continue
        raw = item.raw_total_current_turn_regret
        selected = blocks[-1]
        for block in blocks:
            if raw <= block.upper_prediction:
                selected = block
                break
        if raw <= 0.0:
            if selected.calibrated_regret > 0.0:
                raise ValueError("cannot distribute positive calibration over zero components")
            scale = 1.0
        else:
            scale = selected.calibrated_regret / raw
        result.append(replace(item, point_calibration_scale=scale))
    return result


def _conformal_quantile(values: Iterable[float], coverage: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("conformal quantile needs observations")
    rank = math.ceil((len(ordered) + 1) * coverage)
    return ordered[min(len(ordered), max(1, rank)) - 1]


def fit_calibration_cells(
    predictions: list[ConditionalPrediction],
    deployment_width: int,
    coverage: float,
    minimum_games: int,
) -> list[CalibrationCell]:
    if coverage <= 0.5 or coverage >= 1.0:
        raise ValueError("coverage must be between 0.5 and 1")
    if minimum_games < 2:
        raise ValueError("minimum calibration games must be at least 2")
    deployed = [item for item in predictions if item.row.width == deployment_width]
    if not deployed:
        raise ValueError("deployment width has no cross-fitted predictions")

    level_keys: tuple[tuple[str, Callable[[Row], tuple[str, ...]]], ...] = (
        ("global", lambda row: ()),
        ("bag", lambda row: (row.bag_band,)),
        ("policy", lambda row: (row.policy,)),
        ("policy_bag", lambda row: (row.policy, row.bag_band)),
    )
    cells: list[CalibrationCell] = []
    for level, key_function in level_keys:
        grouped: dict[tuple[str, ...], dict[str, list[float]]] = defaultdict(
            lambda: defaultdict(list)
        )
        for item in deployed:
            residual = (
                item.row.total_current_turn_regret
                - item.predicted_total_current_turn_regret
            )
            grouped[key_function(item.row)][item.row.game].append(residual)
        for key, by_game in grouped.items():
            if level != "global" and len(by_game) < minimum_games:
                continue
            # A game is the exchangeability unit.  The maximum residual gives
            # simultaneous protection if a future corpus contributes more
            # than one deployment-width row from the same complete game.
            game_scores = [max(values) for values in by_game.values()]
            cells.append(
                CalibrationCell(
                    level=level,
                    key=key,
                    addend=max(0.0, _conformal_quantile(game_scores, coverage)),
                    games=len(game_scores),
                )
            )
    if not any(cell.level == "global" for cell in cells):
        raise ValueError("conformal calibration lacks a global fallback")
    return cells


def _calibration_key(level: str, row: Row) -> tuple[str, ...]:
    if level == "global":
        return ()
    if level == "bag":
        return (row.bag_band,)
    if level == "policy":
        return (row.policy,)
    if level == "policy_bag":
        return (row.policy, row.bag_band)
    raise ValueError(f"unknown calibration level {level}")


def apply_calibration_cells(
    predictions: list[ConditionalPrediction], cells: list[CalibrationCell]
) -> list[ConditionalPrediction]:
    by_coordinate = {(cell.level, cell.key): cell for cell in cells}
    preference = ("policy_bag", "policy", "bag", "global")
    result: list[ConditionalPrediction] = []
    for item in predictions:
        selected = None
        for level in preference:
            selected = by_coordinate.get((level, _calibration_key(level, item.row)))
            if selected is not None:
                break
        if selected is None:
            raise ValueError("conformal calibration lacks a matching fallback")
        result.append(
            replace(
                item,
                upper_residual_addend=selected.addend,
                calibration_level=selected.level,
                calibration_games=selected.games,
            )
        )
    return result


def nested_cross_fit_with_bounds(
    rows: list[Row],
    folds: int,
    inner_folds: int,
    seed: int,
    event_prior_strength: float,
    severity_prior_strength: float,
    positive_epsilon: float,
    deployment_width: int,
    coverage: float,
    minimum_games: int,
    isotonic_prior_strength: float,
) -> tuple[
    list[ConditionalPrediction], list[CalibrationCell], list[IsotonicBlock]
]:
    """Cross-fit both the point model and its residual safety bound.

    For each outer held-out fold, conformal scores come from an inner
    cross-fit performed entirely inside the outer training games.  Thus no
    label from the held-out fold can affect either its point prediction or its
    upper residual addend.  Final runtime calibration cells are separately
    fit from ordinary out-of-fold residuals over the full development corpus;
    only a subsequent fresh panel may validate those final cells.
    """

    assignments = _assign_folds(rows, folds, seed)
    bounded: list[ConditionalPrediction] = []
    ordinary: list[ConditionalPrediction] = []
    for outer_fold in range(folds):
        outer_training = [
            row for row in rows if assignments[row.game] != outer_fold
        ]
        outer_held = [row for row in rows if assignments[row.game] == outer_fold]
        held_predictions = _predict_rows(
            outer_training,
            outer_held,
            outer_fold,
            event_prior_strength,
            severity_prior_strength,
            positive_epsilon,
        )
        ordinary.extend(held_predictions)

        inner_seed = seed + 10_000 + outer_fold
        inner_assignments = _assign_folds(
            outer_training, inner_folds, inner_seed
        )
        calibration_predictions: list[ConditionalPrediction] = []
        for inner_fold in range(inner_folds):
            inner_training = [
                row
                for row in outer_training
                if inner_assignments[row.game] != inner_fold
            ]
            inner_held = [
                row
                for row in outer_training
                if inner_assignments[row.game] == inner_fold
            ]
            calibration_predictions.extend(
                _predict_rows(
                    inner_training,
                    inner_held,
                    inner_fold,
                    event_prior_strength,
                    severity_prior_strength,
                    positive_epsilon,
                )
            )
        isotonic_blocks = fit_isotonic_calibrator(
            calibration_predictions,
            deployment_width,
            isotonic_prior_strength,
        )
        calibration_predictions = apply_isotonic_calibrator(
            calibration_predictions, isotonic_blocks, deployment_width
        )
        held_predictions = apply_isotonic_calibrator(
            held_predictions, isotonic_blocks, deployment_width
        )
        cells = fit_calibration_cells(
            calibration_predictions,
            deployment_width,
            coverage,
            minimum_games,
        )
        bounded.extend(apply_calibration_cells(held_predictions, cells))

    ordinary = sorted(ordinary, key=lambda item: (item.row.game, item.row.width))
    bounded = sorted(bounded, key=lambda item: (item.row.game, item.row.width))
    final_isotonic = fit_isotonic_calibrator(
        ordinary, deployment_width, isotonic_prior_strength
    )
    calibrated_ordinary = apply_isotonic_calibrator(
        ordinary, final_isotonic, deployment_width
    )
    final_cells = fit_calibration_cells(
        calibrated_ordinary, deployment_width, coverage, minimum_games
    )
    return bounded, final_cells, final_isotonic


def _reject_invalid_protocol(path: Path) -> None:
    marker = path.parent / "INVALID_PROTOCOL.txt"
    if marker.exists():
        raise ValueError(f"refusing invalid protocol input: {marker}")


def _metadata_by_position(manifest: Path) -> dict[int, dict[str, object]]:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    mapping = {
        int(item["position"]): item for item in document.get("mapping", [])
    }
    if not mapping:
        raise ValueError("selection manifest has no root mapping")
    return mapping


def _finite_nonnegative(values: Iterable[str], field: str) -> float:
    for raw in values:
        try:
            value = float(raw)
        except (TypeError, ValueError):
            continue
        if math.isfinite(value) and value >= 0.0:
            return value
    raise ValueError(f"{field} lacks a finite nonnegative value")


def read_combined_rows(
    log: Path,
    manifest: Path,
    panel_name: str,
    source_offset: int,
) -> list[Row]:
    """Read one corrected cumulative stopped checkpoint per source game."""

    _reject_invalid_protocol(log)
    metadata = _metadata_by_position(manifest)
    positions: dict[int, dict[str, str]] = {}
    points: dict[int, dict[str, str]] = {}
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION "):
                fields = parse_fields(line)
                source = int(fields["source_index"])
                if source in positions:
                    raise ValueError(f"duplicate combined position {source}")
                positions[source] = fields
            elif line.startswith("THINKING_CURVE_POINT "):
                fields = parse_fields(line)
                if (
                    fields.get("final") != "0"
                    or fields.get("control_kind", "stopped") != "stopped"
                ):
                    continue
                source = int(fields["source_index"])
                if source in points:
                    raise ValueError(f"duplicate combined stopped point {source}")
                points[source] = fields

    rows: list[Row] = []
    for source, point in sorted(points.items()):
        position = positions.get(source)
        if position is None:
            raise ValueError(f"combined source {source} lacks position metadata")
        position_index = int(point.get("position", source))
        info = metadata.get(position_index)
        if info is None:
            raise ValueError(f"combined position {position_index} lacks manifest row")
        best = float(point["estimated_best"])
        challenger = float(point["estimated_challenger"])
        bai_gap = (
            max(0.0, best - challenger)
            if math.isfinite(best) and math.isfinite(challenger)
            else 1.0
        )
        near_tie = int(point.get("near_tie_challengers_at_stop", "-1"))
        if near_tie < 0:
            near_tie = int(point["near_tie_challengers"])
        actual = _finite_nonnegative((point["judge_regret"],), "judge_regret")
        rows.append(
            Row(
                source_index=source_offset + source,
                game=(
                    f"{panel_name}:{info['source_seed']}:{info['start']}"
                ),
                policy=str(info["trajectory_policy"]),
                bag=int(info["bag"]),
                bag_band=str(info["bag_band"]),
                width=int(point["arm_plays"]),
                target_nodes=int(point["nodes"]),
                static_cutoff_gap=float(position["static_primary_gap"]),
                estimated_regret=_finite_nonnegative(
                    (
                        point.get("regret_at_stop", "nan"),
                        point.get("estimated_regret", "nan"),
                    ),
                    "estimated_regret",
                ),
                joint_estimated_regret=_finite_nonnegative(
                    (
                        point.get("joint_regret_at_stop", "nan"),
                        point.get("joint_estimated_regret", "nan"),
                    ),
                    "joint_estimated_regret",
                ),
                bai_gap=bai_gap,
                near_tie_challengers=near_tie,
                # The common checkpoint-observable top-K union is the label
                # scope.  Moves outside that union remain unmeasured.
                candidate_generation_regret=0.0,
                within_set_bai_regret=actual,
                total_current_turn_regret=actual,
            )
        )
    if len(rows) != len(positions):
        raise ValueError("combined panel lacks exactly one stopped point per root")
    return rows


def _checkpoint_landmarks(
    checkpoints: list[dict[str, str]],
) -> list[dict[str, str]]:
    """Select fixed work landmarks plus the final observable checkpoint."""

    valid: list[dict[str, str]] = []
    for checkpoint in sorted(
        checkpoints, key=lambda item: int(item["checkpoint"])
    ):
        try:
            estimates = (
                float(checkpoint["estimated_best"]),
                float(checkpoint["estimated_challenger"]),
                float(checkpoint["estimated_regret"]),
                float(checkpoint["joint_estimated_regret"]),
            )
        except (KeyError, ValueError):
            continue
        if (
            all(math.isfinite(value) for value in estimates)
            and estimates[2] >= 0.0
            and estimates[3] >= 0.0
            and int(checkpoint.get("near_tie_challengers", "-1")) >= 0
        ):
            valid.append(checkpoint)
    if not valid:
        raise ValueError("checkpoint root has no post-floor finite telemetry")

    selected: list[dict[str, str]] = []
    for landmark in CHECKPOINT_NODE_LANDMARKS:
        candidate = next(
            (row for row in valid if int(row["nodes"]) >= landmark), None
        )
        if candidate is not None and candidate not in selected:
            selected.append(candidate)
    if valid[-1] not in selected:
        selected.append(valid[-1])
    return selected


def read_checkpoint_rows(
    log: Path,
    manifest: Path,
    panel_name: str,
    source_offset: int,
) -> list[Row]:
    """Read live-observable checkpoint landmarks with common-judge labels."""

    _reject_invalid_protocol(log)
    metadata = _metadata_by_position(manifest)
    positions: dict[int, dict[str, str]] = {}
    checkpoints: dict[int, list[dict[str, str]]] = defaultdict(list)
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION "):
                item = parse_fields(line)
                source = int(item["source_index"])
                if source in positions:
                    raise ValueError(f"duplicate checkpoint position {source}")
                positions[source] = item
            elif line.startswith("THINKING_CURVE_CHECKPOINT "):
                item = parse_fields(line)
                checkpoints[int(item["source_index"])].append(item)

    if set(positions) != set(checkpoints):
        raise ValueError("checkpoint panel roots and position metadata differ")
    rows: list[Row] = []
    for source, root_checkpoints in sorted(checkpoints.items()):
        position = positions[source]
        position_index = int(position.get("position", source))
        info = metadata.get(position_index)
        if info is None:
            raise ValueError(
                f"checkpoint position {position_index} lacks manifest row"
            )
        for checkpoint in _checkpoint_landmarks(root_checkpoints):
            best = float(checkpoint["estimated_best"])
            challenger = float(checkpoint["estimated_challenger"])
            actual = _finite_nonnegative(
                (checkpoint["judge_regret"],), "judge_regret"
            )
            rows.append(
                Row(
                    source_index=source_offset + source,
                    game=f"{panel_name}:{info['source_seed']}:{info['start']}",
                    policy=str(info["trajectory_policy"]),
                    bag=int(info["bag"]),
                    bag_band=str(info["bag_band"]),
                    width=int(checkpoint["arm_plays"]),
                    target_nodes=int(checkpoint["nodes"]),
                    static_cutoff_gap=float(position["static_primary_gap"]),
                    estimated_regret=float(checkpoint["estimated_regret"]),
                    joint_estimated_regret=float(
                        checkpoint["joint_estimated_regret"]
                    ),
                    bai_gap=max(0.0, best - challenger),
                    near_tie_challengers=int(
                        checkpoint["near_tie_challengers"]
                    ),
                    candidate_generation_regret=0.0,
                    within_set_bai_regret=actual,
                    total_current_turn_regret=actual,
                    stable_checkpoints=int(checkpoint["stable_checkpoints"]),
                    stable_iterations=int(checkpoint["stable_iterations"]),
                    selected_switches=int(checkpoint["selected_switches"]),
                )
            )
    if not rows:
        raise ValueError("checkpoint panel has no landmark rows")
    return rows


def _component_artifact(model: TwoPartComponentModel) -> dict[str, object]:
    return {
        "event_probability": _serialize_model(model.event_probability),
        "positive_severity": _serialize_model(model.positive_severity),
    }


def _coverage_summary(predictions: list[ConditionalPrediction]) -> dict[str, object]:
    if not predictions:
        raise ValueError("coverage summary needs predictions")
    covered = [
        item.row.total_current_turn_regret
        <= item.upper_total_current_turn_regret + 1.0e-15
        for item in predictions
    ]
    return {
        "rows": len(predictions),
        "games": len({item.row.game for item in predictions}),
        "coverage": statistics.fmean(covered),
        "mean_expected_regret": statistics.fmean(
            item.predicted_total_current_turn_regret for item in predictions
        ),
        "mean_upper_regret": statistics.fmean(
            item.upper_total_current_turn_regret for item in predictions
        ),
        "mean_actual_regret": statistics.fmean(
            item.row.total_current_turn_regret for item in predictions
        ),
    }


def _prediction_summary(
    predictions: list[ConditionalPrediction], deployment_width: int
) -> dict[str, object]:
    total = lambda item: item.predicted_total_current_turn_regret
    deployed = [item for item in predictions if item.row.width == deployment_width]
    if not deployed:
        raise ValueError("deployment width has no predictions")
    return {
        "all_widths": {
            "errors": error_metrics(
                predictions, total, lambda row: row.total_current_turn_regret
            ),
            "reliability": reliability(
                predictions, total, lambda row: row.total_current_turn_regret
            ),
        },
        "deployment_width": {
            "width": deployment_width,
            "errors": error_metrics(
                deployed, total, lambda row: row.total_current_turn_regret
            ),
            "reliability": reliability(
                deployed, total, lambda row: row.total_current_turn_regret
            ),
            "upper_bound": _coverage_summary(deployed),
        },
    }


def write_predictions(path: Path, predictions: list[ConditionalPrediction]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        fieldnames = [
            *asdict(predictions[0].row).keys(),
            "fold",
            "candidate_event_probability",
            "candidate_positive_severity",
            "predicted_candidate_generation_regret",
            "within_event_probability",
            "within_positive_severity",
            "predicted_within_set_bai_regret",
            "predicted_total_current_turn_regret",
            "raw_total_current_turn_regret",
            "point_calibration_scale",
            "upper_residual_addend",
            "upper_total_current_turn_regret",
            "calibration_level",
            "calibration_games",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for item in predictions:
            writer.writerow(
                {
                    **asdict(item.row),
                    "fold": item.fold,
                    "candidate_event_probability": item.candidate.event_probability,
                    "candidate_positive_severity": item.candidate.positive_severity,
                    "predicted_candidate_generation_regret": item.predicted_candidate_generation_regret,
                    "within_event_probability": item.within.event_probability,
                    "within_positive_severity": item.within.positive_severity,
                    "predicted_within_set_bai_regret": item.predicted_within_set_bai_regret,
                    "predicted_total_current_turn_regret": item.predicted_total_current_turn_regret,
                    "raw_total_current_turn_regret": item.raw_total_current_turn_regret,
                    "point_calibration_scale": item.point_calibration_scale,
                    "upper_residual_addend": item.upper_residual_addend,
                    "upper_total_current_turn_regret": item.upper_total_current_turn_regret,
                    "calibration_level": item.calibration_level,
                    "calibration_games": item.calibration_games,
                }
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument(
        "--combined-panel",
        nargs=3,
        action="append",
        metavar=("LOG", "MANIFEST", "NAME"),
        default=[],
        help="append a corrected cumulative panel",
    )
    parser.add_argument(
        "--checkpoint-panel",
        nargs=3,
        action="append",
        metavar=("LOG", "MANIFEST", "NAME"),
        default=[],
        help="append an audited cumulative checkpoint panel",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--predictions", required=True, type=Path)
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--inner-folds", type=int, default=4)
    parser.add_argument("--seed", type=int, default=82605)
    parser.add_argument("--event-prior-strength", type=float, default=16.0)
    parser.add_argument("--severity-prior-strength", type=float, default=8.0)
    parser.add_argument("--positive-epsilon", type=float, default=1.0e-15)
    parser.add_argument("--deployment-width", type=int, default=60)
    parser.add_argument("--upper-coverage", type=float, default=0.90)
    parser.add_argument("--minimum-calibration-games", type=int, default=16)
    parser.add_argument("--isotonic-prior-strength", type=float, default=2.0)
    args = parser.parse_args()

    _reject_invalid_protocol(args.log)
    rows = [replace(row, game=f"breadth:{row.game}") for row in read_rows(args.log, args.manifest)]
    source_offset = max(row.source_index for row in rows) + 1
    checkpoint_rows = 0
    checkpoint_games: set[str] = set()
    for raw_log, raw_manifest, panel_name in args.combined_panel:
        combined = read_combined_rows(
            Path(raw_log), Path(raw_manifest), panel_name, source_offset
        )
        rows.extend(combined)
        source_offset = max(row.source_index for row in rows) + 1
    for raw_log, raw_manifest, panel_name in args.checkpoint_panel:
        checkpoint = read_checkpoint_rows(
            Path(raw_log), Path(raw_manifest), panel_name, source_offset
        )
        rows.extend(checkpoint)
        checkpoint_rows += len(checkpoint)
        checkpoint_games.update(row.game for row in checkpoint)
        source_offset = max(row.source_index for row in rows) + 1

    predictions, calibration_cells, isotonic_blocks = nested_cross_fit_with_bounds(
        rows,
        args.folds,
        args.inner_folds,
        args.seed,
        args.event_prior_strength,
        args.severity_prior_strength,
        args.positive_epsilon,
        args.deployment_width,
        args.upper_coverage,
        args.minimum_calibration_games,
        args.isotonic_prior_strength,
    )
    candidate_model, within_model = fit_models(
        rows,
        args.event_prior_strength,
        args.severity_prior_strength,
        args.positive_epsilon,
    )
    artifact = {
        "artifact_kind": "cross_fitted_conditional_current_turn_regret_model",
        "schema_version": 1,
        "label_scope": "common_checkpoint_observable_top60_union",
        "beyond_top60_regret": "unmeasured",
        "rows": len(rows),
        "games": len({row.game for row in rows}),
        "widths": sorted({row.width for row in rows}),
        "checkpoint_landmarks": list(CHECKPOINT_NODE_LANDMARKS),
        "checkpoint_rows": checkpoint_rows,
        "checkpoint_games": len(checkpoint_games),
        "deployment_width": args.deployment_width,
        "event_prior_strength": args.event_prior_strength,
        "severity_prior_strength": args.severity_prior_strength,
        "positive_epsilon": args.positive_epsilon,
        "upper_coverage": args.upper_coverage,
        "minimum_calibration_games": args.minimum_calibration_games,
        "isotonic_prior_strength": args.isotonic_prior_strength,
        "candidate_model": _component_artifact(candidate_model),
        "within_set_model": _component_artifact(within_model),
        "calibration_cells": [asdict(cell) for cell in calibration_cells],
        "isotonic_blocks": [asdict(block) for block in isotonic_blocks],
        "cross_fitted": _prediction_summary(
            predictions, args.deployment_width
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(artifact, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_predictions(args.predictions, predictions)
    print(json.dumps(artifact["cross_fitted"], indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

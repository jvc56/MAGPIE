#!/usr/bin/env python3
"""Fit and replay a realizable same-arm horizon-value model.

Absolute regret against a deeper judge is not the value of buying more work
from the current SIM arm.  Once the arm's incumbent has settled, that label
can remain positive even though further same-ply work returns the same move.
This tool instead predicts

    P(choice at horizon != incumbent now)
      * E[judge_regret(now) - judge_regret(horizon) | choices differ].

The improvement is signed: continuing can replace a better judged incumbent
with a worse horizon move.  Judge values and the horizon identity are labels
only.  Models are fitted on fixed work landmarks with complete source games
as folds; full checkpoint traces are used only to replay a fitted held-out
score or an explicitly declared Rule-of-Zero baseline.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import asdict, dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import statistics
from typing import Callable

try:
    from tools.analyze_candidate_miss import parse_fields
    from tools.clustered_inference import student_t_critical
    from tools.fit_total_current_turn_regret import fold_for_game
except ModuleNotFoundError:  # Direct execution from tools/.
    from analyze_candidate_miss import parse_fields  # type: ignore[no-redef]
    from clustered_inference import student_t_critical  # type: ignore[no-redef]
    from fit_total_current_turn_regret import fold_for_game  # type: ignore[no-redef]


DEFAULT_LANDMARKS = (
    25_000,
    50_000,
    100_000,
    150_000,
    200_000,
    300_000,
    400_000,
    500_000,
    600_000,
    750_000,
    1_000_000,
)
WORK_FRACTION_BOUNDS = (0.02, 0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.01)
STABILITY_FRACTION_BOUNDS = (0.10, 0.25, 0.50, 0.75, 0.90, 1.01)
ESTIMATED_REGRET_BOUNDS = (
    0.0,
    1.0e-7,
    1.0e-6,
    1.0e-5,
    1.0e-4,
    1.0e-3,
    1.0e-2,
    1.0,
)
BAI_GAP_BOUNDS = (0.0, 1.0e-3, 2.5e-3, 5.0e-3, 1.0e-2, 2.0e-2, 1.0)


def bucket(value: float, bounds: tuple[float, ...]) -> int:
    for index, upper in enumerate(bounds):
        if value <= upper:
            return index
    return len(bounds) - 1


@dataclass(frozen=True)
class HorizonRow:
    source_index: int
    game: str
    policy: str
    bag: int
    bag_band: str
    width: int
    checkpoint: int
    iterations: int
    nodes: int
    horizon_iterations: int
    horizon_nodes: int
    landmark_nodes: int
    static_cutoff_gap: float
    estimated_regret: float
    joint_estimated_regret: float
    bai_gap: float
    near_tie_challengers: int
    stable_checkpoints: int
    stable_iterations: int
    selected_switches: int
    selected_rank: int
    horizon_selected_rank: int
    current_judge_regret: float
    horizon_judge_regret: float
    horizon_mismatch: int
    signed_horizon_improvement: float

    @property
    def work_fraction(self) -> float:
        return min(1.0, self.nodes / max(1, self.horizon_nodes))

    @property
    def work_band(self) -> int:
        return bucket(self.work_fraction, WORK_FRACTION_BOUNDS)

    @property
    def stability_fraction(self) -> float:
        return min(1.0, self.stable_iterations / max(1, self.iterations))

    @property
    def stability_band(self) -> int:
        return bucket(self.stability_fraction, STABILITY_FRACTION_BOUNDS)

    @property
    def near_tie_band(self) -> int:
        count = self.near_tie_challengers
        if count <= 1:
            return max(0, count)
        if count <= 3:
            return 2
        return 3

    @property
    def switch_band(self) -> int:
        if self.selected_switches <= 1:
            return self.selected_switches
        if self.selected_switches <= 3:
            return 2
        return 3

    @property
    def estimated_regret_band(self) -> int:
        return bucket(self.joint_estimated_regret, ESTIMATED_REGRET_BOUNDS)

    @property
    def bai_gap_band(self) -> int:
        return bucket(self.bai_gap, BAI_GAP_BOUNDS)


KeyFunction = Callable[[HorizonRow], tuple[object, ...]]

EVENT_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.work_band,),
    lambda row: (row.work_band, row.stability_band),
    lambda row: (row.work_band, row.stability_band, row.near_tie_band),
    lambda row: (
        row.work_band,
        row.stability_band,
        row.near_tie_band,
        row.switch_band,
    ),
    lambda row: (
        row.work_band,
        row.stability_band,
        row.near_tie_band,
        row.switch_band,
        row.bag_band,
    ),
    lambda row: (
        row.work_band,
        row.stability_band,
        row.near_tie_band,
        row.switch_band,
        row.bag_band,
        row.policy,
    ),
)

SEVERITY_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.work_band,),
    lambda row: (row.work_band, row.near_tie_band),
    lambda row: (row.work_band, row.near_tie_band, row.bag_band),
    lambda row: (
        row.work_band,
        row.near_tie_band,
        row.bag_band,
        row.policy,
    ),
)


@dataclass(frozen=True)
class MeanCell:
    mean: float
    observations: int


@dataclass(frozen=True)
class MeanModel:
    cells: dict[tuple[int, tuple[object, ...]], MeanCell]


@dataclass(frozen=True)
class HorizonModel:
    mismatch: MeanModel
    signed_improvement: MeanModel


@dataclass(frozen=True)
class HorizonPrediction:
    row: HorizonRow
    fold: int
    mismatch_probability: float
    conditional_signed_improvement: float

    @property
    def expected_signed_improvement(self) -> float:
        return self.mismatch_probability * self.conditional_signed_improvement

    @property
    def stopping_score(self) -> float:
        return max(0.0, self.expected_signed_improvement)


def _reject_invalid_protocol(path: Path) -> None:
    for parent in (path.parent, path.parent.parent):
        marker = parent / "INVALID_PROTOCOL.txt"
        if marker.exists():
            raise ValueError(f"invalid protocol marker present: {marker}")


def _manifest_rows(path: Path) -> dict[int, dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    rows = {int(item["position"]): item for item in document.get("mapping", [])}
    if not rows:
        raise ValueError("selection manifest has no root mapping")
    return rows


def _finite_nonnegative(value: str, field: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ValueError(f"{field} must be finite and nonnegative")
    return parsed


def _valid_checkpoint(fields: dict[str, str]) -> bool:
    try:
        estimates = (
            float(fields["estimated_best"]),
            float(fields["estimated_challenger"]),
            float(fields["estimated_regret"]),
            float(fields["joint_estimated_regret"]),
        )
        return (
            all(math.isfinite(value) for value in estimates)
            and estimates[2] >= 0.0
            and estimates[3] >= 0.0
            and int(fields.get("near_tie_challengers", "-1")) >= 0
        )
    except (KeyError, ValueError):
        return False


def _select_landmarks(
    rows: list[HorizonRow], landmarks: tuple[int, ...]
) -> list[HorizonRow]:
    result: list[HorizonRow] = []
    selected_checkpoints: set[int] = set()
    for landmark in landmarks:
        candidate = next((row for row in rows if row.nodes >= landmark), None)
        if candidate is None or candidate.checkpoint in selected_checkpoints:
            continue
        result.append(replace(candidate, landmark_nodes=landmark))
        selected_checkpoints.add(candidate.checkpoint)
    if not result:
        raise ValueError("root has no requested checkpoint landmark")
    return result


def read_horizon_panel(
    log: Path,
    manifest: Path,
    landmarks: tuple[int, ...] = DEFAULT_LANDMARKS,
) -> tuple[list[HorizonRow], list[HorizonRow]]:
    """Return fixed training landmarks and all finite replay checkpoints."""

    _reject_invalid_protocol(log)
    metadata = _manifest_rows(manifest)
    positions: dict[int, dict[str, str]] = {}
    checkpoints: dict[int, list[dict[str, str]]] = defaultdict(list)
    horizons: dict[int, dict[str, str]] = {}
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION "):
                fields = parse_fields(line)
                source = int(fields["source_index"])
                if source in positions:
                    raise ValueError(f"duplicate position row for source {source}")
                positions[source] = fields
            elif line.startswith("THINKING_CURVE_CHECKPOINT "):
                fields = parse_fields(line)
                checkpoints[int(fields["source_index"])].append(fields)
            elif line.startswith("THINKING_CURVE_POINT "):
                fields = parse_fields(line)
                if fields.get("final") != "0":
                    continue
                if fields.get("control_kind", "stopped") != "stopped":
                    continue
                source = int(fields["source_index"])
                if source in horizons:
                    raise ValueError(f"duplicate horizon point for source {source}")
                horizons[source] = fields

    if set(positions) != set(checkpoints) or set(positions) != set(horizons):
        raise ValueError("position, checkpoint, and horizon roots differ")
    landmark_rows: list[HorizonRow] = []
    all_rows: list[HorizonRow] = []
    for source in sorted(positions):
        position = positions[source]
        position_index = int(position.get("position", source))
        info = metadata.get(position_index)
        if info is None:
            raise ValueError(f"position {position_index} lacks manifest metadata")
        horizon = horizons[source]
        horizon_rank = int(horizon["selected_rank"])
        horizon_regret = _finite_nonnegative(
            horizon["judge_regret"], "horizon judge_regret"
        )
        horizon_nodes = int(horizon["nodes"])
        horizon_iterations = int(horizon["iterations"])
        root_rows: list[HorizonRow] = []
        for checkpoint in sorted(
            checkpoints[source], key=lambda item: int(item["checkpoint"])
        ):
            if not _valid_checkpoint(checkpoint):
                continue
            if int(checkpoint["full_selected_rank"]) != horizon_rank:
                raise ValueError("checkpoint horizon rank disagrees with final point")
            selected_rank = int(checkpoint["selected_rank"])
            current_regret = _finite_nonnegative(
                checkpoint["judge_regret"], "checkpoint judge_regret"
            )
            mismatch = int(selected_rank != horizon_rank)
            improvement = current_regret - horizon_regret
            if mismatch == 0 and abs(improvement) > 2.0e-9:
                raise ValueError("identical move has inconsistent common-judge value")
            best = float(checkpoint["estimated_best"])
            challenger = float(checkpoint["estimated_challenger"])
            row = HorizonRow(
                source_index=source,
                game=f"{info['source_seed']}:{info['start']}",
                policy=str(info["trajectory_policy"]),
                bag=int(info["bag"]),
                bag_band=str(info["bag_band"]),
                width=int(checkpoint["arm_plays"]),
                checkpoint=int(checkpoint["checkpoint"]),
                iterations=int(checkpoint["iterations"]),
                nodes=int(checkpoint["nodes"]),
                horizon_iterations=horizon_iterations,
                horizon_nodes=horizon_nodes,
                landmark_nodes=0,
                static_cutoff_gap=float(position["static_primary_gap"]),
                estimated_regret=float(checkpoint["estimated_regret"]),
                joint_estimated_regret=float(
                    checkpoint["joint_estimated_regret"]
                ),
                bai_gap=max(0.0, best - challenger),
                near_tie_challengers=int(
                    checkpoint["near_tie_challengers"]
                ),
                stable_checkpoints=int(checkpoint["stable_checkpoints"]),
                stable_iterations=int(checkpoint["stable_iterations"]),
                selected_switches=int(checkpoint["selected_switches"]),
                selected_rank=selected_rank,
                horizon_selected_rank=horizon_rank,
                current_judge_regret=current_regret,
                horizon_judge_regret=horizon_regret,
                horizon_mismatch=mismatch,
                signed_horizon_improvement=improvement,
            )
            root_rows.append(row)
        if not root_rows:
            raise ValueError(f"source {source} has no finite checkpoints")
        all_rows.extend(root_rows)
        landmark_rows.extend(_select_landmarks(root_rows, landmarks))
    return landmark_rows, all_rows


def fit_mean_model(
    rows: list[HorizonRow],
    levels: tuple[KeyFunction, ...],
    label: Callable[[HorizonRow], float],
    prior_strength: float,
) -> MeanModel:
    if not rows:
        raise ValueError("mean model needs rows")
    cells: dict[tuple[int, tuple[object, ...]], MeanCell] = {}
    for level, key_function in enumerate(levels):
        groups: dict[tuple[object, ...], list[HorizonRow]] = defaultdict(list)
        for row in rows:
            groups[key_function(row)].append(row)
        for key, group in groups.items():
            total = sum(label(row) for row in group)
            if level == 0:
                mean = total / len(group)
            else:
                parent_key = levels[level - 1](group[0])
                parent = cells[(level - 1, parent_key)]
                mean = (total + prior_strength * parent.mean) / (
                    len(group) + prior_strength
                )
            cells[(level, key)] = MeanCell(mean, len(group))
    return MeanModel(cells)


def predict_mean(
    model: MeanModel, row: HorizonRow, levels: tuple[KeyFunction, ...]
) -> float:
    for level in range(len(levels) - 1, -1, -1):
        cell = model.cells.get((level, levels[level](row)))
        if cell is not None:
            return cell.mean
    raise ValueError("mean model lacks a global cell")


def fit_model(
    rows: list[HorizonRow], event_prior: float, severity_prior: float
) -> HorizonModel:
    # Every fixed landmark is a deployment-time state. Keep a repeated
    # incumbent/horizon pair at each landmark so conditional severity is
    # learned for the work band where the prediction is made. Complete-game
    # folds, rather than row deduplication, prevent leakage.
    mismatch_rows = [row for row in rows if row.horizon_mismatch]
    if not mismatch_rows:
        raise ValueError("training fold has no horizon mismatches")
    return HorizonModel(
        mismatch=fit_mean_model(
            rows,
            EVENT_LEVELS,
            lambda row: float(row.horizon_mismatch),
            event_prior,
        ),
        signed_improvement=fit_mean_model(
            mismatch_rows,
            SEVERITY_LEVELS,
            lambda row: row.signed_horizon_improvement,
            severity_prior,
        ),
    )


def predict(model: HorizonModel, row: HorizonRow, fold: int) -> HorizonPrediction:
    probability = predict_mean(model.mismatch, row, EVENT_LEVELS)
    if probability < -1.0e-12 or probability > 1.0 + 1.0e-12:
        raise ValueError("predicted horizon-mismatch probability is invalid")
    return HorizonPrediction(
        row=row,
        fold=fold,
        mismatch_probability=min(1.0, max(0.0, probability)),
        conditional_signed_improvement=predict_mean(
            model.signed_improvement, row, SEVERITY_LEVELS
        ),
    )


def cross_fit(
    landmarks: list[HorizonRow],
    replay_rows: list[HorizonRow],
    folds: int,
    seed: int,
    event_prior: float,
    severity_prior: float,
) -> tuple[list[HorizonPrediction], list[HorizonPrediction]]:
    games = {row.game for row in landmarks}
    if folds < 2 or folds > len(games):
        raise ValueError("fold count must be between 2 and game count")
    assignment = {game: fold_for_game(game, folds, seed) for game in games}
    if len(set(assignment.values())) != folds:
        raise ValueError("one or more cross-fitting folds are empty")
    landmark_predictions: list[HorizonPrediction] = []
    replay_predictions: list[HorizonPrediction] = []
    for fold in range(folds):
        training = [row for row in landmarks if assignment[row.game] != fold]
        model = fit_model(training, event_prior, severity_prior)
        landmark_predictions.extend(
            predict(model, row, fold)
            for row in landmarks
            if assignment[row.game] == fold
        )
        replay_predictions.extend(
            predict(model, row, fold)
            for row in replay_rows
            if assignment[row.game] == fold
        )
    order = lambda item: (item.row.source_index, item.row.iterations)
    return sorted(landmark_predictions, key=order), sorted(
        replay_predictions, key=order
    )


def reliability_bins(
    predictions: list[HorizonPrediction],
    predicted: Callable[[HorizonPrediction], float],
    actual: Callable[[HorizonRow], float],
) -> dict[str, object]:
    observations = sorted((predicted(item), actual(item.row)) for item in predictions)
    denominator = sum(item[0] * item[0] for item in observations)
    slope = (
        sum(left * right for left, right in observations) / denominator
        if denominator > 0.0
        else None
    )
    bins: list[dict[str, object]] = []
    start = 0
    while start < len(observations) and len(bins) < 10:
        bins_left = 10 - len(bins)
        target = math.ceil((len(observations) - start) / bins_left)
        end = min(len(observations), start + target)
        while end < len(observations) and observations[end][0] == observations[end - 1][0]:
            end += 1
        group = observations[start:end]
        predicted_mean = statistics.fmean(item[0] for item in group)
        actual_mean = statistics.fmean(item[1] for item in group)
        ratio = actual_mean / predicted_mean if predicted_mean > 0.0 else None
        bins.append(
            {
                "rows": len(group),
                "predicted_mean": predicted_mean,
                "actual_mean": actual_mean,
                "actual_to_predicted": ratio,
                "underprediction_over_2x": (
                    (ratio is None and actual_mean > 0.0)
                    or (ratio is not None and ratio > 2.0)
                ),
            }
        )
        start = end
    return {
        "rows": len(observations),
        "slope_through_origin": slope,
        "underprediction_bins_over_2x": sum(
            bool(item["underprediction_over_2x"]) for item in bins
        ),
        "bins": bins,
    }


def _auc(predictions: list[HorizonPrediction]) -> float | None:
    ordered = sorted(
        (item.mismatch_probability, item.row.horizon_mismatch)
        for item in predictions
    )
    positives = sum(actual for _, actual in ordered)
    negatives = len(ordered) - positives
    if positives == 0 or negatives == 0:
        return None
    rank_sum = 0.0
    index = 0
    while index < len(ordered):
        stop = index + 1
        while stop < len(ordered) and ordered[stop][0] == ordered[index][0]:
            stop += 1
        average_rank = (index + 1 + stop) / 2.0
        rank_sum += average_rank * sum(actual for _, actual in ordered[index:stop])
        index = stop
    return (rank_sum - positives * (positives + 1) / 2.0) / (
        positives * negatives
    )


def prediction_summary(predictions: list[HorizonPrediction]) -> dict[str, object]:
    mismatch = reliability_bins(
        predictions,
        lambda item: item.mismatch_probability,
        lambda row: float(row.horizon_mismatch),
    )
    errors = [
        item.expected_signed_improvement - item.row.signed_horizon_improvement
        for item in predictions
    ]
    mismatch["brier"] = statistics.fmean(
        (item.mismatch_probability - item.row.horizon_mismatch) ** 2
        for item in predictions
    )
    mismatch["auc"] = _auc(predictions)
    mismatch["gate_pass"] = (
        mismatch["slope_through_origin"] is not None
        and 0.7 <= float(mismatch["slope_through_origin"]) <= 1.4
        and mismatch["underprediction_bins_over_2x"] == 0
    )
    return {
        "rows": len(predictions),
        "games": len({item.row.game for item in predictions}),
        "horizon_mismatch": mismatch,
        "signed_improvement": {
            "mean_actual": statistics.fmean(
                item.row.signed_horizon_improvement for item in predictions
            ),
            "mean_predicted": statistics.fmean(
                item.expected_signed_improvement for item in predictions
            ),
            "bias": statistics.fmean(errors),
            "mae": statistics.fmean(abs(error) for error in errors),
            "rmse": math.sqrt(statistics.fmean(error * error for error in errors)),
        },
    }


def target_summary(rows: list[HorizonRow]) -> dict[str, object]:
    def summarize(group: list[HorizonRow]) -> dict[str, object]:
        mismatches = [row for row in group if row.horizon_mismatch]
        return {
            "rows": len(group),
            "games": len({row.game for row in group}),
            "horizon_mismatches": len(mismatches),
            "horizon_mismatch_rate": len(mismatches) / len(group),
            "positive_same_arm_improvements": sum(
                row.signed_horizon_improvement > 1.0e-12 for row in mismatches
            ),
            "harmful_same_arm_changes": sum(
                row.signed_horizon_improvement < -1.0e-12 for row in mismatches
            ),
            "mean_signed_improvement": statistics.fmean(
                row.signed_horizon_improvement for row in group
            ),
            "mean_signed_improvement_given_mismatch": (
                statistics.fmean(
                    row.signed_horizon_improvement for row in mismatches
                )
                if mismatches
                else 0.0
            ),
            "mean_current_judge_regret": statistics.fmean(
                row.current_judge_regret for row in group
            ),
            "mean_horizon_judge_regret": statistics.fmean(
                row.horizon_judge_regret for row in group
            ),
        }

    by_landmark: dict[str, object] = {}
    for landmark in sorted({row.landmark_nodes for row in rows}):
        by_landmark[str(landmark)] = summarize(
            [row for row in rows if row.landmark_nodes == landmark]
        )
    return {"overall": summarize(rows), "by_landmark_nodes": by_landmark}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _mean_interval(values: list[float]) -> dict[str, object]:
    if not values:
        raise ValueError("mean interval needs values")
    mean = statistics.fmean(values)
    if len(values) == 1:
        radius = 0.0
    else:
        radius = student_t_critical(0.95, len(values) - 1) * (
            statistics.stdev(values) / math.sqrt(len(values))
        )
    return {"mean": mean, "ci95": [mean - radius, mean + radius]}


def _zero_event_upper(events: int, trials: int) -> float | None:
    if trials <= 0 or events != 0:
        return None
    return 1.0 - 0.05 ** (1.0 / trials)


def _stop_summary(selected: list[HorizonRow], rule: dict[str, object]) -> dict[str, object]:
    savings = [1.0 - row.nodes / row.horizon_nodes for row in selected]
    added = [row.signed_horizon_improvement for row in selected]
    mismatches = sum(row.horizon_mismatch for row in selected)
    saving_summary = _mean_interval(savings)
    added_summary = _mean_interval(added)
    return {
        "rule": rule,
        "games": len(selected),
        "stopped_before_horizon": sum(row.nodes < row.horizon_nodes for row in selected),
        "choice_mismatches": mismatches,
        "zero_mismatch_one_sided_95_upper": _zero_event_upper(
            mismatches, len(selected)
        ),
        "node_savings": saving_summary,
        "added_judge_regret": added_summary,
        "offline_gate_pass": (
            saving_summary["mean"] >= 0.25
            and added_summary["ci95"][1] <= 0.001
        ),
        "exploratory_only": True,
    }


def _fallback_horizon(row: HorizonRow) -> HorizonRow:
    return replace(
        row,
        checkpoint=row.checkpoint + 1,
        iterations=row.horizon_iterations,
        nodes=row.horizon_nodes,
        landmark_nodes=0,
        selected_rank=row.horizon_selected_rank,
        current_judge_regret=row.horizon_judge_regret,
        horizon_mismatch=0,
        signed_horizon_improvement=0.0,
    )


def replay_rule_of_zero(
    rows: list[HorizonRow], floors: tuple[int, ...], stability: tuple[int, ...]
) -> list[dict[str, object]]:
    by_game: dict[str, list[HorizonRow]] = defaultdict(list)
    for row in rows:
        by_game[row.game].append(row)
    results: list[dict[str, object]] = []
    for floor in floors:
        for minimum_stable in stability:
            selected: list[HorizonRow] = []
            for game_rows in by_game.values():
                ordered = sorted(game_rows, key=lambda row: row.iterations)
                choice = next(
                    (
                        row
                        for row in ordered
                        if row.nodes >= floor
                        and row.stable_checkpoints >= minimum_stable
                        and row.near_tie_challengers == 0
                    ),
                    None,
                )
                selected.append(choice or _fallback_horizon(ordered[-1]))
            results.append(
                _stop_summary(
                    selected,
                    {
                        "kind": "rule_of_zero",
                        "minimum_nodes": floor,
                        "minimum_stable_checkpoints": minimum_stable,
                        "near_tie_challengers": 0,
                    },
                )
            )
    return results


def replay_model(
    predictions: list[HorizonPrediction],
    thresholds: tuple[float, ...],
    minimum_nodes: int,
    minimum_stable: int,
) -> list[dict[str, object]]:
    by_game: dict[str, list[HorizonPrediction]] = defaultdict(list)
    for item in predictions:
        by_game[item.row.game].append(item)
    results: list[dict[str, object]] = []
    for threshold in thresholds:
        selected: list[HorizonRow] = []
        for game_rows in by_game.values():
            ordered = sorted(game_rows, key=lambda item: item.row.iterations)
            choice = next(
                (
                    item.row
                    for item in ordered
                    if item.row.nodes >= minimum_nodes
                    and item.row.stable_checkpoints >= minimum_stable
                    and item.stopping_score <= threshold
                ),
                None,
            )
            selected.append(choice or _fallback_horizon(ordered[-1].row))
        results.append(
            _stop_summary(
                selected,
                {
                    "kind": "cross_fitted_horizon_value",
                    "maximum_expected_signed_improvement": threshold,
                    "minimum_nodes": minimum_nodes,
                    "minimum_stable_checkpoints": minimum_stable,
                },
            )
        )
    return results


def _serialize(model: MeanModel) -> list[dict[str, object]]:
    return [
        {
            "level": level,
            "key": list(key),
            "mean": cell.mean,
            "observations": cell.observations,
        }
        for (level, key), cell in sorted(
            model.cells.items(), key=lambda item: (item[0][0], repr(item[0][1]))
        )
    ]


def write_predictions(path: Path, predictions: list[HorizonPrediction]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        fieldnames = [
            *asdict(predictions[0].row).keys(),
            "fold",
            "work_fraction",
            "stability_fraction",
            "mismatch_probability",
            "conditional_signed_improvement",
            "expected_signed_improvement",
            "stopping_score",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for item in predictions:
            writer.writerow(
                {
                    **asdict(item.row),
                    "fold": item.fold,
                    "work_fraction": item.row.work_fraction,
                    "stability_fraction": item.row.stability_fraction,
                    "mismatch_probability": item.mismatch_probability,
                    "conditional_signed_improvement": item.conditional_signed_improvement,
                    "expected_signed_improvement": item.expected_signed_improvement,
                    "stopping_score": item.stopping_score,
                }
            )


def _parse_ints(raw: str) -> tuple[int, ...]:
    values = tuple(int(item) for item in raw.split(",") if item)
    if not values or any(value < 0 for value in values):
        raise ValueError("integer grid must contain nonnegative values")
    return values


def _parse_floats(raw: str) -> tuple[float, ...]:
    values = tuple(float(item) for item in raw.split(",") if item)
    if not values or any(value < 0.0 or not math.isfinite(value) for value in values):
        raise ValueError("float grid must contain finite nonnegative values")
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--predictions", type=Path, required=True)
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--seed", type=int, default=82631)
    parser.add_argument("--event-prior", type=float, default=12.0)
    parser.add_argument("--severity-prior", type=float, default=8.0)
    parser.add_argument(
        "--landmarks",
        default=",".join(str(value) for value in DEFAULT_LANDMARKS),
    )
    parser.add_argument("--rule-zero-floors", default="50000,100000,200000,300000")
    parser.add_argument("--rule-zero-stability", default="2,8,32")
    parser.add_argument(
        "--model-thresholds",
        default="0,0.00001,0.000025,0.00005,0.0001,0.00025,0.0005,0.001",
    )
    parser.add_argument("--minimum-model-nodes", type=int, default=100_000)
    parser.add_argument("--minimum-model-stability", type=int, default=2)
    args = parser.parse_args()
    if args.event_prior < 0.0 or args.severity_prior < 0.0:
        parser.error("prior strengths must be nonnegative")

    landmarks = _parse_ints(args.landmarks)
    landmark_rows, all_rows = read_horizon_panel(
        args.log, args.manifest, landmarks
    )
    landmark_predictions, replay_predictions = cross_fit(
        landmark_rows,
        all_rows,
        args.folds,
        args.seed,
        args.event_prior,
        args.severity_prior,
    )
    final_model = fit_model(landmark_rows, args.event_prior, args.severity_prior)
    summary = prediction_summary(landmark_predictions)
    result = {
        "artifact_kind": "cross_fitted_same_arm_horizon_differential_model",
        "schema_version": 1,
        "target": (
            "P(horizon choice differs from current incumbent) times "
            "conditional signed common-judge improvement"
        ),
        "label_scope": "checkpoint_observable_sorted_top60_common_judge_union",
        "horizon": "same_ply_full_arm",
        "beyond_top60_regret": "unmeasured",
        "judge_values": "labels_only",
        "full_selected_rank": "label_only_never_a_feature",
        "live_allocation": "disabled",
        "rows": len(landmark_rows),
        "all_checkpoint_rows": len(all_rows),
        "games": len({row.game for row in landmark_rows}),
        "landmarks": list(landmarks),
        "folds": args.folds,
        "event_prior": args.event_prior,
        "severity_prior": args.severity_prior,
        "inputs": {
            "log": str(args.log),
            "log_sha256": _sha256(args.log),
            "manifest": str(args.manifest),
            "manifest_sha256": _sha256(args.manifest),
        },
        "target_summary": target_summary(landmark_rows),
        "mismatch_model": _serialize(final_model.mismatch),
        "signed_improvement_model": _serialize(final_model.signed_improvement),
        "cross_fitted": summary,
        "rule_of_zero_replay": replay_rule_of_zero(
            all_rows,
            _parse_ints(args.rule_zero_floors),
            _parse_ints(args.rule_zero_stability),
        ),
        "model_replay": replay_model(
            replay_predictions,
            _parse_floats(args.model_thresholds),
            args.minimum_model_nodes,
            args.minimum_model_stability,
        ),
        "decision": (
            "development_only_no_fresh_panel_until_rule_and_threshold_are "
            "frozen_before_new_outcomes"
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_predictions(args.predictions, landmark_predictions)
    print(json.dumps({
        "cross_fitted": result["cross_fitted"],
        "rule_of_zero_replay": result["rule_of_zero_replay"],
        "model_replay": result["model_replay"],
        "decision": result["decision"],
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

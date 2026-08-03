#!/usr/bin/env python3
"""Cross-fit candidate-generation plus within-set SIM regret.

The common risk-set judge defines an exact additive label on every nested
candidate arm:

    global judged best - selected
      = (global judged best - best judged inside top K)
      + (best judged inside top K - selected).

The first term prices candidate generation/screening.  The second term prices
the BAI decision conditional on that admitted set.  Oracle judge values are
labels only; predictions use root state, static cutoff gaps, and live BAI
telemetry.  Folds are assigned by source game.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import asdict, dataclass
import hashlib
import json
import math
from pathlib import Path
from typing import Callable

try:
    from tools.analyze_candidate_breadth import decompose_regret, load_roots
    from tools.analyze_candidate_miss import parse_fields
except ModuleNotFoundError:  # Direct execution from tools/.
    from analyze_candidate_breadth import decompose_regret, load_roots
    from analyze_candidate_miss import parse_fields


STATIC_GAP_UPPER_BOUNDS = (1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 1.0e9)
ESTIMATED_REGRET_UPPER_BOUNDS = (
    0.0,
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
BAI_GAP_UPPER_BOUNDS = (
    0.0,
    1.0e-3,
    2.5e-3,
    5.0e-3,
    1.0e-2,
    2.0e-2,
    4.0e-2,
    1.0,
)


def bucket(value: float, bounds: tuple[float, ...]) -> int:
    for index, upper in enumerate(bounds):
        if value <= upper:
            return index
    return len(bounds) - 1


@dataclass(frozen=True)
class Row:
    source_index: int
    game: str
    policy: str
    bag: int
    bag_band: str
    width: int
    target_nodes: int
    static_cutoff_gap: float
    estimated_regret: float
    joint_estimated_regret: float
    bai_gap: float
    near_tie_challengers: int
    candidate_generation_regret: float
    within_set_bai_regret: float
    total_current_turn_regret: float

    @property
    def static_gap_band(self) -> int:
        return bucket(self.static_cutoff_gap, STATIC_GAP_UPPER_BOUNDS)

    @property
    def estimated_regret_band(self) -> int:
        return bucket(
            self.joint_estimated_regret, ESTIMATED_REGRET_UPPER_BOUNDS
        )

    @property
    def bai_gap_band(self) -> int:
        return bucket(self.bai_gap, BAI_GAP_UPPER_BOUNDS)

    @property
    def near_tie_band(self) -> int:
        return max(0, min(self.near_tie_challengers, 5))


@dataclass(frozen=True)
class Cell:
    mean: float
    observations: int


@dataclass(frozen=True)
class ComponentModel:
    cells: dict[tuple[int, tuple[object, ...]], Cell]


@dataclass(frozen=True)
class Prediction:
    row: Row
    fold: int
    predicted_candidate_generation_regret: float
    predicted_within_set_bai_regret: float

    @property
    def predicted_total_current_turn_regret(self) -> float:
        return (
            self.predicted_candidate_generation_regret
            + self.predicted_within_set_bai_regret
        )


def _finite_nonnegative(raw: str, field: str) -> float:
    value = float(raw)
    if not math.isfinite(value) or value < 0.0:
        raise ValueError(f"{field} must be finite and nonnegative")
    return value


def _metadata(manifest: Path) -> dict[int, dict[str, object]]:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    mapping = {
        int(item["position"]): item for item in document.get("mapping", [])
    }
    if not mapping:
        raise ValueError("selection manifest has no root mapping")
    return mapping


def read_rows(log: Path, manifest: Path) -> list[Row]:
    roots = {root.source_index: root for root in load_roots(log)}
    metadata = _metadata(manifest)
    position_rows: dict[int, dict[str, str]] = {}
    point_rows: dict[tuple[int, int], dict[str, str]] = {}
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION "):
                fields = parse_fields(line)
                source = int(fields["source_index"])
                if source in position_rows:
                    raise ValueError(f"duplicate position row for source {source}")
                position_rows[source] = fields
            elif line.startswith("THINKING_CURVE_POINT "):
                fields = parse_fields(line)
                if fields.get("final") != "0":
                    continue
                kind = fields.get("control_kind", "stopped")
                if kind not in {"stopped", "candidate_limit"}:
                    continue
                key = (int(fields["source_index"]), int(fields["arm_plays"]))
                if key in point_rows:
                    raise ValueError(f"duplicate point row {key}")
                point_rows[key] = fields

    result: list[Row] = []
    for source, root in sorted(roots.items()):
        if source not in metadata or source not in position_rows:
            raise ValueError(f"source {source} lacks metadata")
        info = metadata[source]
        position = position_rows[source]
        widths = tuple(sorted(root.points))
        gap_values = tuple(
            float(value)
            for value in position["candidate_control_static_gaps"].split(",")
        )
        if len(gap_values) != len(widths) - 1:
            raise ValueError(f"source {source} static gaps do not join widths")
        gaps = dict(zip(widths[:-1], gap_values))
        gaps[widths[-1]] = float(position["static_primary_gap"])
        for width in widths:
            fields = point_rows.get((source, width))
            if fields is None:
                raise ValueError(f"source {source} lacks top-{width} point")
            best = float(fields["estimated_best"])
            challenger = float(fields["estimated_challenger"])
            bai_gap = max(0.0, best - challenger) if math.isfinite(challenger) else 1.0
            components = decompose_regret(root, width)
            total = _finite_nonnegative(fields["judge_regret"], "judge_regret")
            if abs(total - components.total) > 2.0e-9:
                raise ValueError("point and decomposed total regret differ")
            result.append(
                Row(
                    source_index=source,
                    game=f"{info['source_seed']}:{info['start']}",
                    policy=str(info["trajectory_policy"]),
                    bag=int(info["bag"]),
                    bag_band=str(info["bag_band"]),
                    width=width,
                    target_nodes=int(fields["target_nodes"]),
                    static_cutoff_gap=gaps[width],
                    estimated_regret=_finite_nonnegative(
                        fields["estimated_regret"], "estimated_regret"
                    ),
                    joint_estimated_regret=_finite_nonnegative(
                        fields["joint_estimated_regret"],
                        "joint_estimated_regret",
                    ),
                    bai_gap=bai_gap,
                    near_tie_challengers=int(fields["near_tie_challengers"]),
                    candidate_generation_regret=components.candidate_generation,
                    within_set_bai_regret=components.within_set_bai,
                    total_current_turn_regret=components.total,
                )
            )
    if not result:
        raise ValueError("candidate breadth log has no model rows")
    return result


KeyFunction = Callable[[Row], tuple[object, ...]]


CANDIDATE_LEVELS: tuple[KeyFunction, ...] = (
    lambda row: (),
    lambda row: (row.width,),
    lambda row: (row.width, row.static_gap_band),
    lambda row: (row.width, row.static_gap_band, row.bag_band),
    lambda row: (row.width, row.static_gap_band, row.bag_band, row.policy),
)


WITHIN_LEVELS: tuple[KeyFunction, ...] = (
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
        row.bai_gap_band,
    ),
    lambda row: (
        row.width,
        row.estimated_regret_band,
        row.near_tie_band,
        row.bai_gap_band,
        row.bag_band,
    ),
)


def fit_component_model(
    rows: list[Row],
    levels: tuple[KeyFunction, ...],
    label: Callable[[Row], float],
    prior_strength: float,
) -> ComponentModel:
    if not rows:
        raise ValueError("component model needs training rows")
    cells: dict[tuple[int, tuple[object, ...]], Cell] = {}
    for level_index, key_function in enumerate(levels):
        grouped: dict[tuple[object, ...], list[Row]] = defaultdict(list)
        for row in rows:
            grouped[key_function(row)].append(row)
        for key, group in grouped.items():
            total = sum(label(row) for row in group)
            if level_index == 0:
                mean = total / len(group)
            else:
                exemplar = group[0]
                parent_key = levels[level_index - 1](exemplar)
                parent = cells[(level_index - 1, parent_key)]
                mean = (total + prior_strength * parent.mean) / (
                    len(group) + prior_strength
                )
            cells[(level_index, key)] = Cell(max(0.0, mean), len(group))
    return ComponentModel(cells)


def predict_component(
    model: ComponentModel, row: Row, levels: tuple[KeyFunction, ...]
) -> float:
    for level_index in range(len(levels) - 1, -1, -1):
        cell = model.cells.get((level_index, levels[level_index](row)))
        if cell is not None:
            return cell.mean
    raise ValueError("component model lacks a global cell")


def fold_for_game(game: str, folds: int, seed: int) -> int:
    digest = hashlib.sha256(f"{seed}\0{game}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big") % folds


def cross_fit(
    rows: list[Row], folds: int, seed: int, prior_strength: float
) -> list[Prediction]:
    games = {row.game for row in rows}
    if folds < 2 or folds > len(games):
        raise ValueError("fold count must be between 2 and number of games")
    assignments = {game: fold_for_game(game, folds, seed) for game in games}
    if len(set(assignments.values())) != folds:
        raise ValueError("one or more cross-fitting folds are empty")
    predictions: list[Prediction] = []
    for fold in range(folds):
        training = [row for row in rows if assignments[row.game] != fold]
        held_out = [row for row in rows if assignments[row.game] == fold]
        candidate_model = fit_component_model(
            training,
            CANDIDATE_LEVELS,
            lambda row: row.candidate_generation_regret,
            prior_strength,
        )
        within_model = fit_component_model(
            training,
            WITHIN_LEVELS,
            lambda row: row.within_set_bai_regret,
            prior_strength,
        )
        for row in held_out:
            predictions.append(
                Prediction(
                    row=row,
                    fold=fold,
                    predicted_candidate_generation_regret=predict_component(
                        candidate_model, row, CANDIDATE_LEVELS
                    ),
                    predicted_within_set_bai_regret=predict_component(
                        within_model, row, WITHIN_LEVELS
                    ),
                )
            )
    return sorted(predictions, key=lambda item: (item.row.source_index, item.row.width))


def reliability(
    predictions: list[Prediction],
    prediction: Callable[[Prediction], float],
    label: Callable[[Row], float],
) -> dict[str, object]:
    observations = sorted((prediction(item), label(item.row)) for item in predictions)
    denominator = sum(predicted * predicted for predicted, _ in observations)
    slope = (
        sum(predicted * actual for predicted, actual in observations) / denominator
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
        predicted_mean = sum(value[0] for value in group) / len(group)
        actual_mean = sum(value[1] for value in group) / len(group)
        bins.append(
            {
                "rows": len(group),
                "predicted_mean": predicted_mean,
                "actual_mean": actual_mean,
                "actual_to_predicted": (
                    actual_mean / predicted_mean if predicted_mean > 0.0 else None
                ),
                "infinite_underprediction": predicted_mean == 0.0
                and actual_mean > 0.0,
            }
        )
        start = end
    return {"rows": len(observations), "slope_through_origin": slope, "bins": bins}


def error_metrics(
    predictions: list[Prediction],
    prediction: Callable[[Prediction], float],
    label: Callable[[Row], float],
) -> dict[str, float | int]:
    errors = [prediction(item) - label(item.row) for item in predictions]
    return {
        "rows": len(errors),
        "games": len({item.row.game for item in predictions}),
        "bias": sum(errors) / len(errors),
        "mae": sum(abs(error) for error in errors) / len(errors),
        "rmse": math.sqrt(sum(error * error for error in errors) / len(errors)),
    }


def _serialize_model(model: ComponentModel) -> list[dict[str, object]]:
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


def write_runtime_model(
    path: Path, candidate_model: ComponentModel, within_model: ComponentModel
) -> None:
    """Write the deliberately small, audit-friendly C harness format.

    Missing coordinates are ``*`` and therefore act as hierarchical
    backoffs.  The runtime reader always chooses the highest matching level,
    reproducing :func:`predict_component` without needing a JSON parser in the
    solver benchmark.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        stream.write("SIM_TOTAL_REGRET_MODEL\t1\n")
        for (level, key), cell in sorted(
            candidate_model.cells.items(),
            key=lambda item: (item[0][0], repr(item[0][1])),
        ):
            coordinates = ["*", "*", "*", "*"]
            for index, value in enumerate(key):
                coordinates[index] = str(value)
            stream.write(
                "\t".join(
                    [
                        "C",
                        str(level),
                        *coordinates,
                        f"{cell.mean:.17g}",
                        str(cell.observations),
                    ]
                )
                + "\n"
            )
        for (level, key), cell in sorted(
            within_model.cells.items(),
            key=lambda item: (item[0][0], repr(item[0][1])),
        ):
            coordinates = ["*", "*", "*", "*", "*"]
            for index, value in enumerate(key):
                coordinates[index] = str(value)
            stream.write(
                "\t".join(
                    [
                        "W",
                        str(level),
                        *coordinates,
                        f"{cell.mean:.17g}",
                        str(cell.observations),
                    ]
                )
                + "\n"
            )


def fit_artifact(
    rows: list[Row], predictions: list[Prediction], prior_strength: float
) -> dict[str, object]:
    candidate_model = fit_component_model(
        rows,
        CANDIDATE_LEVELS,
        lambda row: row.candidate_generation_regret,
        prior_strength,
    )
    within_model = fit_component_model(
        rows,
        WITHIN_LEVELS,
        lambda row: row.within_set_bai_regret,
        prior_strength,
    )
    total_prediction = lambda item: item.predicted_total_current_turn_regret
    return {
        "artifact_kind": "cross_fitted_total_current_turn_regret_model",
        "label_scope": "common_checkpoint_observable_risk_set",
        "rows": len(rows),
        "games": len({row.game for row in rows}),
        "widths": sorted({row.width for row in rows}),
        "prior_strength": prior_strength,
        "candidate_model": _serialize_model(candidate_model),
        "within_set_model": _serialize_model(within_model),
        "cross_fitted": {
            "candidate_generation": error_metrics(
                predictions,
                lambda item: item.predicted_candidate_generation_regret,
                lambda row: row.candidate_generation_regret,
            ),
            "within_set_bai": error_metrics(
                predictions,
                lambda item: item.predicted_within_set_bai_regret,
                lambda row: row.within_set_bai_regret,
            ),
            "total": error_metrics(
                predictions, total_prediction, lambda row: row.total_current_turn_regret
            ),
            "total_reliability": reliability(
                predictions, total_prediction, lambda row: row.total_current_turn_regret
            ),
        },
    }


def write_predictions(path: Path, predictions: list[Prediction]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        fieldnames = [
            *asdict(predictions[0].row).keys(),
            "fold",
            "predicted_candidate_generation_regret",
            "predicted_within_set_bai_regret",
            "predicted_total_current_turn_regret",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for item in predictions:
            writer.writerow(
                {
                    **asdict(item.row),
                    "fold": item.fold,
                    "predicted_candidate_generation_regret": item.predicted_candidate_generation_regret,
                    "predicted_within_set_bai_regret": item.predicted_within_set_bai_regret,
                    "predicted_total_current_turn_regret": item.predicted_total_current_turn_regret,
                }
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--predictions", required=True, type=Path)
    parser.add_argument(
        "--runtime-model",
        type=Path,
        help="optional compact lookup table for cumulative replay",
    )
    parser.add_argument("--folds", type=int, default=5)
    parser.add_argument("--seed", type=int, default=82604)
    parser.add_argument("--prior-strength", type=float, default=8.0)
    args = parser.parse_args()
    rows = read_rows(args.log, args.manifest)
    predictions = cross_fit(rows, args.folds, args.seed, args.prior_strength)
    artifact = fit_artifact(rows, predictions, args.prior_strength)
    if args.runtime_model is not None:
        candidate_model = fit_component_model(
            rows,
            CANDIDATE_LEVELS,
            lambda row: row.candidate_generation_regret,
            args.prior_strength,
        )
        within_model = fit_component_model(
            rows,
            WITHIN_LEVELS,
            lambda row: row.within_set_bai_regret,
            args.prior_strength,
        )
        write_runtime_model(args.runtime_model, candidate_model, within_model)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(artifact, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_predictions(args.predictions, predictions)
    print(json.dumps(artifact["cross_fitted"], indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

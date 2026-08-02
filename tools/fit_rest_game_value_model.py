#!/usr/bin/env python3
"""Fit an auditable monotone rest-of-game value-to-go model.

Input comes from ``build_rest_game_value_labels.py``. Splits are by complete
source game. The first model is intentionally a small hierarchical table:

    global -> bag phase -> (bag phase, future turns)
           -> (bag phase, future turns, score-spread band)

Every cell is a monotone expected-future-regret curve over remaining clock.
Sparse cells shrink toward their parent. Runtime inference is therefore a few
bucket lookups plus linear interpolation, and the exact finite difference
``F(B - dt) - F(B)`` can price a proposed search chunk.

This fitter never marks an artifact live. Predictive calibration is necessary
but insufficient: a separate held-out allocation backtest must show lower
realized decision regret than equal slicing before the C artifact's held-out
gate may be enabled.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import random
from typing import Iterable


ARTIFACT_VERSION = 2
BAG_UPPER_BOUNDS = (0, 4, 15, 35, 60, 100)
TURN_UPPER_BOUNDS = (0, 1, 2, 3, 5, 8, 64)
SPREAD_UPPER_BOUNDS = (-100, -30, 30, 100, 10000)
COMBINED_RACK_UPPER_BOUNDS = (2, 4, 6, 8, 10, 12, 14)
REQUIRED = {
    "game",
    "bag",
    "spread",
    "budget",
    "forecast_valid",
    "future_turns",
    "predicted_future_turns",
    "oracle_future_regret",
}


@dataclass(frozen=True)
class Label:
    game: str
    budget: float
    regret: float
    bag: int
    spread: int
    predicted_future_turns: int
    rate_profile: int = 0
    own_rack: int = 7
    opp_rack: int = 7
    planning_regret_valid: bool = False
    allocation_policy: str = "legacy_unknown"

    @property
    def phase(self) -> int:
        return bucket(self.bag, BAG_UPPER_BOUNDS)

    @property
    def turn_band(self) -> int:
        return bucket(self.predicted_future_turns, TURN_UPPER_BOUNDS)

    @property
    def spread_band(self) -> int:
        return bucket(self.spread, SPREAD_UPPER_BOUNDS)

    @property
    def combined_rack_band(self) -> int:
        return bucket(
            self.own_rack + self.opp_rack, COMBINED_RACK_UPPER_BOUNDS
        )


@dataclass(frozen=True)
class Curve:
    level: str
    key: tuple[int, ...]
    regrets: tuple[float, ...]
    observations: int


@dataclass(frozen=True)
class Model:
    budgets: tuple[float, ...]
    curves: dict[tuple[str, tuple[int, ...]], Curve]


def bucket(value: int, upper_bounds: tuple[int, ...]) -> int:
    for index, upper in enumerate(upper_bounds):
        if value <= upper:
            return index
    return len(upper_bounds) - 1


def read_labels(path: Path) -> list[Label]:
    labels: list[Label] = []
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        fields = set(reader.fieldnames or [])
        missing = REQUIRED - fields
        if missing:
            raise ValueError(f"label CSV missing fields: {','.join(sorted(missing))}")
        for row in reader:
            if int(row["forecast_valid"]) == 0:
                continue
            label = Label(
                game=row["game"],
                budget=float(row["budget"]),
                regret=float(row["oracle_future_regret"]),
                bag=int(row["bag"]),
                spread=int(row["spread"]),
                predicted_future_turns=int(row["predicted_future_turns"]),
                rate_profile=int(row.get("rate_profile", "0")),
                own_rack=int(row.get("own_rack", "7")),
                opp_rack=int(row.get("opp_rack", "7")),
                planning_regret_valid=bool(
                    int(row.get("planning_regret_valid", "0"))
                ),
                allocation_policy=row.get(
                    "allocation_policy", "legacy_unknown"
                ),
            )
            if (
                not math.isfinite(label.budget)
                or label.budget < 0.0
                or not math.isfinite(label.regret)
                or label.regret < 0.0
            ):
                raise ValueError("budgets and regret must be finite and nonnegative")
            labels.append(label)
    if not labels:
        raise ValueError("label CSV has no valid forecasts")
    return labels


def split_games(
    games: Iterable[str], seed: int
) -> tuple[set[str], set[str], set[str]]:
    unique = sorted(set(games))
    if len(unique) < 3:
        raise ValueError("at least three complete source games are required")
    random.Random(seed).shuffle(unique)
    test_count = max(1, round(0.15 * len(unique)))
    calibration_count = max(1, round(0.15 * len(unique)))
    if test_count + calibration_count >= len(unique):
        test_count = 1
        calibration_count = 1
    test = set(unique[:test_count])
    calibration = set(unique[test_count : test_count + calibration_count])
    train = set(unique[test_count + calibration_count :])
    return train, calibration, test


def pava_nonincreasing(values: list[float], weights: list[float]) -> list[float]:
    if len(values) != len(weights) or not values:
        raise ValueError("PAVA requires equal nonempty value and weight arrays")
    blocks: list[list[float | int]] = []
    for index, (value, weight) in enumerate(zip(values, weights)):
        if not math.isfinite(value) or not math.isfinite(weight) or weight <= 0.0:
            raise ValueError("PAVA values must be finite and weights positive")
        blocks.append([index, index, value * weight, weight])
        while len(blocks) >= 2:
            previous = blocks[-2]
            current = blocks[-1]
            previous_mean = float(previous[2]) / float(previous[3])
            current_mean = float(current[2]) / float(current[3])
            if previous_mean >= current_mean:
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
    for start, end, weighted_sum, weight in blocks:
        mean = float(weighted_sum) / float(weight)
        for index in range(int(start), int(end) + 1):
            fitted[index] = mean
    return fitted


def _curve_key(label: Label, level: str) -> tuple[int, ...]:
    if level == "global":
        return (label.rate_profile,)
    if level == "phase":
        return (label.rate_profile, label.phase)
    if level == "phase_turn":
        return (label.rate_profile, label.phase, label.turn_band)
    if level == "phase_turn_spread":
        return (
            label.rate_profile,
            label.phase,
            label.turn_band,
            label.spread_band,
        )
    if level == "phase_turn_spread_rack":
        return (
            label.rate_profile,
            label.phase,
            label.turn_band,
            label.spread_band,
            label.combined_rack_band,
        )
    raise ValueError(f"unknown curve level {level}")


def fit_model(labels: list[Label], prior_strength: float = 20.0) -> Model:
    if not math.isfinite(prior_strength) or prior_strength < 0.0:
        raise ValueError("prior strength must be finite and nonnegative")
    budgets = tuple(sorted({label.budget for label in labels}))
    budget_index = {budget: index for index, budget in enumerate(budgets)}
    levels = (
        "global",
        "phase",
        "phase_turn",
        "phase_turn_spread",
        "phase_turn_spread_rack",
    )
    curves: dict[tuple[str, tuple[int, ...]], Curve] = {}
    parent_level = {
        "phase": "global",
        "phase_turn": "phase",
        "phase_turn_spread": "phase_turn",
        "phase_turn_spread_rack": "phase_turn_spread",
    }

    for level in levels:
        grouped: dict[tuple[int, ...], list[Label]] = {}
        for label in labels:
            grouped.setdefault(_curve_key(label, level), []).append(label)
        for key, group in grouped.items():
            sums = [0.0] * len(budgets)
            counts = [0] * len(budgets)
            for label in group:
                index = budget_index[label.budget]
                sums[index] += label.regret
                counts[index] += 1

            raw: list[float] = []
            weights: list[float] = []
            parent: Curve | None = None
            if level != "global":
                parent_name = parent_level[level]
                parent_key = key[:-1]
                parent = curves[(parent_name, parent_key)]
            for index in range(len(budgets)):
                count = counts[index]
                if parent is None:
                    if count == 0:
                        # A global budget can be absent when mandatory suffix
                        # work makes all labels invalid. Interpolate it after
                        # fitting from its nearest observed neighbors.
                        raw.append(math.nan)
                        weights.append(0.0)
                    else:
                        raw.append(sums[index] / count)
                        weights.append(float(count))
                else:
                    raw.append(
                        (sums[index] + prior_strength * parent.regrets[index])
                        / (count + prior_strength)
                    )
                    weights.append(count + prior_strength)

            if parent is None and any(weight == 0.0 for weight in weights):
                observed = [index for index, weight in enumerate(weights) if weight > 0]
                if not observed:
                    raise ValueError("global curve has no observed budgets")
                for index, weight in enumerate(weights):
                    if weight > 0:
                        continue
                    nearest = min(observed, key=lambda other: abs(other - index))
                    raw[index] = raw[nearest]
                    weights[index] = 1.0e-9
            fitted = pava_nonincreasing(raw, weights)
            curves[(level, key)] = Curve(
                level=level,
                key=key,
                regrets=tuple(fitted),
                observations=len(group),
            )
    return Model(budgets=budgets, curves=curves)


def select_curve(model: Model, label: Label) -> Curve:
    candidates = (
        (
            "phase_turn_spread_rack",
            _curve_key(label, "phase_turn_spread_rack"),
        ),
        ("phase_turn_spread", _curve_key(label, "phase_turn_spread")),
        ("phase_turn", _curve_key(label, "phase_turn")),
        ("phase", _curve_key(label, "phase")),
        ("global", _curve_key(label, "global")),
    )
    for identity in candidates:
        curve = model.curves.get(identity)
        if curve is not None:
            return curve
    raise AssertionError("fitted model lacks rate-profile global curve")


def interpolate(budgets: tuple[float, ...], values: tuple[float, ...], x: float) -> float:
    if x <= budgets[0]:
        return values[0]
    if x >= budgets[-1]:
        return values[-1]
    for upper in range(1, len(budgets)):
        if x <= budgets[upper]:
            lower = upper - 1
            fraction = (x - budgets[lower]) / (budgets[upper] - budgets[lower])
            return values[lower] + fraction * (values[upper] - values[lower])
    raise AssertionError("interpolation interval not found")


def predict(model: Model, label: Label) -> float:
    curve = select_curve(model, label)
    return interpolate(model.budgets, curve.regrets, label.budget)


def metrics(model: Model, labels: list[Label]) -> dict[str, float | int]:
    if not labels:
        return {"rows": 0, "games": 0, "mae": math.nan, "rmse": math.nan, "bias": math.nan}
    errors = [predict(model, label) - label.regret for label in labels]
    return {
        "rows": len(labels),
        "games": len({label.game for label in labels}),
        "mae": sum(abs(error) for error in errors) / len(errors),
        "rmse": math.sqrt(sum(error * error for error in errors) / len(errors)),
        "bias": sum(errors) / len(errors),
    }


def _curve_json(curve: Curve) -> dict[str, object]:
    return {
        "level": curve.level,
        "key": list(curve.key),
        "observations": curve.observations,
        "expected_future_regret": list(curve.regrets),
    }


def artifact(
    model: Model,
    source: Path,
    seed: int,
    prior_strength: float,
    train_games: set[str],
    calibration_games: set[str],
    test_games: set[str],
    calibration_metrics: dict[str, float | int],
    test_metrics: dict[str, float | int],
    planning_regret_valid: bool,
    allocation_policy: str,
) -> dict[str, object]:
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    return {
        "artifact_version": ARTIFACT_VERSION,
        "artifact_kind": "rest_game_value_to_go_shadow",
        "source_sha256": digest,
        "split_seed": seed,
        "live_enabled": False,
        "surrogate_allocation_gate_passed": False,
        "terminal_game_gate_passed": False,
        "gate_reason": "allocation and terminal game gates not yet supplied",
        "label_scope": (
            "realized_trajectory_equal_slice_policy_evaluation"
            if planning_regret_valid
            else "non_live_prophet_or_contaminated_labels"
        ),
        "planning_regret_valid": planning_regret_valid,
        "allocation_policy": allocation_policy,
        "feature_contract": {
            "future_turns": "predicted_from_current_state_only",
            "spread": "player_on_turn_point_of_view",
            "split_unit": "complete_source_game",
        },
        "budget_knots": list(model.budgets),
        "bag_upper_bounds": list(BAG_UPPER_BOUNDS),
        "future_turn_upper_bounds": list(TURN_UPPER_BOUNDS),
        "spread_upper_bounds": list(SPREAD_UPPER_BOUNDS),
        "combined_rack_upper_bounds": list(COMBINED_RACK_UPPER_BOUNDS),
        "rate_profiles": sorted(
            {curve.key[0] for curve in model.curves.values()}
        ),
        "prior_strength": prior_strength,
        "split_games": {
            "train": sorted(train_games),
            "calibration": sorted(calibration_games),
            "test": sorted(test_games),
        },
        "calibration_metrics": calibration_metrics,
        "test_metrics": test_metrics,
        "curves": [
            _curve_json(curve)
            for _, curve in sorted(model.curves.items(), key=lambda item: item[0])
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("labels", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--split-seed", type=int, default=633)
    parser.add_argument("--prior-strength", type=float, default=20.0)
    args = parser.parse_args()

    labels = read_labels(args.labels)
    train_games, calibration_games, test_games = split_games(
        (label.game for label in labels), args.split_seed
    )
    train = [label for label in labels if label.game in train_games]
    calibration = [label for label in labels if label.game in calibration_games]
    test = [label for label in labels if label.game in test_games]
    model = fit_model(train, args.prior_strength)
    allocation_policies = {label.allocation_policy for label in labels}
    allocation_policy = (
        next(iter(allocation_policies))
        if len(allocation_policies) == 1
        else "mixed"
    )
    planning_regret_valid = (
        allocation_policy == "equal_slice_policy"
        and all(label.planning_regret_valid for label in labels)
    )
    document = artifact(
        model,
        args.labels,
        args.split_seed,
        args.prior_strength,
        train_games,
        calibration_games,
        test_games,
        metrics(model, calibration),
        metrics(model, test),
        planning_regret_valid,
        allocation_policy,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    print(
        f"train_games={len(train_games)} calibration_games={len(calibration_games)} "
        f"test_games={len(test_games)} curves={len(model.curves)} "
        f"live_enabled=0 output={args.output}"
    )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Back-test a nonanticipating dual TimeManager policy.

For each fixed shadow price lambda, the training policy independently chooses
the work boundary minimizing

    expected current-turn regret + lambda * cost.

The model fits expected *future spend* of that fixed policy from current-state
features.  At a held-out turn, it selects the smallest lambda whose current
choice plus predicted future spend fits the remaining clock, then replans on
the next turn.  Expectation is therefore taken before lambda is selected: no
held-out realized suffix is optimized with hindsight.

Held-out oracle regret scores frozen choices only.  This remains an offline
realized-trajectory surrogate and never enables live allocation by itself.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
import json
import math
from pathlib import Path
from typing import Iterable

try:
    from tools.backtest_rest_game_value_policy import (
        ReplayResult,
        _feature_int,
        absolute_options,
        choose_equal,
        clustered_summary,
        fit_cuped_theta,
        oracle_suffix_regret,
        replay_heldout as replay_value_heldout,
        required_games_for_effect,
    )
    from tools.build_rest_game_value_labels import TurnCurve, read_turn_curves
    from tools.fit_rest_game_value_model import (
        BAG_UPPER_BOUNDS,
        COMBINED_RACK_UPPER_BOUNDS,
        SPREAD_UPPER_BOUNDS,
        TURN_UPPER_BOUNDS,
        bucket,
        fit_model as fit_value_model,
        read_labels,
        split_games,
    )
except ModuleNotFoundError:
    from backtest_rest_game_value_policy import (  # type: ignore[no-redef]
        ReplayResult,
        _feature_int,
        absolute_options,
        choose_equal,
        clustered_summary,
        fit_cuped_theta,
        oracle_suffix_regret,
        replay_heldout as replay_value_heldout,
        required_games_for_effect,
    )
    from build_rest_game_value_labels import (  # type: ignore[no-redef]
        TurnCurve,
        read_turn_curves,
    )
    from fit_rest_game_value_model import (  # type: ignore[no-redef]
        BAG_UPPER_BOUNDS,
        COMBINED_RACK_UPPER_BOUNDS,
        SPREAD_UPPER_BOUNDS,
        TURN_UPPER_BOUNDS,
        bucket,
        fit_model as fit_value_model,
        read_labels,
        split_games,
    )


LEVELS = (
    "global",
    "phase",
    "phase_turn",
    "phase_turn_spread",
    "phase_turn_spread_rack",
)


@dataclass(frozen=True)
class DemandLabel:
    curve: TurnCurve
    future_costs: tuple[float, ...]


@dataclass(frozen=True)
class DemandCell:
    future_costs: tuple[float, ...]
    observations: int


@dataclass(frozen=True)
class DemandModel:
    lambdas: tuple[float, ...]
    cells: dict[tuple[str, tuple[int, ...]], DemandCell]
    training_games: int


def _curve_key(curve: TurnCurve, level: str) -> tuple[int, ...]:
    rate_profile = curve.rate_profile
    phase = bucket(_feature_int(curve, "bag", 0), BAG_UPPER_BOUNDS)
    turn_band = bucket(
        _feature_int(curve, "predicted_future_turns", 0),
        TURN_UPPER_BOUNDS,
    )
    spread_band = bucket(
        _feature_int(curve, "spread", 0), SPREAD_UPPER_BOUNDS
    )
    combined_rack_band = bucket(
        _feature_int(curve, "own_rack", 7)
        + _feature_int(curve, "opp_rack", 7),
        COMBINED_RACK_UPPER_BOUNDS,
    )
    if level == "global":
        return (rate_profile,)
    if level == "phase":
        return (rate_profile, phase)
    if level == "phase_turn":
        return (rate_profile, phase, turn_band)
    if level == "phase_turn_spread":
        return (rate_profile, phase, turn_band, spread_band)
    if level == "phase_turn_spread_rack":
        return (
            rate_profile,
            phase,
            turn_band,
            spread_band,
            combined_rack_band,
        )
    raise ValueError(f"unknown demand level {level}")


def _select_option(
    curve: TurnCurve,
    shadow_price: float,
    cost_quantum: float,
    maximum_cost: float = math.inf,
) -> tuple[float, float, float] | None:
    fitting = [
        option
        for option in absolute_options(curve, cost_quantum)
        if option[0] <= maximum_cost + 1.0e-12
    ]
    if not fitting:
        return None
    return min(
        fitting,
        key=lambda option: (
            option[1] + shadow_price * option[0],
            option[0],
        ),
    )


def build_lambda_grid(
    curves: Iterable[TurnCurve], cost_quantum: float, points: int = 65
) -> tuple[float, ...]:
    if points < 3:
        raise ValueError("lambda grid needs at least three points")
    maximum_breakpoint = 0.0
    for curve in curves:
        options = absolute_options(curve, cost_quantum)
        for cheaper_index, cheaper in enumerate(options):
            for expensive in options[cheaper_index + 1 :]:
                cost_difference = expensive[0] - cheaper[0]
                regret_difference = cheaper[1] - expensive[1]
                if cost_difference > 0.0 and regret_difference > 0.0:
                    maximum_breakpoint = max(
                        maximum_breakpoint,
                        regret_difference / cost_difference,
                    )
    upper = max(1.0e-9, 2.0 * maximum_breakpoint)
    positive_points = points - 1
    return (
        0.0,
        *(
            upper * 10.0 ** (-6.0 + 6.0 * index / (positive_points - 1))
            for index in range(positive_points)
        ),
    )


def _build_demand_labels(
    curves: Iterable[TurnCurve],
    lambdas: tuple[float, ...],
    cost_quantum: float,
) -> list[DemandLabel]:
    sequences: dict[tuple[str, int, int], list[TurnCurve]] = defaultdict(list)
    for curve in curves:
        sequences[(curve.game, curve.rate_profile, curve.player)].append(curve)
    labels: list[DemandLabel] = []
    for sequence in sequences.values():
        sequence.sort(key=lambda curve: curve.turn)
        future_costs = [0.0] * len(lambdas)
        for curve in reversed(sequence):
            # The current curve is the observable state.  Its label is spend
            # by later turns under a precommitted fixed-lambda policy.
            labels.append(DemandLabel(curve, tuple(future_costs)))
            for lambda_index, shadow_price in enumerate(lambdas):
                choice = _select_option(curve, shadow_price, cost_quantum)
                if choice is None:
                    raise ValueError("training curve has no legal option")
                future_costs[lambda_index] += choice[0]
    return labels


def fit_demand_model(
    curves: list[TurnCurve],
    cost_quantum: float,
    prior_strength: float = 20.0,
    lambda_points: int = 65,
) -> DemandModel:
    if not curves or not math.isfinite(prior_strength) or prior_strength < 0.0:
        raise ValueError("invalid demand-model training data")
    if not all(curve.has_separate_expected_regret for curve in curves):
        raise ValueError("water filling requires separate planning regret")
    lambdas = build_lambda_grid(curves, cost_quantum, lambda_points)
    labels = _build_demand_labels(curves, lambdas, cost_quantum)
    cells: dict[tuple[str, tuple[int, ...]], DemandCell] = {}
    for level_index, level in enumerate(LEVELS):
        grouped: dict[tuple[int, ...], list[DemandLabel]] = defaultdict(list)
        for label in labels:
            grouped[_curve_key(label.curve, level)].append(label)
        for key, group in grouped.items():
            observations = len(group)
            values = [
                sum(label.future_costs[index] for label in group) / observations
                for index in range(len(lambdas))
            ]
            if level_index > 0:
                parent = cells[(LEVELS[level_index - 1], key[:-1])]
                values = [
                    (observations * value + prior_strength * parent_value)
                    / (observations + prior_strength)
                    for value, parent_value in zip(values, parent.future_costs)
                ]
            # A fixed-lambda policy's spend is nonincreasing in lambda.  The
            # averages preserve this mathematically; clamp only roundoff.
            for index in range(1, len(values)):
                values[index] = min(values[index - 1], values[index])
            cells[(level, key)] = DemandCell(tuple(values), observations)
    return DemandModel(
        lambdas=lambdas,
        cells=cells,
        training_games=len({curve.game for curve in curves}),
    )


def predict_future_cost(
    model: DemandModel, curve: TurnCurve, lambda_index: int
) -> float:
    for level in reversed(LEVELS):
        cell = model.cells.get((level, _curve_key(curve, level)))
        if cell is not None:
            return cell.future_costs[lambda_index]
    raise ValueError("demand model has no matching rate profile")


def choose_water_filling(
    model: DemandModel,
    curve: TurnCurve,
    remaining_budget: float,
    cost_quantum: float,
) -> tuple[float, float, float, float, float] | None:
    if remaining_budget < 0.0:
        return None
    if _feature_int(curve, "predicted_future_turns", 0) == 0:
        choice = _select_option(
            curve, 0.0, cost_quantum, maximum_cost=remaining_budget
        )
        if choice is None:
            return None
        return (*choice, 0.0, choice[0])

    for lambda_index, shadow_price in enumerate(model.lambdas):
        choice = _select_option(
            curve,
            shadow_price,
            cost_quantum,
            maximum_cost=remaining_budget,
        )
        if choice is None:
            continue
        expected_total = choice[0] + predict_future_cost(
            model, curve, lambda_index
        )
        if expected_total <= remaining_budget + 1.0e-12:
            return (*choice, shadow_price, expected_total)

    # Even the maximum shadow price predicts mandatory future work beyond the
    # clock.  Return the cheapest legal current boundary and expose the
    # oversubscription in expected_total; later replanning remains fail-safe.
    choice = min(
        (
            option
            for option in absolute_options(curve, cost_quantum)
            if option[0] <= remaining_budget + 1.0e-12
        ),
        key=lambda option: option[0],
        default=None,
    )
    if choice is None:
        return None
    lambda_index = len(model.lambdas) - 1
    return (
        *choice,
        model.lambdas[lambda_index],
        choice[0] + predict_future_cost(model, curve, lambda_index),
    )


def replay_sequence(
    model: DemandModel,
    curves: list[TurnCurve],
    starting_budget: float,
    cost_quantum: float,
) -> ReplayResult | None:
    learned_budget = starting_budget
    equal_budget = starting_budget
    learned_regret = 0.0
    equal_regret = 0.0
    learned_expected_regret = 0.0
    equal_expected_regret = 0.0
    divergent_turns = 0
    for curve in curves:
        turns_remaining = max(
            1, _feature_int(curve, "predicted_future_turns", 0) + 1
        )
        learned = choose_water_filling(
            model, curve, learned_budget, cost_quantum
        )
        equal = choose_equal(
            curve, equal_budget, turns_remaining, cost_quantum
        )
        if learned is None or equal is None:
            return None
        if not math.isclose(learned[0], equal[0], rel_tol=0.0, abs_tol=1.0e-12):
            divergent_turns += 1
        learned_budget -= learned[0]
        equal_budget -= equal[0]
        learned_expected_regret += learned[1]
        equal_expected_regret += equal[1]
        learned_regret += learned[2]
        equal_regret += equal[2]

    oracle_regret = oracle_suffix_regret(curves, starting_budget, cost_quantum)
    if not math.isfinite(oracle_regret):
        return None
    first = curves[0]
    return ReplayResult(
        game=first.game,
        rate_profile=first.rate_profile,
        player=first.player,
        starting_budget=starting_budget,
        learned_regret=learned_regret,
        equal_regret=equal_regret,
        learned_expected_regret=learned_expected_regret,
        equal_expected_regret=equal_expected_regret,
        oracle_regret=oracle_regret,
        learned_spent=starting_budget - learned_budget,
        equal_spent=starting_budget - equal_budget,
        turns=len(curves),
        divergent_turns=divergent_turns,
        honest_choice=all(
            curve.has_separate_expected_regret for curve in curves
        ),
    )


def replay_heldout(
    model: DemandModel,
    curves: Iterable[TurnCurve],
    heldout_games: set[str],
    budgets: Iterable[float],
    cost_quantum: float,
) -> list[ReplayResult]:
    grouped: dict[tuple[str, int, int], list[TurnCurve]] = defaultdict(list)
    for curve in curves:
        if curve.game in heldout_games:
            grouped[(curve.game, curve.rate_profile, curve.player)].append(curve)
    results: list[ReplayResult] = []
    for sequence in grouped.values():
        sequence.sort(key=lambda curve: curve.turn)
        for budget in budgets:
            result = replay_sequence(model, sequence, budget, cost_quantum)
            if result is not None:
                results.append(result)
    return results


def _result_key(result: ReplayResult) -> tuple[str, int, int, float]:
    return (
        result.game,
        result.rate_profile,
        result.player,
        result.starting_budget,
    )


def compare_policies(
    water_results: list[ReplayResult], value_results: list[ReplayResult]
) -> list[ReplayResult]:
    values = {_result_key(result): result for result in value_results}
    comparisons: list[ReplayResult] = []
    for water in water_results:
        value = values.get(_result_key(water))
        if value is None:
            continue
        if not math.isclose(
            water.oracle_regret,
            value.oracle_regret,
            rel_tol=0.0,
            abs_tol=1.0e-12,
        ):
            raise ValueError("paired policies disagree on oracle baseline")
        comparisons.append(
            ReplayResult(
                game=water.game,
                rate_profile=water.rate_profile,
                player=water.player,
                starting_budget=water.starting_budget,
                learned_regret=water.learned_regret,
                equal_regret=value.learned_regret,
                learned_expected_regret=water.learned_expected_regret,
                equal_expected_regret=value.learned_expected_regret,
                oracle_regret=water.oracle_regret,
                learned_spent=water.learned_spent,
                equal_spent=value.learned_spent,
                turns=water.turns,
                # Aggregate outputs do not retain exact cross-policy turn
                # joins.  Report the all-replay paired comparison only.
                divergent_turns=0,
                honest_choice=water.honest_choice and value.honest_choice,
            )
        )
    return comparisons


def stratified_summaries(
    results: list[ReplayResult], attribute: str
) -> dict[str, dict[str, object]]:
    strata: dict[object, list[ReplayResult]] = defaultdict(list)
    for result in results:
        strata[getattr(result, attribute)].append(result)
    return {
        str(value): clustered_summary(group, True)
        for value, group in sorted(strata.items(), key=lambda item: item[0])
    }


def _comma_floats(value: str) -> list[float]:
    values = [float(item) for item in value.split(",") if item]
    if not values:
        raise argparse.ArgumentTypeError("expected at least one budget")
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", type=Path)
    parser.add_argument("--budgets", required=True, type=_comma_floats)
    parser.add_argument("--cost-quantum", required=True, type=float)
    parser.add_argument("--split-seed", type=int, default=633)
    parser.add_argument("--prior-strength", type=float, default=20.0)
    parser.add_argument("--lambda-points", type=int, default=65)
    parser.add_argument(
        "--value-labels",
        type=Path,
        help="optional equal-policy value labels for a direct one-step comparison",
    )
    parser.add_argument("--value-prior-strength", type=float, default=20.0)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    with args.curves.open(newline="", encoding="utf-8") as stream:
        curves = read_turn_curves(stream, args.cost_quantum)
    train_games, calibration_games, test_games = split_games(
        (curve.game for curve in curves), args.split_seed
    )
    model = fit_demand_model(
        [curve for curve in curves if curve.game in train_games],
        args.cost_quantum,
        args.prior_strength,
        args.lambda_points,
    )
    calibration_results = replay_heldout(
        model,
        curves,
        calibration_games,
        args.budgets,
        args.cost_quantum,
    )
    test_results = replay_heldout(
        model, curves, test_games, args.budgets, args.cost_quantum
    )
    calibration = clustered_summary(calibration_results, True)
    test = clustered_summary(test_results, True)
    cuped_theta = fit_cuped_theta(calibration_results, divergent_only=True)
    calibration_divergent = clustered_summary(
        calibration_results, True, divergent_only=True
    )
    test_divergent = clustered_summary(
        test_results,
        True,
        divergent_only=True,
        cuped_theta=cuped_theta,
    )
    calibration_sd = calibration_divergent["per_game_sd"]
    power_analysis = {}
    if isinstance(calibration_sd, float):
        power_analysis = {
            f"{effect:.3f}": required_games_for_effect(
                calibration_sd, effect
            )
            for effect in (0.002, 0.005)
        }
    value_comparison: dict[str, object] | None = None
    if args.value_labels is not None:
        labels = read_labels(args.value_labels)
        value_model = fit_value_model(
            [label for label in labels if label.game in train_games],
            args.value_prior_strength,
        )
        value_calibration_results = replay_value_heldout(
            value_model,
            curves,
            calibration_games,
            args.budgets,
            args.cost_quantum,
        )
        value_test_results = replay_value_heldout(
            value_model,
            curves,
            test_games,
            args.budgets,
            args.cost_quantum,
        )
        value_comparison = {
            "estimand": "water_filling_minus_one_step_equal_policy_value",
            "calibration": clustered_summary(
                compare_policies(
                    calibration_results, value_calibration_results
                ),
                True,
            ),
            "test": clustered_summary(
                compare_policies(test_results, value_test_results), True
            ),
        }
    document = {
        "artifact_kind": "rest_game_water_filling_backtest",
        "policy": "fixed_lambda_expected_future_demand",
        "split_unit": "complete_source_game",
        "split_seed": args.split_seed,
        "training_games": model.training_games,
        "lambda_points": len(model.lambdas),
        "lambda_range": [model.lambdas[0], model.lambdas[-1]],
        "demand_cells": len(model.cells),
        "calibration": calibration,
        "test": test,
        "calibration_divergent": calibration_divergent,
        "test_divergent_cuped": test_divergent,
        "test_by_starting_budget": stratified_summaries(
            test_results, "starting_budget"
        ),
        "test_by_rate_profile": stratified_summaries(
            test_results, "rate_profile"
        ),
        "cuped_theta": cuped_theta,
        "required_games_by_effect": power_analysis,
        "water_filling_vs_value_policy": value_comparison,
        "surrogate_gate_passed": False,
        "terminal_game_gate_passed": False,
        "live_enabled": False,
        "gate_reason": "research comparator; powered gates not supplied",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print(
        f"train_games={model.training_games} "
        f"calibration_games={calibration['source_games']} "
        f"test_games={test['source_games']} "
        f"test_delta={test['mean_learned_minus_equal']} "
        "surrogate_gate=0 terminal_gate=0 live_enabled=0 "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()

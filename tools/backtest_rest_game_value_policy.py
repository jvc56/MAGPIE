#!/usr/bin/env python3
"""Back-test a learned rest-of-game clock allocator on held-out games.

This is a decision test, not another regression metric.  At each realized
turn the learned policy chooses one legal completed-work boundary by minimizing

    current cross-fitted expected regret + predicted regret of the later-turn
    suffix

under its remaining clock. Equal slicing chooses the deepest boundary fitting
an equal share of that same clock. Only after those choices are frozen does
the replay read their held-out oracle regret. Inputs without a separate
``expected_regret`` column retain the legacy oracle-choice behavior and are
reported as such; they cannot pass the honest-choice gate. Both policies then
advance along the same held-out source trajectory, so the comparison isolates
allocation quality but does not model positions changed by a different move.
It is therefore a necessary surrogate gate, never the terminal live-policy
gate.

Inference and confidence intervals are clustered by complete source game.
Rate profiles, player trajectories, and starting budgets within a source game
are repeated measurements rather than independent samples.
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
    from tools.build_rest_game_value_labels import TurnCurve, read_turn_curves
    from tools.clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
    from tools.fit_rest_game_value_model import (
        Label,
        Model,
        fit_model,
        predict,
        read_labels,
        split_games,
    )
except ModuleNotFoundError:
    from build_rest_game_value_labels import TurnCurve, read_turn_curves
    from clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
    from fit_rest_game_value_model import (
        Label,
        Model,
        fit_model,
        predict,
        read_labels,
        split_games,
    )


@dataclass(frozen=True)
class ReplayResult:
    game: str
    rate_profile: int
    player: int
    starting_budget: float
    learned_regret: float
    equal_regret: float
    learned_expected_regret: float
    equal_expected_regret: float
    oracle_regret: float
    learned_spent: float
    equal_spent: float
    honest_choice: bool

    @property
    def learned_minus_equal(self) -> float:
        return self.learned_regret - self.equal_regret

    @property
    def learned_excess_oracle(self) -> float:
        return self.learned_regret - self.oracle_regret

    @property
    def equal_excess_oracle(self) -> float:
        return self.equal_regret - self.oracle_regret


def _feature_int(curve: TurnCurve, key: str, default: int) -> int:
    raw = curve.features.get(key, "")
    return int(raw) if raw != "" else default


def probe_for(curve: TurnCurve, budget: float) -> Label:
    if "predicted_future_turns" not in curve.features:
        raise ValueError(
            f"curve game={curve.game} turn={curve.turn} lacks "
            "predicted_future_turns"
        )
    return Label(
        game=curve.game,
        budget=budget,
        regret=0.0,
        bag=_feature_int(curve, "bag", 0),
        spread=_feature_int(curve, "spread", 0),
        predicted_future_turns=_feature_int(
            curve, "predicted_future_turns", 0
        ),
        rate_profile=curve.rate_profile,
        own_rack=_feature_int(curve, "own_rack", 7),
        opp_rack=_feature_int(curve, "opp_rack", 7),
    )


def absolute_options(
    curve: TurnCurve, cost_quantum: float, *, include_dominated: bool = False
) -> list[tuple[float, float, float]]:
    options = curve.all_options if include_dominated else curve.options
    return [
        (
            curve.mandatory_cost + option.cost_units * cost_quantum,
            option.regret,
            option.score_regret,
        )
        for option in options
    ]


def _best_fitting_option(
    options: list[tuple[float, float, float]], limit: float
) -> tuple[float, float, float] | None:
    fitting = [option for option in options if option[0] <= limit + 1.0e-12]
    if not fitting:
        return None
    # Equal slicing chooses its work budget before seeing a search result. It
    # therefore buys the deepest completed boundary that fits, not the
    # held-out oracle-best affordable boundary.
    return max(fitting, key=lambda option: option[0])


def choose_learned(
    model: Model,
    curve: TurnCurve,
    remaining_budget: float,
    is_last_turn: bool,
    cost_quantum: float,
) -> tuple[float, float, float] | None:
    choices: list[tuple[float, float, float, float]] = []
    for cost, expected_regret, actual_regret in absolute_options(
        curve, cost_quantum
    ):
        future_budget = remaining_budget - cost
        if future_budget < -1.0e-12:
            continue
        future_regret = (
            0.0
            if is_last_turn
            else predict(model, probe_for(curve, max(0.0, future_budget)))
        )
        if not math.isfinite(future_regret) or future_regret < 0.0:
            continue
        choices.append(
            (
                expected_regret + future_regret,
                cost,
                expected_regret,
                actual_regret,
            )
        )
    if not choices:
        return None
    _, cost, expected_regret, actual_regret = min(
        choices, key=lambda choice: (choice[0], choice[1])
    )
    return cost, expected_regret, actual_regret


def choose_equal(
    curve: TurnCurve,
    remaining_budget: float,
    turns_remaining: int,
    cost_quantum: float,
) -> tuple[float, float, float] | None:
    options = absolute_options(curve, cost_quantum, include_dominated=True)
    choice = _best_fitting_option(
        options, remaining_budget / float(turns_remaining)
    )
    if choice is not None:
        return choice
    # Equal slicing still has to return a legal move. If its nominal slice is
    # below mandatory work, buy the cheapest boundary that fits the full clock
    # and let later turns absorb the shortfall.
    fitting = [option for option in options if option[0] <= remaining_budget + 1e-12]
    return min(fitting, key=lambda option: option[0]) if fitting else None


def oracle_suffix_regret(
    curves: list[TurnCurve], budget: float, cost_quantum: float
) -> float:
    mandatory = sum(curve.mandatory_cost for curve in curves)
    if mandatory > budget + 1.0e-12:
        return math.nan
    maximum_units = int(
        math.floor((budget - mandatory) / cost_quantum + 1.0e-12)
    )
    following = [0.0] * (maximum_units + 1)
    for curve in reversed(curves):
        current = [math.inf] * (maximum_units + 1)
        for capacity in range(maximum_units + 1):
            for option in (curve.all_options or curve.options):
                if option.cost_units > capacity:
                    break
                current[capacity] = min(
                    current[capacity],
                    option.score_regret
                    + following[capacity - option.cost_units],
                )
        following = current
    return following[maximum_units]


def replay_sequence(
    model: Model,
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
    for curve in curves:
        # Both policies must use the same state-derived forecast available in
        # production. The realized suffix length is an offline oracle and can
        # materially alter both equal slicing and the last-turn branch.
        remaining_turns = max(
            1,
            _feature_int(curve, "predicted_future_turns", 0) + 1,
        )
        learned = choose_learned(
            model,
            curve,
            learned_budget,
            remaining_turns == 1,
            cost_quantum,
        )
        equal = choose_equal(
            curve, equal_budget, remaining_turns, cost_quantum
        )
        if learned is None or equal is None:
            return None
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
        honest_choice=all(
            curve.has_separate_expected_regret for curve in curves
        ),
    )


def replay_heldout(
    model: Model,
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


def clustered_summary(
    results: list[ReplayResult], planning_model_honest: bool = False
) -> dict[str, object]:
    if not results:
        return {
            "source_games": 0,
            "replays": 0,
            "mean_learned_minus_equal": None,
            "ci95": [None, None],
            "p_value": None,
            "honest_choice_replays": 0,
            "legacy_oracle_choice_replays": 0,
            "planning_model_honest": planning_model_honest,
            "surrogate_gate_passed": False,
        }
    by_game: dict[str, list[ReplayResult]] = defaultdict(list)
    for result in results:
        by_game[result.game].append(result)
    game_deltas = [
        sum(item.learned_minus_equal for item in items) / len(items)
        for items in by_game.values()
    ]
    mean = sum(game_deltas) / len(game_deltas)
    if len(game_deltas) >= 2:
        variance = sum((value - mean) ** 2 for value in game_deltas) / (
            len(game_deltas) - 1
        )
        standard_error = math.sqrt(variance / len(game_deltas))
        degrees_freedom = len(game_deltas) - 1
        half_width = (
            student_t_critical(0.95, degrees_freedom) * standard_error
        )
        if standard_error > 0.0:
            p_value = student_t_two_sided_p_value(
                mean / standard_error, degrees_freedom
            )
        else:
            p_value = 0.0 if mean != 0.0 else 1.0
    else:
        half_width = None
        p_value = None
    ci = (
        [mean - half_width, mean + half_width]
        if half_width is not None
        else [None, None]
    )
    return {
        "source_games": len(by_game),
        "replays": len(results),
        "mean_learned_regret": sum(item.learned_regret for item in results)
        / len(results),
        "mean_equal_regret": sum(item.equal_regret for item in results)
        / len(results),
        "mean_learned_expected_regret": sum(
            item.learned_expected_regret for item in results
        )
        / len(results),
        "mean_equal_expected_regret": sum(
            item.equal_expected_regret for item in results
        )
        / len(results),
        "mean_learned_calibration_residual": sum(
            item.learned_regret - item.learned_expected_regret
            for item in results
        )
        / len(results),
        "mean_equal_calibration_residual": sum(
            item.equal_regret - item.equal_expected_regret
            for item in results
        )
        / len(results),
        "honest_choice_replays": sum(item.honest_choice for item in results),
        "legacy_oracle_choice_replays": sum(
            not item.honest_choice for item in results
        ),
        "planning_model_honest": planning_model_honest,
        "mean_oracle_regret": sum(item.oracle_regret for item in results)
        / len(results),
        "mean_learned_excess_oracle": sum(
            item.learned_excess_oracle for item in results
        )
        / len(results),
        "mean_equal_excess_oracle": sum(
            item.equal_excess_oracle for item in results
        )
        / len(results),
        "mean_learned_minus_equal": mean,
        "ci95": ci,
        "p_value": p_value,
        # Passing this gate means only that the allocator improved the
        # realized-trajectory oracle-regret surrogate on held-out games.
        "surrogate_gate_passed": (
            len(by_game) >= 20
            and planning_model_honest
            and all(item.honest_choice for item in results)
            and ci[1] is not None
            and ci[1] < 0.0
        ),
    }


def labels_have_honest_planning(path: Path) -> bool:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {"planning_regret_valid", "allocation_policy"}
        if not required.issubset(reader.fieldnames or []):
            return False
        relevant = [
            row
            for row in reader
            if int(row["forecast_valid"]) and int(row["future_turns"]) > 0
        ]
    return bool(relevant) and all(
        int(row["planning_regret_valid"])
        and row["allocation_policy"] == "equal_slice_policy"
        for row in relevant
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("labels", type=Path)
    parser.add_argument("curves", type=Path)
    parser.add_argument("--cost-quantum", required=True, type=float)
    parser.add_argument("--split-seed", type=int, default=633)
    parser.add_argument("--prior-strength", type=float, default=20.0)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    labels = read_labels(args.labels)
    planning_model_honest = labels_have_honest_planning(args.labels)
    train_games, calibration_games, test_games = split_games(
        (label.game for label in labels), args.split_seed
    )
    model = fit_model(
        [label for label in labels if label.game in train_games],
        args.prior_strength,
    )
    with args.curves.open(newline="", encoding="utf-8") as stream:
        curves = read_turn_curves(stream, args.cost_quantum)

    calibration = clustered_summary(
        replay_heldout(
            model,
            curves,
            calibration_games,
            model.budgets,
            args.cost_quantum,
        ),
        planning_model_honest,
    )
    test = clustered_summary(
        replay_heldout(
            model,
            curves,
            test_games,
            model.budgets,
            args.cost_quantum,
        ),
        planning_model_honest,
    )
    document = {
        "artifact_kind": "rest_game_value_policy_backtest",
        "scope": "heldout_realized_trajectory_expected_choice_oracle_score",
        "split_seed": args.split_seed,
        "calibration": calibration,
        "test": test,
        "terminal_game_gate_passed": False,
        "live_enabled": False,
        "gate_reason": "mirrored terminal-utility game-pair gate not supplied",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print(
        f"calibration_games={calibration['source_games']} "
        f"test_games={test['source_games']} "
        f"surrogate_gate={int(bool(calibration['surrogate_gate_passed']))} "
        "terminal_gate=0 live_enabled=0 "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()

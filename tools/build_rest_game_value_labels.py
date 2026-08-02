#!/usr/bin/env python3
"""Build rest-of-game allocation labels from complete turn curves.

The input is a CSV with one row per legal work boundary and these required
columns:

    game, turn, player, cost, regret[, expected_regret]

All rows for one (game, turn, player) form that turn's work/regret curve.
``regret`` is always the held-out score. When ``expected_regret`` is present,
the allocator uses it and retains ``regret`` only for later policy scoring.
Legacy inputs without it use the oracle score for both roles and therefore
cannot establish an oracle-independent policy gate.
`cost` may be seconds for one runtime-rate scenario or any other consistent
portable cost unit. To keep the eventual runtime artifact hardware-independent,
generate labels under several mode-rate scenarios rather than baking one
machine's wall clock into the source curves.

For every current turn and requested remaining-cost budget, this tool
evaluates a realizable equal-slice policy over that player's *later* turns
using the planning-regret column. The historical hindsight-optimal suffix
allocator remains available as an explicitly labeled prophet bound, but it is
not a valid runtime value-to-go target. Complete source games must remain
intact when splitting the result into training/calibration/test sets.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import math
from pathlib import Path
import sys
from typing import Iterable, TextIO


@dataclass(frozen=True)
class WorkOption:
    cost_units: int
    # Regret available to the offline/runtime allocator. In legacy inputs this
    # falls back to the oracle score; honest policy tests provide a separately
    # cross-fitted ``expected_regret`` column.
    regret: float
    # Held-out score for evaluating the choice. It must never break a tie or
    # otherwise affect selection when expected_regret was supplied.
    actual_regret: float | None = None

    @property
    def score_regret(self) -> float:
        return self.regret if self.actual_regret is None else self.actual_regret


@dataclass(frozen=True)
class TurnCurve:
    game: str
    turn: int
    player: int
    rate_profile: int
    mandatory_cost: float
    options: tuple[WorkOption, ...]
    features: dict[str, str]
    has_separate_expected_regret: bool = False
    # Exact measured boundaries before expected-regret dominance pruning.
    # Baseline policies that commit to a fixed budget must select from these,
    # because a held-out score may worsen at a later boundary.
    all_options: tuple[WorkOption, ...] = ()


@dataclass(frozen=True)
class ValueLabel:
    game: str
    turn: int
    player: int
    rate_profile: int
    budget: float
    forecast_valid: bool
    future_turns: int
    mandatory_future_cost: float
    discretionary_future_cost: float
    oracle_future_regret: float
    future_loss_per_cost: float
    future_gain_per_cost: float
    features: dict[str, str]
    planning_regret_valid: bool
    allocation_policy: str


REQUIRED_FIELDS = {"game", "turn", "player", "cost", "regret"}
OPTION_FIELDS = {
    "cost",
    "regret",
    "expected_regret",
    "option",
    # Native work coordinates vary between legal result boundaries and are
    # inputs to cost conversion, not state features for value-to-go fitting.
    "nodes",
    "scenarios",
    "candidates",
    "fixed_seconds",
    "greedy_scenarios",
    "greedy_endgame_nodes",
    "root_candidates",
    "added_scenarios",
    "added_endgame_nodes",
    "added_candidates",
    "observed_seconds",
    "depth",
    "stage",
    "plies",
    "tail_imputed",
    "tail_scenario",
}

ALLOCATION_POLICY_EQUAL_SLICE = "equal_slice_policy"
ALLOCATION_POLICY_PROPHET_BOUND = "prophet_bound"
ALLOCATION_POLICIES = (
    ALLOCATION_POLICY_EQUAL_SLICE,
    ALLOCATION_POLICY_PROPHET_BOUND,
)


def _finite_nonnegative(value: str, field: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ValueError(f"{field} must be finite and nonnegative, got {value!r}")
    return parsed


def _normalize_turn_rows(
    rows: list[dict[str, str]], cost_quantum: float
) -> TurnCurve:
    first = rows[0]
    game = first["game"]
    turn = int(first["turn"])
    player = int(first["player"])
    rate_profile = int(first.get("rate_profile", "0"))
    if player < 0:
        raise ValueError("player must be nonnegative")

    feature_fields = set(first) - OPTION_FIELDS
    features = {field: first[field] for field in feature_fields}
    for row in rows[1:]:
        for field in feature_fields:
            if row.get(field) != features[field]:
                raise ValueError(
                    f"inconsistent {field} for game={game} turn={turn} player={player}"
                )

    expected_presence = {
        row.get("expected_regret", "").strip() != "" for row in rows
    }
    if len(expected_presence) != 1:
        raise ValueError(
            f"partial expected_regret curve for game={game} turn={turn}"
        )
    has_expected_regret = expected_presence.pop()
    raw_options = []
    for row_index, row in enumerate(rows):
        actual_regret = _finite_nonnegative(row["regret"], "regret")
        expected_regret = (
            _finite_nonnegative(row["expected_regret"], "expected_regret")
            if has_expected_regret
            else actual_regret
        )
        raw_options.append(
            (
                _finite_nonnegative(row["cost"], "cost"),
                expected_regret,
                actual_regret,
                row_index,
            )
        )
    mandatory_cost = min(cost for cost, _, _, _ in raw_options)

    # Round incremental costs upward. A training label must never buy work that
    # does not actually fit the advertised budget merely because of binning.
    by_units: dict[int, tuple[float, float, int]] = {}
    for cost, expected_regret, actual_regret, row_index in raw_options:
        incremental = max(0.0, cost - mandatory_cost)
        units = int(math.ceil(incremental / cost_quantum - 1.0e-12))
        previous = by_units.get(units)
        # Equal-cost ties are resolved by the original stable row order, not
        # by the held-out oracle score.
        if previous is None or expected_regret < previous[0] - 1.0e-15:
            by_units[units] = (expected_regret, actual_regret, row_index)

    # For a start-of-turn allocator, discard an absolute boundary predicted no
    # better than a cheaper boundary: it can plan to stop at the cheaper one.
    # Keep all_options unchanged for equal slicing and sequential-package
    # analysis. This pruning does not claim that a live solver may revert to an
    # earlier move after it has completed and published a deeper boundary.
    all_options = [
        WorkOption(units, expected_regret, actual_regret)
        for units, (expected_regret, actual_regret, _) in sorted(
            by_units.items()
        )
    ]
    options: list[WorkOption] = []
    best_regret = math.inf
    for option in all_options:
        if option.regret + 1.0e-15 < best_regret:
            options.append(option)
            best_regret = option.regret
    if not options or options[0].cost_units != 0:
        raise ValueError(
            f"turn curve lacks a minimum-cost option for game={game} turn={turn}"
        )
    return TurnCurve(
        game=game,
        turn=turn,
        player=player,
        rate_profile=rate_profile,
        mandatory_cost=mandatory_cost,
        options=tuple(options),
        features=features,
        has_separate_expected_regret=has_expected_regret,
        all_options=tuple(all_options),
    )


def read_turn_curves(stream: TextIO, cost_quantum: float) -> list[TurnCurve]:
    if not math.isfinite(cost_quantum) or cost_quantum <= 0.0:
        raise ValueError("cost_quantum must be finite and positive")
    reader = csv.DictReader(stream)
    if reader.fieldnames is None or not REQUIRED_FIELDS.issubset(reader.fieldnames):
        missing = sorted(REQUIRED_FIELDS - set(reader.fieldnames or []))
        raise ValueError(f"curve CSV is missing required fields: {','.join(missing)}")

    grouped: dict[tuple[str, int, int, int], list[dict[str, str]]] = {}
    for row in reader:
        key = (
            row["game"],
            int(row["turn"]),
            int(row["player"]),
            int(row.get("rate_profile", "0")),
        )
        grouped.setdefault(key, []).append(row)

    curves = [
        _normalize_turn_rows(rows, cost_quantum) for rows in grouped.values()
    ]
    curves.sort(
        key=lambda curve: (
            curve.game,
            curve.rate_profile,
            curve.player,
            curve.turn,
        )
    )
    return curves


def _suffix_regret_tables(
    curves: list[TurnCurve], maximum_units: int
) -> list[list[float]]:
    """Return optimal regret for every suffix and discretionary capacity."""
    tables = [[0.0] * (maximum_units + 1) for _ in range(len(curves) + 1)]
    for index in range(len(curves) - 1, -1, -1):
        following = tables[index + 1]
        current = tables[index]
        for capacity in range(maximum_units + 1):
            best = math.inf
            for option in curves[index].options:
                if option.cost_units > capacity:
                    break
                candidate = option.regret + following[capacity - option.cost_units]
                if candidate < best:
                    best = candidate
            current[capacity] = best
    return tables


def _predicted_turns_remaining(curve: TurnCurve) -> int:
    raw = curve.features.get("predicted_future_turns", "0")
    try:
        future = int(raw)
    except ValueError as error:
        raise ValueError(
            "predicted_future_turns must be an integer for "
            f"game={curve.game} turn={curve.turn}"
        ) from error
    if future < 0:
        raise ValueError("predicted_future_turns must be nonnegative")
    return future + 1


def _equal_slice_policy_tables(
    curves: list[TurnCurve],
    maximum_units: int,
    cost_quantum: float,
    mandatory_suffix: list[float],
) -> list[list[float]]:
    """Evaluate equal slicing without seeing later-turn curve difficulty."""

    tables = [[math.inf] * (maximum_units + 1) for _ in range(len(curves) + 1)]
    tables[-1] = [0.0] * (maximum_units + 1)
    for index in range(len(curves) - 1, -1, -1):
        curve = curves[index]
        following = tables[index + 1]
        current = tables[index]
        turns_remaining = _predicted_turns_remaining(curve)
        for capacity in range(maximum_units + 1):
            remaining_budget = mandatory_suffix[index] + capacity * cost_quantum
            slice_budget = remaining_budget / turns_remaining
            affordable = [
                option
                for option in (curve.all_options or curve.options)
                if curve.mandatory_cost + option.cost_units * cost_quantum
                <= slice_budget + 1.0e-12
            ]
            if affordable:
                # Equal slicing commits to the deepest completed boundary that
                # fits its state-derived slice. Expected or oracle regret does
                # not break ties or choose the boundary.
                choice = max(affordable, key=lambda option: option.cost_units)
            else:
                # The live fallback must still return a legal result when the
                # nominal slice is below mandatory work.
                choice = min(
                    curve.all_options or curve.options,
                    key=lambda option: option.cost_units,
                )
            if choice.cost_units > capacity:
                # A bad turn-count forecast can consume clock needed for a
                # later mandatory boundary. That is a genuine policy failure,
                # not an invitation to choose a different action in hindsight.
                continue
            suffix_regret = following[capacity - choice.cost_units]
            if math.isfinite(suffix_regret):
                current[capacity] = choice.regret + suffix_regret
    return tables


def build_value_labels(
    curves: Iterable[TurnCurve],
    budgets: Iterable[float],
    cost_quantum: float,
    allocation_policy: str = ALLOCATION_POLICY_EQUAL_SLICE,
) -> list[ValueLabel]:
    if allocation_policy not in ALLOCATION_POLICIES:
        raise ValueError(f"unknown allocation policy {allocation_policy!r}")
    budget_values = sorted(set(float(value) for value in budgets))
    if not budget_values:
        raise ValueError("at least one budget is required")
    for budget in budget_values:
        if not math.isfinite(budget) or budget < 0.0:
            raise ValueError("budgets must be finite and nonnegative")

    by_game_player: dict[tuple[str, int, int], list[TurnCurve]] = {}
    for curve in curves:
        by_game_player.setdefault(
            (curve.game, curve.rate_profile, curve.player), []
        ).append(curve)
    for sequence in by_game_player.values():
        sequence.sort(key=lambda curve: curve.turn)
        if len({curve.turn for curve in sequence}) != len(sequence):
            raise ValueError("duplicate turn in a game/player sequence")

    maximum_units = int(math.floor(max(budget_values) / cost_quantum + 1.0e-12))
    labels: list[ValueLabel] = []
    for sequence in by_game_player.values():
        mandatory_suffix = [0.0] * (len(sequence) + 1)
        for index in range(len(sequence) - 1, -1, -1):
            mandatory_suffix[index] = (
                sequence[index].mandatory_cost + mandatory_suffix[index + 1]
            )
        tables = (
            _equal_slice_policy_tables(
                sequence, maximum_units, cost_quantum, mandatory_suffix
            )
            if allocation_policy == ALLOCATION_POLICY_EQUAL_SLICE
            else _suffix_regret_tables(sequence, maximum_units)
        )

        for index, current_curve in enumerate(sequence):
            # The future opportunity set deliberately excludes the current
            # turn. Its next-chunk value is estimated by the current solver;
            # this label prices preserving clock for later turns.
            future_index = index + 1
            future_turns = len(sequence) - future_index
            planning_regret_valid = all(
                curve.has_separate_expected_regret
                for curve in sequence[future_index:]
            )
            required = mandatory_suffix[future_index]
            table = tables[future_index]
            for budget in budget_values:
                if budget + 1.0e-12 < required:
                    labels.append(
                        ValueLabel(
                            game=current_curve.game,
                            turn=current_curve.turn,
                            player=current_curve.player,
                            rate_profile=current_curve.rate_profile,
                            budget=budget,
                            forecast_valid=False,
                            future_turns=future_turns,
                            mandatory_future_cost=required,
                            discretionary_future_cost=math.nan,
                            oracle_future_regret=math.nan,
                            future_loss_per_cost=math.nan,
                            future_gain_per_cost=math.nan,
                            features=current_curve.features,
                            planning_regret_valid=planning_regret_valid,
                            allocation_policy=allocation_policy,
                        )
                    )
                    continue

                discretionary = budget - required
                units = min(
                    maximum_units,
                    int(math.floor(discretionary / cost_quantum + 1.0e-12)),
                )
                regret = table[units]
                if not math.isfinite(regret):
                    labels.append(
                        ValueLabel(
                            game=current_curve.game,
                            turn=current_curve.turn,
                            player=current_curve.player,
                            rate_profile=current_curve.rate_profile,
                            budget=budget,
                            forecast_valid=False,
                            future_turns=future_turns,
                            mandatory_future_cost=required,
                            discretionary_future_cost=discretionary,
                            oracle_future_regret=math.nan,
                            future_loss_per_cost=math.nan,
                            future_gain_per_cost=math.nan,
                            features=current_curve.features,
                            planning_regret_valid=planning_regret_valid,
                            allocation_policy=allocation_policy,
                        )
                    )
                    continue
                loss = (
                    max(0.0, table[units - 1] - regret) / cost_quantum
                    if units > 0
                    else math.inf if future_turns > 0 else 0.0
                )
                gain = (
                    max(0.0, regret - table[units + 1]) / cost_quantum
                    if units < maximum_units
                    else math.nan
                )
                labels.append(
                    ValueLabel(
                        game=current_curve.game,
                        turn=current_curve.turn,
                        player=current_curve.player,
                        rate_profile=current_curve.rate_profile,
                        budget=budget,
                        forecast_valid=True,
                        future_turns=future_turns,
                        mandatory_future_cost=required,
                        discretionary_future_cost=discretionary,
                        oracle_future_regret=regret,
                        future_loss_per_cost=loss,
                        future_gain_per_cost=gain,
                        features=current_curve.features,
                        planning_regret_valid=planning_regret_valid,
                        allocation_policy=allocation_policy,
                    )
                )
    labels.sort(
        key=lambda label: (
            label.game,
            label.rate_profile,
            label.turn,
            label.player,
            label.budget,
        )
    )
    return labels


def _format_float(value: float) -> str:
    return f"{value:.12g}" if math.isfinite(value) else str(value).lower()


def write_labels(labels: list[ValueLabel], stream: TextIO) -> None:
    feature_fields = sorted(
        {
            field
            for label in labels
            for field in label.features
            if field not in {"game", "turn", "player", "rate_profile"}
        }
    )
    fields = [
        "game",
        "turn",
        "player",
        "rate_profile",
        "budget",
        "forecast_valid",
        "future_turns",
        "mandatory_future_cost",
        "discretionary_future_cost",
        "oracle_future_regret",
        "future_loss_per_cost",
        "future_gain_per_cost",
        "planning_regret_valid",
        "allocation_policy",
        "label_scope",
        *feature_fields,
    ]
    writer = csv.DictWriter(stream, fieldnames=fields)
    writer.writeheader()
    for label in labels:
        row = {
            "game": label.game,
            "turn": label.turn,
            "player": label.player,
            "rate_profile": label.rate_profile,
            "budget": _format_float(label.budget),
            "forecast_valid": int(label.forecast_valid),
            "future_turns": label.future_turns,
            "mandatory_future_cost": _format_float(label.mandatory_future_cost),
            "discretionary_future_cost": _format_float(
                label.discretionary_future_cost
            ),
            "oracle_future_regret": _format_float(label.oracle_future_regret),
            "future_loss_per_cost": _format_float(label.future_loss_per_cost),
            "future_gain_per_cost": _format_float(label.future_gain_per_cost),
            "planning_regret_valid": int(label.planning_regret_valid),
            "allocation_policy": label.allocation_policy,
            "label_scope": (
                (
                    "realized_trajectory_equal_slice_policy_evaluation"
                    if label.allocation_policy == ALLOCATION_POLICY_EQUAL_SLICE
                    else "realized_suffix_expected_regret_prophet_bound"
                )
                if label.planning_regret_valid
                else "realized_trajectory_oracle_choice_contaminated"
            ),
        }
        row.update(
            {
                field: label.features.get(field, "")
                for field in feature_fields
            }
        )
        writer.writerow(row)


def _comma_floats(value: str) -> list[float]:
    try:
        values = [float(item) for item in value.split(",") if item]
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if not values:
        raise argparse.ArgumentTypeError("expected at least one number")
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", type=Path)
    parser.add_argument("--budgets", required=True, type=_comma_floats)
    parser.add_argument("--cost-quantum", required=True, type=float)
    parser.add_argument(
        "--allocation-policy",
        choices=ALLOCATION_POLICIES,
        default=ALLOCATION_POLICY_EQUAL_SLICE,
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    with args.curves.open(newline="", encoding="utf-8") as stream:
        curves = read_turn_curves(stream, args.cost_quantum)
    labels = build_value_labels(
        curves,
        args.budgets,
        args.cost_quantum,
        allocation_policy=args.allocation_policy,
    )

    if args.output is None:
        write_labels(labels, sys.stdout)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="", encoding="utf-8") as stream:
            write_labels(labels, stream)


if __name__ == "__main__":
    main()

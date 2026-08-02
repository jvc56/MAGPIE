#!/usr/bin/env python3

from __future__ import annotations

import io
import math
import unittest

from tools.build_rest_game_value_labels import (
    build_value_labels,
    read_turn_curves,
)


def curves(text: str, quantum: float = 1.0):
    return read_turn_curves(io.StringIO(text), quantum)


class RestGameValueLabelsTest(unittest.TestCase):
    def test_future_allocator_excludes_current_turn_and_prices_suffix(self) -> None:
        source = """game,turn,player,cost,regret,bag
g,0,0,0,9,10
g,0,0,1,0,10
g,2,0,0,1,5
g,2,0,1,0,5
g,4,0,0,2,0
g,4,0,1,0,0
g,1,1,0,7,8
g,1,1,1,0,8
"""
        labels = build_value_labels(curves(source), [0, 1, 2], 1.0)
        by_key = {
            (label.turn, label.player, label.budget): label for label in labels
        }

        # Turn 0's own dramatic curve is excluded. One unit goes to the more
        # valuable later turn (turn 4), and two units solve both future turns.
        self.assertEqual(by_key[(0, 0, 0.0)].oracle_future_regret, 3.0)
        self.assertEqual(by_key[(0, 0, 1.0)].oracle_future_regret, 1.0)
        self.assertEqual(by_key[(0, 0, 2.0)].oracle_future_regret, 0.0)
        self.assertEqual(by_key[(0, 0, 1.0)].future_loss_per_cost, 2.0)
        self.assertEqual(by_key[(0, 0, 2.0)].future_loss_per_cost, 1.0)
        self.assertFalse(by_key[(0, 0, 2.0)].planning_regret_valid)

        # A player's suffix contains only that player's later decisions.
        self.assertEqual(by_key[(1, 1, 2.0)].future_turns, 0)
        self.assertEqual(by_key[(1, 1, 2.0)].oracle_future_regret, 0.0)
        self.assertEqual(by_key[(1, 1, 2.0)].future_loss_per_cost, 0.0)

    def test_mandatory_minimum_cost_is_reserved_before_discretionary_work(self) -> None:
        source = """game,turn,player,cost,regret
g,0,0,0,0
g,2,0,5,1
g,2,0,6,0
"""
        labels = build_value_labels(curves(source), [4, 5, 6], 1.0)
        by_budget = {label.budget: label for label in labels if label.turn == 0}
        self.assertFalse(by_budget[4.0].forecast_valid)
        self.assertTrue(math.isnan(by_budget[4.0].oracle_future_regret))
        self.assertEqual(by_budget[5.0].mandatory_future_cost, 5.0)
        self.assertEqual(by_budget[5.0].oracle_future_regret, 1.0)
        self.assertEqual(by_budget[6.0].oracle_future_regret, 0.0)

    def test_more_expensive_worse_option_is_removed(self) -> None:
        source = """game,turn,player,cost,regret
g,0,0,0,1
g,0,0,1,2
g,0,0,2,0
"""
        parsed = curves(source)
        self.assertEqual([(option.cost_units, option.regret) for option in parsed[0].options], [(0, 1.0), (2, 0.0)])
        self.assertEqual(
            [option.cost_units for option in parsed[0].all_options],
            [0, 1, 2],
        )

    def test_expected_regret_selects_without_reading_actual_regret(self) -> None:
        source = """game,turn,player,cost,regret,expected_regret
g,0,0,0,9,0
g,0,0,1,0,1
g,2,0,0,1,0.2
g,2,0,1,0,0.1
"""
        parsed = curves(source)
        self.assertTrue(parsed[0].has_separate_expected_regret)
        # The planning envelope keeps the cheap option because its expected
        # regret is lower, even though the held-out oracle prefers the other.
        self.assertEqual(len(parsed[0].options), 1)
        self.assertEqual(parsed[0].options[0].regret, 0.0)
        self.assertEqual(parsed[0].options[0].score_regret, 9.0)
        self.assertEqual(len(parsed[0].all_options), 2)
        labels = build_value_labels(parsed, [1], 1.0)
        at_turn_zero = next(label for label in labels if label.turn == 0)
        self.assertTrue(at_turn_zero.planning_regret_valid)

    def test_runtime_rate_profiles_are_allocated_independently(self) -> None:
        source = """game,turn,player,rate_profile,cost,regret,bag
g,0,0,0,0,0,20
g,2,0,0,0,1,10
g,2,0,0,1,0,10
g,0,0,1,0,0,20
g,2,0,1,0,2,10
g,2,0,1,1,1,10
"""
        labels = build_value_labels(curves(source), [0, 1], 1.0)
        at_start = {
            (label.rate_profile, label.budget): label.oracle_future_regret
            for label in labels
            if label.turn == 0
        }
        self.assertEqual(at_start[(0, 0.0)], 1.0)
        self.assertEqual(at_start[(0, 1.0)], 0.0)
        self.assertEqual(at_start[(1, 0.0)], 2.0)
        self.assertEqual(at_start[(1, 1.0)], 1.0)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import io
import unittest

from tools.backtest_water_filling_policy import (
    build_lambda_grid,
    compare_policies,
    fit_demand_model,
    predict_future_cost,
    replay_sequence,
)
from tools.backtest_rest_game_value_policy import ReplayResult
from tools.build_rest_game_value_labels import read_turn_curves


class BacktestWaterFillingPolicyTest(unittest.TestCase):
    def test_dual_policy_banks_for_more_valuable_later_work(self) -> None:
        rows = [
            "game,turn,player,rate_profile,cost,regret,expected_regret,bag,"
            "spread,predicted_future_turns"
        ]
        # Training games teach expected future demand under each fixed lambda.
        for game in ("a", "b", "c"):
            rows.extend(
                [
                    f"{game},0,0,0,0,1,1,20,0,1",
                    f"{game},0,0,0,.5,0,0,20,0,1",
                    f"{game},2,0,0,0,10,10,10,0,0",
                    f"{game},2,0,0,1,0,0,10,0,0",
                ]
            )
        training = read_turn_curves(io.StringIO("\n".join(rows) + "\n"), 0.5)
        model = fit_demand_model(training, 0.5, prior_strength=0.0)

        heldout = read_turn_curves(
            io.StringIO(
                "game,turn,player,rate_profile,cost,regret,expected_regret,"
                "bag,spread,predicted_future_turns\n"
                "test,0,0,0,0,1,1,20,0,1\n"
                "test,0,0,0,.5,0,0,20,0,1\n"
                "test,2,0,0,0,10,10,10,0,0\n"
                "test,2,0,0,1,0,0,10,0,0\n"
            ),
            0.5,
        )
        result = replay_sequence(model, heldout, 1.0, 0.5)

        assert result is not None
        self.assertEqual(result.learned_regret, 1.0)
        self.assertEqual(result.equal_regret, 10.0)
        self.assertGreater(result.divergent_turns, 0)
        self.assertAlmostEqual(result.learned_spent, 1.0)

    def test_lambda_grid_reaches_cheapest_boundary(self) -> None:
        curves = read_turn_curves(
            io.StringIO(
                "game,turn,player,cost,regret,expected_regret,bag,spread,"
                "predicted_future_turns\n"
                "g,0,0,0,1,1,0,0,0\n"
                "g,0,0,2,0,0,0,0,0\n"
            ),
            1.0,
        )
        lambdas = build_lambda_grid(curves, 1.0, points=9)
        self.assertEqual(lambdas[0], 0.0)
        self.assertGreater(lambdas[-1], 0.5)
        self.assertEqual(tuple(sorted(lambdas)), lambdas)

    def test_training_rejects_oracle_choice_curves(self) -> None:
        curves = read_turn_curves(
            io.StringIO(
                "game,turn,player,cost,regret,bag,spread,"
                "predicted_future_turns\n"
                "g,0,0,0,1,0,0,0\n"
            ),
            1.0,
        )
        with self.assertRaisesRegex(ValueError, "separate planning regret"):
            fit_demand_model(curves, 1.0)

    def test_future_demand_averages_before_lambda_is_chosen(self) -> None:
        curves = read_turn_curves(
            io.StringIO(
                "game,turn,player,cost,regret,expected_regret,bag,spread,"
                "predicted_future_turns\n"
                "a,0,0,0,0,0,20,0,1\n"
                "a,2,0,0,10,10,10,0,0\n"
                "a,2,0,1,0,0,10,0,0\n"
                "b,0,0,0,0,0,20,0,1\n"
                "b,2,0,0,1,1,10,0,0\n"
                "b,2,0,1,0,0,10,0,0\n"
            ),
            1.0,
        )
        model = fit_demand_model(curves, 1.0, prior_strength=0.0)
        probe = next(
            curve
            for curve in curves
            if curve.game == "a" and curve.turn == 0
        )
        middle_demands = [
            predict_future_cost(model, probe, index)
            for index, shadow_price in enumerate(model.lambdas)
            if 1.0 < shadow_price < 10.0
        ]

        # One training suffix buys the future option and the other does not.
        # The current-state demand target is their expectation, not a separate
        # hindsight-selected lambda for either suffix.
        self.assertIn(0.5, middle_demands)

    def test_direct_policy_comparison_is_paired_by_replay_key(self) -> None:
        common = dict(
            game="g",
            rate_profile=0,
            player=0,
            starting_budget=10.0,
            oracle_regret=0.0,
            turns=2,
            divergent_turns=1,
            honest_choice=True,
        )
        water = ReplayResult(
            learned_regret=1.0,
            equal_regret=3.0,
            learned_expected_regret=1.5,
            equal_expected_regret=3.0,
            learned_spent=9.0,
            equal_spent=8.0,
            **common,
        )
        value = ReplayResult(
            learned_regret=2.0,
            equal_regret=3.0,
            learned_expected_regret=2.5,
            equal_expected_regret=3.0,
            learned_spent=8.5,
            equal_spent=8.0,
            **common,
        )

        paired = compare_policies([water], [value])

        self.assertEqual(len(paired), 1)
        self.assertEqual(paired[0].learned_minus_equal, -1.0)
        self.assertEqual(paired[0].learned_spent, 9.0)
        self.assertEqual(paired[0].equal_spent, 8.5)


if __name__ == "__main__":
    unittest.main()

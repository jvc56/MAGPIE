#!/usr/bin/env python3

from __future__ import annotations

import io
import unittest

from tools.backtest_rest_game_value_policy import (
    clustered_summary,
    fit_cuped_theta,
    replay_sequence,
    required_games_for_effect,
)
from tools.build_rest_game_value_labels import read_turn_curves
from tools.fit_rest_game_value_model import Label, fit_model


class BacktestRestGameValuePolicyTest(unittest.TestCase):
    def test_learned_policy_can_deposit_for_a_more_valuable_later_turn(self) -> None:
        # Current turn: half a unit removes one point of regret. Later turn: a
        # full unit removes ten. Equal slicing of a one-unit total budget buys
        # the current option; the learned suffix curve saves it for later.
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,bag,spread,"
            "predicted_future_turns,own_rack,opp_rack\n"
            "test,0,0,0,0,1,20,0,1,7,7\n"
            "test,0,0,0,0.5,0,20,0,1,7,7\n"
            "test,2,0,0,0,10,10,0,0,7,7\n"
            "test,2,0,0,1,0,10,0,0,7,7\n"
        )
        curves = read_turn_curves(source, 0.5)
        training: list[Label] = []
        for game in ("a", "b", "c"):
            training.extend(
                [
                    Label(game, 0.0, 10.0, 20, 0, 1),
                    Label(game, 0.5, 10.0, 20, 0, 1),
                    Label(game, 1.0, 0.0, 20, 0, 1),
                ]
            )
        model = fit_model(training, prior_strength=0.0)
        result = replay_sequence(model, curves, 1.0, 0.5)
        self.assertIsNotNone(result)
        assert result is not None
        self.assertEqual(result.learned_regret, 1.0)
        self.assertEqual(result.equal_regret, 10.0)
        self.assertEqual(result.oracle_regret, 1.0)

    def test_gate_clusters_repeated_measurements_by_source_game(self) -> None:
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,bag,spread,"
            "predicted_future_turns\n"
            "g,0,0,0,0,1,0,0,0\n"
        )
        curve = read_turn_curves(source, 1.0)
        model = fit_model(
            [
                Label("a", 0.0, 0.0, 0, 0, 0),
                Label("b", 0.0, 0.0, 0, 0, 0),
                Label("c", 0.0, 0.0, 0, 0, 0),
            ]
        )
        result = replay_sequence(model, curve, 0.0, 1.0)
        assert result is not None
        summary = clustered_summary([result, result])
        self.assertEqual(summary["source_games"], 1)
        self.assertEqual(summary["replays"], 2)
        self.assertFalse(summary["surrogate_gate_passed"])

    def test_expected_choice_is_frozen_before_heldout_scoring(self) -> None:
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,expected_regret,bag,"
            "spread,predicted_future_turns\n"
            # Expected regret says stop cheaply; the held-out oracle says the
            # exact opposite. Learned must not peek, while equal slicing buys
            # the deepest boundary fitting its committed slice.
            "test,0,0,0,0,9,0,0,0,0\n"
            "test,0,0,0,1,0,1,0,0,0\n"
        )
        curves = read_turn_curves(source, 1.0)
        model = fit_model(
            [
                Label("a", 0.0, 0.0, 0, 0, 0),
                Label("b", 0.0, 0.0, 0, 0, 0),
                Label("c", 0.0, 0.0, 0, 0, 0),
            ]
        )
        result = replay_sequence(model, curves, 1.0, 1.0)
        assert result is not None
        self.assertTrue(result.honest_choice)
        self.assertEqual(result.learned_expected_regret, 0.0)
        self.assertEqual(result.learned_regret, 9.0)
        self.assertEqual(result.equal_expected_regret, 1.0)
        self.assertEqual(result.equal_regret, 0.0)
        self.assertEqual(result.oracle_regret, 0.0)

    def test_replay_uses_predicted_not_realized_turns_remaining(self) -> None:
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,expected_regret,bag,"
            "spread,predicted_future_turns\n"
            # Although the corpus contains a later turn, the state forecast
            # says this is the last one. Both policies must spend the full
            # one-unit slice instead of dividing by the realized suffix.
            "test,0,0,0,0,1,1,0,0,0\n"
            "test,0,0,0,1,0,0,0,0,0\n"
            "test,2,0,0,0,0,0,0,0,0\n"
        )
        curves = read_turn_curves(source, 1.0)
        model = fit_model(
            [
                Label("a", 0.0, 0.0, 0, 0, 0),
                Label("b", 0.0, 0.0, 0, 0, 0),
                Label("c", 0.0, 0.0, 0, 0, 0),
            ]
        )

        result = replay_sequence(model, curves, 1.0, 1.0)

        assert result is not None
        self.assertEqual(result.learned_spent, 1.0)
        self.assertEqual(result.equal_spent, 1.0)
        self.assertEqual(result.learned_regret, 0.0)
        self.assertEqual(result.equal_regret, 0.0)

    def test_gate_requires_preregistered_power_and_allocation_divergence(
        self,
    ) -> None:
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,expected_regret,bag,"
            "spread,predicted_future_turns\n"
            "g,0,0,0,0,1,0,0,0,0\n"
            "g,0,0,0,1,0,1,0,0,0\n"
        )
        curves = read_turn_curves(source, 1.0)
        model = fit_model(
            [
                Label("a", 0.0, 0.0, 0, 0, 0),
                Label("b", 0.0, 0.0, 0, 0, 0),
                Label("c", 0.0, 0.0, 0, 0, 0),
            ]
        )
        result = replay_sequence(model, curves, 1.0, 1.0)
        assert result is not None
        self.assertEqual(result.divergent_turns, 1)
        self.assertTrue(result.allocation_diverged)
        self.assertFalse(
            clustered_summary(
                [result], planning_model_honest=True, divergent_only=True
            )["surrogate_gate_passed"]
        )

    def test_power_arithmetic_and_cuped_are_game_clustered(self) -> None:
        # This reproduces the review's rough arithmetic: sd=.018 needs about
        # 640 games for a .002 effect and about 100 for a .005 effect.
        self.assertGreaterEqual(required_games_for_effect(0.018, 0.002), 630)
        self.assertLessEqual(required_games_for_effect(0.018, 0.002), 650)
        self.assertGreaterEqual(required_games_for_effect(0.018, 0.005), 100)
        self.assertLessEqual(required_games_for_effect(0.018, 0.005), 105)

        # With only one game, repeated replays cannot fit a covariate slope.
        source = io.StringIO(
            "game,turn,player,rate_profile,cost,regret,expected_regret,bag,"
            "spread,predicted_future_turns\n"
            "g,0,0,0,0,1,0,0,0,0\n"
            "g,0,0,0,1,0,1,0,0,0\n"
        )
        curves = read_turn_curves(source, 1.0)
        model = fit_model(
            [
                Label("a", 0.0, 0.0, 0, 0, 0),
                Label("b", 0.0, 0.0, 0, 0, 0),
                Label("c", 0.0, 0.0, 0, 0, 0),
            ]
        )
        result = replay_sequence(model, curves, 1.0, 1.0)
        assert result is not None
        self.assertEqual(
            fit_cuped_theta([result, result], divergent_only=True), 0.0
        )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import io
import unittest

from tools.backtest_rest_game_value_policy import (
    clustered_summary,
    replay_sequence,
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


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import unittest

from tools.fit_rest_game_value_model import (
    Label,
    fit_model,
    metrics,
    pava_nonincreasing,
    predict,
    select_curve,
    split_games,
)


class FitRestGameValueModelTest(unittest.TestCase):
    def test_pava_enforces_nonincreasing_curve(self) -> None:
        fitted = pava_nonincreasing([0.20, 0.10, 0.12, 0.05], [1, 1, 1, 1])
        self.assertEqual(fitted, [0.20, 0.11, 0.11, 0.05])

    def test_game_splits_are_disjoint_and_complete(self) -> None:
        games = [f"game-{index}" for index in range(10)]
        train, calibration, test = split_games(games, 7)
        self.assertFalse(train & calibration)
        self.assertFalse(train & test)
        self.assertFalse(calibration & test)
        self.assertEqual(train | calibration | test, set(games))

    def test_hierarchical_curve_is_monotone_and_predicts(self) -> None:
        labels: list[Label] = []
        for game_index in range(6):
            for budget, regret in ((0.0, 0.20), (10.0, 0.10), (20.0, 0.05)):
                labels.append(
                    Label(
                        game=f"game-{game_index}",
                        budget=budget,
                        regret=regret + 0.001 * game_index,
                        bag=40,
                        spread=0,
                        predicted_future_turns=4,
                    )
                )
        model = fit_model(labels, prior_strength=2.0)
        probe = Label("probe", 15.0, 0.0, 40, 0, 4)
        curve = select_curve(model, probe)
        self.assertTrue(
            all(
                earlier >= later
                for earlier, later in zip(curve.regrets, curve.regrets[1:])
            )
        )
        self.assertAlmostEqual(predict(model, probe), 0.0775)
        fitted_metrics = metrics(model, labels)
        self.assertLess(float(fitted_metrics["mae"]), 0.01)


if __name__ == "__main__":
    unittest.main()

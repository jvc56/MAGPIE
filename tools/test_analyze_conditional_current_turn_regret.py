#!/usr/bin/env python3

from __future__ import annotations

import unittest

from tools.analyze_conditional_current_turn_regret import (
    clustered_summary,
    matched_ablation,
    reliability_bins,
)


def prediction_row(
    game: str,
    target_nodes: int,
    predicted: float,
    actual: float,
) -> dict[str, str]:
    return {
        "source_index": f"{game}-{target_nodes}",
        "game": game,
        "width": "60",
        "target_nodes": str(target_nodes),
        "predicted_total_current_turn_regret": str(predicted),
        "total_current_turn_regret": str(actual),
    }


class AnalyzeConditionalCurrentTurnRegretTest(unittest.TestCase):
    def test_reliability_marks_zero_and_finite_underprediction(self) -> None:
        rows = [
            prediction_row("a", 1, 0.0, 0.001),
            prediction_row("b", 1, 0.001, 0.003),
        ]
        bins = reliability_bins(rows)
        self.assertTrue(bins[0]["underprediction_over_2x"])
        self.assertTrue(bins[1]["underprediction_over_2x"])

    def test_clustered_summary_uses_games_as_inference_units(self) -> None:
        rows = [
            {"game": "a", "value": "0"},
            {"game": "a", "value": "2"},
            {"game": "b", "value": "5"},
        ]
        summary = clustered_summary(rows, lambda row: float(row["value"]))
        self.assertEqual(summary["rows"], 3)
        self.assertEqual(summary["games"], 2)
        self.assertAlmostEqual(summary["game_weighted_mean"], 3.0)
        self.assertAlmostEqual(summary["row_weighted_mean"], 7.0 / 3.0)

    def test_ablation_join_is_order_independent(self) -> None:
        main = [
            prediction_row("a", 1, 0.003, 0.002),
            prediction_row("b", 2, 0.004, 0.006),
        ]
        ablated = [
            prediction_row("b", 2, 0.005, 0.006),
            prediction_row("a", 1, 0.002, 0.002),
        ]
        result = matched_ablation(main, ablated, bootstrap_samples=100)
        paired = result[
            "paired_absolute_error_delta_stability_minus_ablation"
        ]
        self.assertAlmostEqual(paired["game_weighted_mean"], 0.001)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import math
import unittest

import numpy as np

from analyze_regret_estimators import (
    METRICS,
    Panel,
    analyze_panel,
    clustered_summary,
    count_near_ties,
    expected_positive_normal,
    normal_draws,
    pairwise_expected_regret,
    positive_semidefinite,
    print_summary,
    summarize_draws,
)


class RegretEstimatorTest(unittest.TestCase):
    def test_joint_max_exposes_flat_multiarm_optimism(self) -> None:
        arm_count = 6
        means = np.zeros((arm_count, len(METRICS)))
        covariance = np.zeros((means.size, means.size))
        for arm in range(arm_count):
            covariance[arm * len(METRICS), arm * len(METRICS)] = 1.0
        covariance = positive_semidefinite(covariance)

        pairwise = pairwise_expected_regret(means, covariance, selected=0)
        normals = np.random.default_rng(123).standard_normal((200_000, means.size))
        joint = summarize_draws(
            normal_draws(means.reshape(-1), covariance, normals), selected=0
        )["predicted_utility_regret"]

        self.assertAlmostEqual(pairwise, 1.0 / math.sqrt(math.pi), places=6)
        self.assertGreater(joint, pairwise * 2.0)

    def test_expected_positive_normal_and_near_ties(self) -> None:
        self.assertAlmostEqual(
            expected_positive_normal(0.0, 1.0),
            1.0 / math.sqrt(2.0 * math.pi),
            places=12,
        )
        means = np.zeros((3, len(METRICS)))
        means[2, 0] = -10.0
        covariance = np.eye(means.size) * 0.01
        self.assertEqual(count_near_ties(means, covariance, selected=0), 1)

    def test_clustered_summary_uses_games_not_panels(self) -> None:
        rows = [
            {"game": 1, "value": 1.0},
            {"game": 1, "value": 3.0},
            {"game": 2, "value": 10.0},
        ]
        summary = clustered_summary(rows, "value")
        self.assertEqual(summary.rows, 3)
        self.assertEqual(summary.games, 2)
        self.assertEqual(summary.mean, 6.0)
        self.assertLess(summary.low, summary.mean)
        self.assertGreater(summary.high, summary.mean)

    def test_panel_reports_logged_and_reconstructed_estimators(self) -> None:
        fields = {
            "source_index": "7",
            "position": "7",
            "game": "3",
            "bag": "40",
            "plies": "4",
            "target_nodes": "1000",
            "risk_count": "2",
            "paired_rows": "4",
            "selected_rank": "0",
            "judge_best_rank": "1",
            "actual_utility_regret": ".02",
            "actual_win_regret": ".01",
            "actual_spread_regret": "2",
            "estimated_regret": ".004",
            "joint_estimated_regret": ".006",
            "near_tie_challengers": "1",
        }
        arms = {
            0: {
                "samples": "100",
                "utility_mean": ".50",
                "utility_variance": ".04",
                "win_mean": ".50",
                "win_variance": ".04",
                "spread_mean": "0",
                "spread_variance": "100",
                "judge_utility": ".50",
            },
            1: {
                "samples": "100",
                "utility_mean": ".49",
                "utility_variance": ".04",
                "win_mean": ".49",
                "win_variance": ".04",
                "spread_mean": "-1",
                "spread_variance": "100",
                "judge_utility": ".52",
            },
        }
        rows = {
            row: {
                0: (0.4 + row * 0.1, 0.4 + row * 0.1, float(row)),
                1: (0.39 + row * 0.1, 0.39 + row * 0.1, float(row) - 1),
            }
            for row in range(4)
        }
        result = analyze_panel(
            (7, 4, 1000),
            Panel(fields, arms, rows),
            draws=20_000,
            shrinkage=0.1,
            seed=99,
            paired_row_limit=None,
            correlation_override=None,
        )
        self.assertEqual(result["game"], 3)
        self.assertEqual(result["logged_near_tie_challengers"], 1)
        self.assertEqual(result["logged_estimated_regret"], 0.004)
        self.assertGreaterEqual(result["correlated_joint_minus_pairwise"], 0.0)

    def test_print_summary_declares_game_clustered_inference(self) -> None:
        base = {
            "actual_utility_regret": 0.01,
            "actual_win_regret": 0.01,
            "actual_spread_regret": 1.0,
            "median_pairwise_utility_correlation": 0.5,
            "median_correlated_to_independent_delta_se": 0.7,
            "independent_predicted_utility_regret": 0.012,
            "correlated_predicted_utility_regret": 0.011,
            "independent_predicted_utility_p90": 0.02,
            "correlated_predicted_utility_p90": 0.02,
            "independent_predicted_utility_p95": 0.03,
            "correlated_predicted_utility_p95": 0.03,
            "independent_predicted_win_error": 0.011,
            "correlated_predicted_win_error": 0.010,
            "independent_predicted_spread_error": 1.1,
            "correlated_predicted_spread_error": 1.0,
            "independent_pairwise_predicted_utility_regret": 0.008,
            "correlated_pairwise_predicted_utility_regret": 0.007,
            "independent_joint_minus_pairwise": 0.004,
            "correlated_joint_minus_pairwise": 0.004,
            "independent_near_tie_challengers": 2,
            "correlated_near_tie_challengers": 2,
            "independent_pair_nll": 1.0,
            "correlated_pair_nll": 0.8,
            "independent_pair_mean_squared_z": 1.2,
            "correlated_pair_mean_squared_z": 1.0,
            "independent_pair_mean_absolute_z": 0.9,
            "correlated_pair_mean_absolute_z": 0.8,
            "bag": 30,
        }
        results = [dict(base, game=1), dict(base, game=2)]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            print_summary(results)
        rendered = output.getvalue()
        self.assertIn("games=2", rendered)
        self.assertIn("game_clustered_t_ci95", rendered)
        self.assertIn("REGRET_MAX_NEAR_TIE_STRATUM near_ties=2-3", rendered)


if __name__ == "__main__":
    unittest.main()

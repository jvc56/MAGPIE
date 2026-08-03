#!/usr/bin/env python3

from __future__ import annotations

import unittest

from tools.analyze_rule_zero_panel import clopper_pearson_upper, mean_interval


class AnalyzeRuleZeroPanelTest(unittest.TestCase):
    def test_zero_event_upper_bound_motivates_320_roots(self) -> None:
        self.assertAlmostEqual(
            clopper_pearson_upper(0, 320), 1.0 - 0.05 ** (1.0 / 320), places=12
        )
        self.assertLess(clopper_pearson_upper(0, 320), 0.015)
        self.assertGreater(clopper_pearson_upper(0, 96), 0.015)

    def test_mean_interval_is_game_level_student_t(self) -> None:
        summary = mean_interval([0.0, 0.001, -0.001, 0.0])
        self.assertEqual(summary["observations"], 4)
        self.assertAlmostEqual(float(summary["mean"]), 0.0)
        self.assertLess(float(summary["ci95"][0]), 0.0)
        self.assertGreater(float(summary["ci95"][1]), 0.0)


if __name__ == "__main__":
    unittest.main()

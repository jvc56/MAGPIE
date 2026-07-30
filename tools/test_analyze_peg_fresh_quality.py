#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from analyze_peg_fresh_quality import (  # noqa: E402
    bootstrap_estimate,
    build_comparison_rows,
    correction_by_bag,
)


class AnalyzePegFreshQualityTest(unittest.TestCase):
    def test_policy_agreement_is_exact_zero_without_oracle(self):
        quality_map = {
            "position": "p",
            "bag": 1,
            "nominees": ["A"],
            "policies": {
                "before": {"move": "A"},
                "after": {"move": "A"},
            },
        }
        rows = build_comparison_rows(
            [quality_map], {}, {}, "before", "after"
        )
        self.assertFalse(rows[0]["disagree"])
        self.assertEqual(rows[0]["direct3"]["utility"], 0.0)
        self.assertEqual(rows[0]["direct4"]["utility"], 0.0)

    def test_bag_correction_falls_back_to_pooled_mean(self):
        rows = [
            {
                "bag": 1,
                "disagree": True,
                "direct3": {"utility": 0.1},
                "direct4": {"utility": 0.3},
            }
        ]
        means, samples, pooled = correction_by_bag(rows, "utility")
        self.assertAlmostEqual(pooled, 0.2)
        self.assertAlmostEqual(means[1], 0.2)
        self.assertAlmostEqual(means[4], 0.2)
        self.assertEqual(samples[2], [])

    def test_two_stage_bootstrap_applies_correction_only_to_disagreements(self):
        rows = []
        for bag in range(1, 5):
            rows.extend(
                [
                    {
                        "bag": bag,
                        "disagree": True,
                        "direct3": {"utility": 0.1},
                        "direct4": {
                            "utility": 0.2 if bag == 1 else None
                        },
                    },
                    {
                        "bag": bag,
                        "disagree": False,
                        "direct3": {"utility": 0.0},
                        "direct4": {"utility": 0.0},
                    },
                ]
            )
        point, bootstraps, source = bootstrap_estimate(
            rows,
            "utility",
            replicates=20,
            seed_text="test",
        )
        self.assertEqual(source, "model_direct3_plus_direct4_correction")
        self.assertAlmostEqual(point, 0.1)
        self.assertEqual(len(bootstraps), 20)


if __name__ == "__main__":
    unittest.main()

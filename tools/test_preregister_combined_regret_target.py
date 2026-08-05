#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
from pathlib import Path
import tempfile
import unittest

from tools.preregister_combined_regret_target import (
    nearest_grid,
    parse_grid,
    preregister,
)


class PreregisterCombinedRegretTargetTest(unittest.TestCase):
    def test_freezes_median_width_prediction_on_declared_grid(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            predictions = root / "predictions.csv"
            with predictions.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(
                    stream,
                    fieldnames=(
                        "width",
                        "game",
                        "predicted_total_current_turn_regret",
                    ),
                )
                writer.writeheader()
                writer.writerows(
                    (
                        {
                            "width": 60,
                            "game": "a",
                            "predicted_total_current_turn_regret": 8.0e-5,
                        },
                        {
                            "width": 60,
                            "game": "b",
                            "predicted_total_current_turn_regret": 1.2e-4,
                        },
                        {
                            "width": 40,
                            "game": "c",
                            "predicted_total_current_turn_regret": 0.1,
                        },
                    )
                )
            output = root / "target.json"
            document = preregister(
                predictions,
                output,
                width=60,
                quantile=0.5,
                grid=(5.0e-5, 1.0e-4, 2.5e-4),
            )
            self.assertEqual(document["rows"], 2)
            self.assertEqual(document["games"], 2)
            self.assertAlmostEqual(document["prediction_quantile"], 1.0e-4)
            self.assertEqual(document["combined_regret_stop_target"], 1.0e-4)
            self.assertFalse(document["prospective_judge_outcomes_used"])
            self.assertEqual(json.loads(output.read_text()), document)

    def test_zero_prediction_uses_smallest_positive_target(self) -> None:
        self.assertEqual(nearest_grid(0.0, (1.0e-5, 1.0e-4)), 1.0e-5)

    def test_grid_must_be_increasing_and_unique(self) -> None:
        with self.assertRaises(ValueError):
            parse_grid("0.001,0.0001")
        with self.assertRaises(ValueError):
            parse_grid("0.001,0.001")


if __name__ == "__main__":
    unittest.main()

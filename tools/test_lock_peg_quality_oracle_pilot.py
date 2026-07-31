#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from lock_peg_quality_oracle_pilot import (  # noqa: E402
    select_all_disagreements,
    select_pilot,
)


class LockPegQualityOraclePilotTest(unittest.TestCase):
    def test_selection_is_balanced_deterministic_and_disagreements_only(self):
        maps = [
            {
                "position": f"b{bag}-p{index}",
                "bag": bag,
                "distinct_nominees": 2 if index < 4 else 1,
            }
            for bag in range(1, 5)
            for index in range(5)
        ]
        first = select_pilot(maps, 2, "seed")
        second = select_pilot(list(reversed(maps)), 2, "seed")
        self.assertEqual(
            [row["position"] for row in first],
            [row["position"] for row in second],
        )
        self.assertEqual(
            {
                bag: sum(row["bag"] == bag for row in first)
                for bag in range(1, 5)
            },
            {1: 2, 2: 2, 3: 2, 4: 2},
        )
        self.assertTrue(
            all(row["distinct_nominees"] > 1 for row in first)
        )

    def test_all_disagreements_is_complete_and_stable(self):
        maps = [
            {
                "position": f"b{bag}-p{index}",
                "bag": bag,
                "distinct_nominees": 1 + (index % 2),
            }
            for bag in range(1, 5)
            for index in range(4)
        ]
        selected = select_all_disagreements(list(reversed(maps)))
        self.assertEqual(
            [row["position"] for row in selected],
            [
                "b1-p1",
                "b1-p3",
                "b2-p1",
                "b2-p3",
                "b3-p1",
                "b3-p3",
                "b4-p1",
                "b4-p3",
            ],
        )


if __name__ == "__main__":
    unittest.main()

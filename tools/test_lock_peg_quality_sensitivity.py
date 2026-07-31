#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from lock_peg_quality_sensitivity import select_sensitivity  # noqa: E402


class LockPegQualitySensitivityTest(unittest.TestCase):
    def test_selection_is_balanced_deterministic(self):
        rows = [
            {"position": f"b{bag}-p{index}", "bag": bag}
            for bag in range(1, 5)
            for index in range(5)
        ]
        first = select_sensitivity(rows, 2, "seed")
        second = select_sensitivity(list(reversed(rows)), 2, "seed")
        self.assertEqual(
            [row["position"] for row in first],
            [row["position"] for row in second],
        )
        self.assertEqual(
            {
                bag: sum(int(row["bag"]) == bag for row in first)
                for bag in range(1, 5)
            },
            {1: 2, 2: 2, 3: 2, 4: 2},
        )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from analyze_peg_quality_oracle_pilot import (  # noqa: E402
    best_move,
    rank_pair_agreement,
)


class AnalyzePegQualityOraclePilotTest(unittest.TestCase):
    def test_best_move_uses_utility(self):
        values = {
            "A": {"utility": 0.5, "win": 0.5, "spread": 0.0},
            "B": {"utility": 0.6, "win": 0.4, "spread": 0.0},
        }
        self.assertEqual(best_move(values), "B")

    def test_rank_pair_agreement_counts_flip(self):
        first = {
            "A": {"utility": 0.6},
            "B": {"utility": 0.5},
            "C": {"utility": 0.4},
        }
        second = {
            "A": {"utility": 0.4},
            "B": {"utility": 0.5},
            "C": {"utility": 0.6},
        }
        result = rank_pair_agreement(first, second)
        self.assertEqual(result["discordant"], 3)
        self.assertEqual(result["concordant"], 0)


if __name__ == "__main__":
    unittest.main()

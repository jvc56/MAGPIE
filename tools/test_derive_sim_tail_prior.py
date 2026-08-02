from __future__ import annotations

import unittest

from tools.derive_sim_tail_prior import summarize


class DeriveSimTailPriorTest(unittest.TestCase):
    def test_clustered_monotone_scenarios(self) -> None:
        points = {
            (6, 1, 300): 0.4,
            (6, 1, 500): 0.3,
            (6, 1, 1000): 0.2,
            (6, 2, 300): 0.2,
            (6, 2, 500): 0.3,
            (6, 2, 1000): 0.1,
            (6, 3, 300): 0.3,
            (6, 3, 500): 0.2,
            (6, 3, 1000): 0.1,
        }
        games = {(6, 1): "a", (6, 2): "a", (6, 3): "b"}
        result = summarize(points, games, 6, 300, (500, 1000))
        self.assertEqual(result["positions"], 3)
        self.assertEqual(result["source_games"], 2)
        targets = result["targets"]
        self.assertLessEqual(
            targets[1]["central_retention"], targets[0]["central_retention"]
        )
        self.assertLessEqual(
            targets[1]["lower95_retention"], targets[0]["lower95_retention"]
        )


if __name__ == "__main__":
    unittest.main()

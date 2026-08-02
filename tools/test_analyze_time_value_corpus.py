#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.analyze_time_value_corpus import analyze


class AnalyzeTimeValueCorpusTest(unittest.TestCase):
    def test_compares_only_with_later_same_player_turns(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "positions"
            rows = []
            for turn, player, predicted in ((1, 0, 1), (2, 1, 0), (3, 0, 0)):
                rows.append(
                    "TIME_VALUE_POSITION "
                    f"position={turn - 1} game=0 source_seed=1 start=0 "
                    f"turn={turn} player={player} bag={90 - turn} "
                    "own_rack=7 opp_rack=7 spread=0 "
                    f"predicted_future_turns={predicted} "
                    "trajectory_policy=static cgp=15 A/B 0/0 0\n"
                )
            path.write_text("".join(rows), encoding="utf-8")
            result = analyze(path)
        self.assertEqual(result["games"], 1)
        self.assertEqual(result["positions"], 3)
        self.assertEqual(result["future_turn_prediction"]["mae"], 0.0)


if __name__ == "__main__":
    unittest.main()

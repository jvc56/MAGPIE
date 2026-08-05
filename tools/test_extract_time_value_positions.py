#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("extract_time_value_positions.py")
SPEC = importlib.util.spec_from_file_location("extract_time_value_positions", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ExtractTimeValuePositionsTest(unittest.TestCase):
    def write_log(self, text: str) -> Path:
        temporary = tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", suffix=".log", delete=False
        )
        with temporary:
            temporary.write(text)
        self.addCleanup(Path(temporary.name).unlink)
        return Path(temporary.name)

    def test_reads_only_reconciled_complete_game(self) -> None:
        path = self.write_log(
            'PCTURN game=0 seed=17 turn=1 start=0 player=0 bag=86 '
            'own_rack=7 opp_rack=7 spread_before=0 move="8h TEST" '
            'cgp="15/15 AAAAAAA/BBBBBBB 0/0 0"\n'
            'PCTURN game=0 seed=17 turn=2 start=0 player=1 bag=80 '
            'own_rack=7 opp_rack=7 spread_before=-20 move="pass" '
            'cgp="15/15 CCCCCCC/DDDDDDD 20/0 0"\n'
            'PCGAME seed=17 start=0 turns=2 p0_score=20 p1_score=0\n'
        )
        games = MODULE.read_complete_games([path])
        self.assertEqual(len(games), 1)
        self.assertEqual([position.turn for position in games[0]], [1, 2])
        self.assertEqual(games[0][0].cgp, "15/15 AAAAAAA/BBBBBBB 0/0 0")

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "positions.txt"
            self.assertEqual(
                MODULE.write_corpus(games, output, "static"), 2
            )
            rows = output.read_text(encoding="utf-8").splitlines()
        # This estimate is available from the current position. In
        # particular, the corpus must not expose the realized number of later
        # turns, which would leak the future into the learned value model.
        self.assertIn("predicted_future_turns=12", rows[0])
        self.assertIn("trajectory_policy=static", rows[0])
        self.assertIn("predicted_future_turns=11", rows[1])

    def test_rejects_truncated_game(self) -> None:
        path = self.write_log(
            'PCTURN game=0 seed=19 turn=1 start=0 player=0 bag=86 '
            'own_rack=1 opp_rack=1 spread_before=0 cgp="15 A/B 0/0 0"\n'
            'PCGAME seed=19 start=0 turns=2\n'
        )
        with self.assertRaisesRegex(ValueError, "incomplete"):
            MODULE.read_complete_games([path])


if __name__ == "__main__":
    unittest.main()

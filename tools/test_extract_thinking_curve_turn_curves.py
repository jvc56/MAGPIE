#!/usr/bin/env python3

from __future__ import annotations

import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ExtractThinkingCurveTurnCurvesTest(unittest.TestCase):
    def test_uses_stable_source_game_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "corpus.log"
            trace = root / "trace.log"
            output = root / "curves.csv"
            corpus.write_text(
                "TIME_VALUE_POSITION position=0 game=0 source_seed=99 start=1 "
                "turn=3 player=1 bag=50 own_rack=7 opp_rack=6 spread=-12 "
                "predicted_future_turns=7 cgp=15 A/B 0/0 0\n",
                encoding="utf-8",
            )
            trace.write_text(
                "THINKING_CURVE_POINT position=0 plies=2 final=0 "
                "target_nodes=100 nodes=99 judge_regret=0.01 candidates=15\n",
                encoding="utf-8",
            )
            subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name(
                        "extract_thinking_curve_turn_curves.py"
                    )),
                    str(corpus),
                    str(trace),
                    "--plies",
                    "2",
                    "--output",
                    str(output),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["game"], "99:1")
        self.assertEqual(rows[0]["predicted_future_turns"], "7")
        self.assertEqual(rows[0]["expected_regret"], "")

    def test_attaches_cross_fitted_regret_without_replacing_score(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "corpus.log"
            trace = root / "trace.log"
            predictions = root / "predictions.csv"
            output = root / "curves.csv"
            corpus.write_text(
                "TIME_VALUE_POSITION position=0 game=0 source_seed=99 start=1 "
                "turn=3 player=1 bag=50 own_rack=7 opp_rack=6 spread=-12 "
                "predicted_future_turns=7 cgp=15 A/B 0/0 0\n",
                encoding="utf-8",
            )
            trace.write_text(
                "THINKING_CURVE_POINT position=0 plies=4 final=0 "
                "target_nodes=100 nodes=99 judge_regret=0.01 candidates=15\n",
                encoding="utf-8",
            )
            predictions.write_text(
                "position,plies,target_nodes,actual_regret,state_expected_regret\n"
                "0,4,100,0.01,0.02\n",
                encoding="utf-8",
            )
            subprocess.run(
                [
                    sys.executable,
                    str(
                        Path(__file__).with_name(
                            "extract_thinking_curve_turn_curves.py"
                        )
                    ),
                    str(corpus),
                    str(trace),
                    "--plies",
                    "4",
                    "--regret-predictions",
                    str(predictions),
                    "--output",
                    str(output),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
        self.assertEqual(rows[0]["regret"], "0.01")
        self.assertEqual(rows[0]["expected_regret"], "0.02")


if __name__ == "__main__":
    unittest.main()

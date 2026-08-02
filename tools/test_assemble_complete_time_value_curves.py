#!/usr/bin/env python3

from __future__ import annotations

import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HEADER = "game,turn,player,cost,regret,mode,plies,option\n"


class AssembleCompleteCurvesTest(unittest.TestCase):
    def test_filters_games_and_checks_complete_root_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            eligibility = root / "eligibility.csv"
            corpus = root / "corpus.log"
            sim = root / "sim.csv"
            endgame = root / "endgame.csv"
            output = root / "output.csv"
            eligibility.write_text(
                "game,eligible\n1:0,1\n2:0,0\n", encoding="utf-8"
            )
            corpus.write_text(
                "TIME_VALUE_POSITION position=0 source_seed=1 start=0 "
                "turn=1 player=0 bag=5\n"
                "TIME_VALUE_POSITION position=1 source_seed=1 start=0 "
                "turn=2 player=1 bag=0\n",
                encoding="utf-8",
            )
            sim.write_text(
                HEADER
                + "1:0,1,0,1,0.1,sim,2,1\n"
                + "2:0,1,0,1,0.2,sim,2,1\n",
                encoding="utf-8",
            )
            endgame.write_text(
                HEADER + "1:0,2,1,1,0.0,endgame,1,1\n",
                encoding="utf-8",
            )
            subprocess.run(
                [
                    sys.executable,
                    str(
                        Path(__file__).with_name(
                            "assemble_complete_time_value_curves.py"
                        )
                    ),
                    str(sim),
                    str(endgame),
                    "--eligibility",
                    str(eligibility),
                    "--source-corpus",
                    str(corpus),
                    "--output",
                    str(output),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 2)
        self.assertEqual({row["game"] for row in rows}, {"1:0"})


if __name__ == "__main__":
    unittest.main()

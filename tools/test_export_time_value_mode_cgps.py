#!/usr/bin/env python3

from __future__ import annotations

import csv
from pathlib import Path
import tempfile
import unittest

from tools.export_time_value_mode_cgps import export_modes


class ExportTimeValueModeCgpsTest(unittest.TestCase):
    def test_exports_cgps_and_source_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "panel.positions"
            corpus.write_text(
                "TIME_VALUE_POSITION position=0 game=0 source_seed=7 start=1 "
                "turn=1 player=1 bag=4 own_rack=7 opp_rack=7 spread=3 "
                "predicted_future_turns=1 trajectory_policy=static "
                "cgp=15 A/B 3/0 0\n"
                "TIME_VALUE_POSITION position=1 game=0 source_seed=7 start=1 "
                "turn=2 player=0 bag=0 own_rack=5 opp_rack=4 spread=-2 "
                "predicted_future_turns=0 trajectory_policy=static "
                "cgp=14A A/B 3/5 1\n",
                encoding="utf-8",
            )
            output = root / "modes"
            manifest = export_modes(corpus, output)
            peg = (output / "random_4peg.txt").read_text()
            endgame = (output / "endgame-cgps.txt").read_text()
            with (output / "position-map.csv").open(newline="") as stream:
                mappings = list(csv.DictReader(stream))

        self.assertEqual(manifest["positions"], 2)
        self.assertEqual(peg, "15 A/B 3/0 0\n")
        self.assertEqual(endgame, "14A A/B 3/5 1\n")
        self.assertEqual(mappings[0]["source_position"], "1")
        self.assertEqual(mappings[1]["source_position"], "0")
        self.assertEqual(mappings[1]["mode_index"], "0")


if __name__ == "__main__":
    unittest.main()

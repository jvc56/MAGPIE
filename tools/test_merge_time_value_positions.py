#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("merge_time_value_positions.py")
SPEC = importlib.util.spec_from_file_location("merge_time_value_positions", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class MergeTimeValuePositionsTest(unittest.TestCase):
    def test_reindexes_positions_and_games(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.positions"
            second = root / "second.positions"
            output = root / "merged.positions"
            manifest = root / "manifest.json"
            template = (
                "TIME_VALUE_POSITION position=0 game=0 source_seed={seed} "
                "start=0 turn=1 player=0 bag={bag} own_rack=7 opp_rack=7 "
                "spread=0 predicted_future_turns=1 "
                "trajectory_policy={policy} cgp=15 A/B 0/0 0\n"
            )
            first.write_text(
                template.format(seed=1, bag=86, policy="static"),
                encoding="utf-8",
            )
            second.write_text(
                template.format(seed=2, bag=4, policy="playchooser-g3s"),
                encoding="utf-8",
            )
            document = MODULE.merge([first, second], output, manifest)
            rows = output.read_text(encoding="utf-8").splitlines()
            persisted = json.loads(manifest.read_text(encoding="utf-8"))
        self.assertIn("position=0 game=0", rows[0])
        self.assertIn("position=1 game=1", rows[1])
        self.assertEqual(document["games"], 2)
        self.assertEqual(persisted["positions_by_bag_band"]["peg"], 1)


if __name__ == "__main__":
    unittest.main()

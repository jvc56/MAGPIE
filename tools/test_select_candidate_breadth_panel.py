#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.extract_time_value_positions import fields
from tools.select_candidate_breadth_panel import select_candidate_breadth_panel


class SelectCandidateBreadthPanelTest(unittest.TestCase):
    def test_selects_balanced_outcome_free_complement(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "source.positions"
            source_manifest = root / "source.json"
            exclude_manifest = root / "exclude.json"
            lines: list[str] = []
            mapping: list[dict[str, object]] = []
            excluded: list[dict[str, int]] = []
            for position in range(16):
                policy = "static" if position < 8 else "playchooser-g3000ms"
                band = "early" if position % 8 < 4 else "late"
                bag = 70 if band == "early" else 10
                lines.append(
                    "TIME_VALUE_POSITION "
                    f"position={position} game={position} source_seed={1000 + position} "
                    f"start={position % 2} turn=2 player=0 bag={bag} own_rack=7 "
                    "opp_rack=7 spread=0 predicted_future_turns=3 "
                    f"trajectory_policy={policy} cgp=15 A/B 0/0 0\n"
                )
                mapping.append(
                    {
                        "position": position,
                        "panel_position": 100 + position,
                        "trajectory_policy": policy,
                        "bag_band": band,
                    }
                )
                if position % 4 == 0:
                    excluded.append({"selection_source_index": position})
            corpus.write_text("".join(lines), encoding="utf-8")
            source_manifest.write_text(
                json.dumps({"mapping": mapping}), encoding="utf-8"
            )
            exclude_manifest.write_text(
                json.dumps({"mapping": excluded}), encoding="utf-8"
            )
            output = root / "selected.positions"
            result = select_candidate_breadth_panel(
                corpus,
                source_manifest,
                exclude_manifest,
                output,
                root / "selected.json",
                per_stratum=2,
                seed=7,
            )
            rows = [fields(line) for line in output.read_text().splitlines()]

        self.assertEqual(result["roots"], 8)
        self.assertEqual(result["source_games"], 8)
        self.assertEqual(set(result["strata_selected"].values()), {2})
        self.assertEqual([int(row["position"]) for row in rows], list(range(8)))
        self.assertTrue(all("breadth_source_index" in row for row in rows))
        self.assertFalse(result["oracle_fields_used"])


if __name__ == "__main__":
    unittest.main()

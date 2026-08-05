#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.extract_time_value_positions import fields
from tools.select_candidate_miss_panel import select_candidate_miss_panel


class SelectCandidateMissPanelTest(unittest.TestCase):
    def test_balances_checkpoint_hardness_without_oracle_selection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "source.positions"
            manifest_path = root / "source.json"
            log = root / "curve.log"
            lines: list[str] = []
            mapping: list[dict[str, object]] = []
            log_lines: list[str] = []
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
                log_lines.append(
                    "THINKING_CURVE_POINT "
                    f"source_index={position} position={position} game={position} "
                    f"bag={bag} plies=6 candidates=15 arm_plays=15 "
                    "target_nodes=300 final=0 control=0 control_kind=stopped "
                    f"iterations={20 + position} nodes={100 + position} "
                    f"near_tie_challengers_at_stop={position % 4} "
                    f"joint_regret_at_stop={position / 1000:.6f} "
                    f"bai_status={3 if position % 4 == 3 else 4} "
                    "judge_regret=999\n"
                )
            corpus.write_text("".join(lines), encoding="utf-8")
            manifest_path.write_text(json.dumps({"mapping": mapping}), encoding="utf-8")
            log.write_text("".join(log_lines), encoding="utf-8")
            output = root / "selected.positions"
            selected_manifest = select_candidate_miss_panel(
                corpus,
                log,
                manifest_path,
                output,
                root / "selected.json",
                per_stratum=2,
                seed=7,
            )
            rows = [fields(line) for line in output.read_text().splitlines()]

        self.assertEqual(selected_manifest["roots"], 8)
        self.assertEqual(set(selected_manifest["strata_selected"].values()), {2})
        self.assertEqual([int(row["position"]) for row in rows], list(range(8)))
        self.assertTrue(all("selection_source_index" in row for row in rows))
        self.assertFalse(selected_manifest["oracle_fields_used"])


if __name__ == "__main__":
    unittest.main()

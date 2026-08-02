#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.extract_time_value_positions import fields
from tools.select_matched_sim_tail_panel import BAG_BANDS, select_tail_panel


class SelectMatchedSimTailPanelTest(unittest.TestCase):
    def test_balances_policy_and_bag_with_unique_games(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.positions"
            lines: list[str] = []
            position = 0
            bags = [minimum for _, minimum, _ in BAG_BANDS]
            for game in range(24):
                policy = "static" if game < 12 else "playchooser-g3000ms"
                for turn, bag in enumerate(bags, start=1):
                    lines.append(
                        "TIME_VALUE_POSITION "
                        f"position={position} game={game} source_seed={1000 + game} "
                        f"start={game % 2} turn={turn} player={turn % 2} bag={bag} "
                        "own_rack=7 opp_rack=7 spread=0 predicted_future_turns=2 "
                        f"trajectory_policy={policy} cgp=15 A/B 0/0 0\n"
                    )
                    position += 1
            source.write_text("".join(lines), encoding="utf-8")
            output = root / "tail.positions"
            manifest_path = root / "manifest.json"
            manifest = select_tail_panel(
                source,
                output,
                manifest_path,
                {"static": 8, "playchooser-g3000ms": 8},
                11,
            )
            output_rows = [fields(line) for line in output.read_text().splitlines()]
            on_disk = json.loads(manifest_path.read_text())

        self.assertEqual(manifest["roots"], 16)
        self.assertEqual(manifest["source_games"], 16)
        self.assertEqual(
            [int(row["position"]) for row in output_rows], list(range(16))
        )
        self.assertEqual(
            len({(row["source_seed"], row["start"]) for row in output_rows}), 16
        )
        self.assertTrue(all("panel_position" in row for row in output_rows))
        self.assertEqual(set(manifest["strata_selected"].values()), {2})
        self.assertEqual(on_disk["mapping"], manifest["mapping"])

    def test_excludes_games_from_prior_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.positions"
            lines: list[str] = []
            position = 0
            for game in range(12):
                for turn, (_, minimum, _) in enumerate(BAG_BANDS, start=1):
                    lines.append(
                        "TIME_VALUE_POSITION "
                        f"position={position} game={game} "
                        f"source_seed={1000 + game} start=0 turn={turn} "
                        f"player=0 bag={minimum} own_rack=7 opp_rack=7 "
                        "spread=0 predicted_future_turns=2 "
                        "trajectory_policy=static cgp=15 A/B 0/0 0\n"
                    )
                    position += 1
            source.write_text("".join(lines), encoding="utf-8")
            exclusion = root / "old.json"
            exclusion.write_text(
                json.dumps(
                    {
                        "mapping": [
                            {"source_seed": str(1000 + game), "start": "0"}
                            for game in range(4)
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output = root / "tail.positions"
            manifest = select_tail_panel(
                source,
                output,
                root / "manifest.json",
                {"static": 4},
                7,
                [exclusion],
            )

        self.assertEqual(manifest["excluded_source_games"], 4)
        self.assertTrue(
            all(int(row["source_seed"]) >= 1004 for row in manifest["mapping"])
        )


if __name__ == "__main__":
    unittest.main()

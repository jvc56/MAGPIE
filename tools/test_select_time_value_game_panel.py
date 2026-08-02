#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.extract_time_value_positions import fields
from tools.select_time_value_game_panel import select_panel


class SelectTimeValueGamePanelTest(unittest.TestCase):
    def test_selects_only_complete_games_and_reindexes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.positions"
            lines: list[str] = []
            source_position = 0
            for game in range(8):
                policy = "static" if game < 4 else "playchooser-g3000ms"
                turns = 20 + game % 4
                for turn in range(1, turns + 1):
                    bag = max(0, 90 - 5 * turn)
                    lines.append(
                        "TIME_VALUE_POSITION "
                        f"position={source_position} game={game} "
                        f"source_seed={1000 + game} start={game % 2} "
                        f"turn={turn} player={turn % 2} bag={bag} "
                        "own_rack=7 opp_rack=7 spread=0 "
                        "predicted_future_turns=1 "
                        f"trajectory_policy={policy} cgp=15 A/B 0/0 0\n"
                    )
                    source_position += 1
            source.write_text("".join(lines), encoding="utf-8")
            output = root / "panel.positions"
            manifest_path = root / "manifest.json"
            manifest = select_panel(
                source,
                output,
                manifest_path,
                {"static": 2, "playchooser-g3000ms": 2},
                7,
            )
            rows = [fields(line) for line in output.read_text().splitlines()]
            manifest_on_disk = json.loads(manifest_path.read_text())

        self.assertEqual(manifest["games"], 4)
        self.assertEqual(len({(row["source_seed"], row["start"]) for row in rows}), 4)
        self.assertEqual([int(row["position"]) for row in rows], list(range(len(rows))))
        by_game: dict[int, list[int]] = {}
        for row in rows:
            by_game.setdefault(int(row["game"]), []).append(int(row["turn"]))
        self.assertEqual(sorted(by_game), [0, 1, 2, 3])
        for turns in by_game.values():
            self.assertEqual(turns, list(range(1, len(turns) + 1)))
        self.assertEqual(manifest_on_disk["games"], 4)


if __name__ == "__main__":
    unittest.main()

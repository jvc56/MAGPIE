#!/usr/bin/env python3

from __future__ import annotations

from collections import defaultdict
import csv
from pathlib import Path
import tempfile
import unittest

from tools.fit_mode_boundary_regret import (
    cross_fit,
    read_boundaries,
    write_curves,
)


class FitModeBoundaryRegretTest(unittest.TestCase):
    def test_predictions_are_game_heldout_and_monotone(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "curves.csv"
            output = root / "predicted.csv"
            rows = [
                "game,turn,player,mode,option,bag,spread,own_rack,opp_rack,"
                "candidates,regret"
            ]
            for game in range(20):
                for option, regret in ((3, 0.03), (4, 0.01), (5, 0.0)):
                    rows.append(
                        f"g{game},1,0,endgame,{option},0,0,7,7,20,"
                        f"{regret + 0.001 * (game % 3)}"
                    )
            source.write_text("\n".join(rows) + "\n", encoding="utf-8")
            raw, boundaries = read_boundaries(source)
            predictions = cross_fit(boundaries, 5, 19, 5.0)
            write_curves(output, raw, predictions)
            with output.open(newline="", encoding="utf-8") as stream:
                predicted_rows = list(csv.DictReader(stream))

        self.assertEqual(len(predictions), 60)
        self.assertTrue(all(item.expected_regret >= 0.0 for item in predictions))
        by_game: dict[str, list[tuple[int, float]]] = defaultdict(list)
        for item in predictions:
            by_game[item.boundary.game].append(
                (item.boundary.option, item.expected_regret)
            )
        for curve in by_game.values():
            curve.sort()
            self.assertGreaterEqual(curve[0][1], curve[1][1])
            self.assertGreaterEqual(curve[1][1], curve[2][1])
        self.assertTrue(all(row["expected_regret"] for row in predicted_rows))


if __name__ == "__main__":
    unittest.main()

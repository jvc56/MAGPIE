from __future__ import annotations

import unittest

from tools.extend_sim_curves_with_tail import extend_rows


def row(option: str, regret: str, candidates: str = "15") -> dict[str, str]:
    return {
        "game": "g",
        "turn": "1",
        "player": "0",
        "mode": "sim",
        "plies": "6",
        "option": option,
        "cost": option if option != "forced" else "0",
        "nodes": option if option != "forced" else "0",
        "observed_seconds": "1",
        "regret": regret,
        "candidates": candidates,
        "trajectory_scope": "measured",
    }


class ExtendSimCurvesWithTailTest(unittest.TestCase):
    def test_extends_from_baseline(self) -> None:
        output, count = extend_rows(
            [row("50000", "0.3"), row("300000", "0.2")],
            300000,
            {6: [(1000000, 0.5), (3000000, 0.25)]},
            "lower95",
        )
        self.assertEqual(count, 2)
        added = {
            item["option"]: item
            for item in output
            if item.get("tail_imputed") == "1"
        }
        self.assertEqual(float(added["1000000"]["regret"]), 0.1)
        self.assertEqual(float(added["3000000"]["regret"]), 0.05)
        self.assertEqual(added["1000000"]["nodes"], "1000000")
        self.assertEqual(added["1000000"]["tail_scenario"], "lower95")

    def test_forced_curve_is_not_extended(self) -> None:
        output, count = extend_rows(
            [row("forced", "0", "1")],
            300000,
            {6: [(1000000, 0.5)]},
            "central",
        )
        self.assertEqual(count, 0)
        self.assertEqual(len(output), 1)


if __name__ == "__main__":
    unittest.main()

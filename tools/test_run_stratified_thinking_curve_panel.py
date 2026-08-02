#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.run_stratified_thinking_curve_panel import (
    completed_by_plies,
    eligible_indices,
    validate_prefix,
)


class RunStratifiedThinkingCurvePanelTest(unittest.TestCase):
    def test_eligible_indices_and_completed_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "panel.positions"
            corpus.write_text(
                "".join(
                    "TIME_VALUE_POSITION "
                    f"position={index} game=0 source_seed=1 start=0 "
                    f"turn={index + 1} player={index % 2} bag={bag} "
                    "own_rack=7 opp_rack=7 spread=0 predicted_future_turns=1 "
                    "trajectory_policy=static cgp=15 A/B 0/0 0\n"
                    for index, bag in enumerate((86, 12, 4, 0))
                ),
                encoding="utf-8",
            )
            log = root / "curve.log"
            log.write_text(
                "THINKING_CURVE_POSITION_DONE source_index=0 position=0 "
                "plies=2 events=8 dropped=0 final_iterations=1 final_nodes=3\n"
                "THINKING_CURVE_FORCED_POSITION source_index=1 position=1 "
                "game=0 bag=12 plies=2 candidates=1 regret=0 cgp=x\n",
                encoding="utf-8",
            )
            eligible = eligible_indices(corpus, 5, 100)
            completed = completed_by_plies(log)[2]
        self.assertEqual(eligible, [0, 1])
        self.assertEqual(completed, {0, 1})
        self.assertEqual(validate_prefix(eligible, completed), 2)

    def test_rejects_nonprefix_completion(self) -> None:
        with self.assertRaises(ValueError):
            validate_prefix([0, 2, 4], {0, 4})


if __name__ == "__main__":
    unittest.main()

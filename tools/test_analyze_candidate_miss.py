#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.analyze_candidate_miss import analyze, load_pairs


class AnalyzeCandidateMissTest(unittest.TestCase):
    def test_common_judge_gain_and_outside_event(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "curve.log"
            path.write_text(
                "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
                "plies=6 candidates=40 arm_plays=40 target_nodes=300 final=0 "
                "control=0 control_kind=stopped iterations=42 nodes=295 "
                "selected_rank=18 judge_best_rank=18 judge_regret=0.0 "
                "judge_win_regret=0.0 judge_spread_regret=0.0\n"
                "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
                "plies=6 candidates=40 arm_plays=15 target_nodes=300 final=0 "
                "control=1 control_kind=candidate_limit iterations=42 nodes=290 "
                "selected_rank=2 judge_best_rank=18 judge_regret=0.02 "
                "judge_win_regret=0.01 judge_spread_regret=3.0\n"
                "THINKING_CURVE_POINT source_index=1 position=1 game=10 bag=20 "
                "plies=6 candidates=40 arm_plays=40 target_nodes=300 final=0 "
                "control=0 control_kind=stopped iterations=42 nodes=298 "
                "selected_rank=1 judge_best_rank=1 judge_regret=0.0 "
                "judge_win_regret=0.0 judge_spread_regret=0.0\n"
                "THINKING_CURVE_POINT source_index=1 position=1 game=10 bag=20 "
                "plies=6 candidates=40 arm_plays=15 target_nodes=300 final=0 "
                "control=1 control_kind=candidate_limit iterations=42 nodes=292 "
                "selected_rank=1 judge_best_rank=1 judge_regret=0.0 "
                "judge_win_regret=0.0 judge_spread_regret=0.0\n",
                encoding="utf-8",
            )
            pairs = load_pairs(path)
            report = analyze(pairs, {})
        self.assertEqual(len(pairs), 2)
        self.assertIn("utility_gain_full_minus_limit +0.010000000", report)
        self.assertIn("positive_candidate_miss events=1/2", report)
        self.assertIn("full_selected_outside_limit events=1/2", report)


if __name__ == "__main__":
    unittest.main()

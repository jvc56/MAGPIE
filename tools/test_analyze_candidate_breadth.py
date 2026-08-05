#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.analyze_candidate_breadth import analyze, decompose_regret, load_roots


class AnalyzeCandidateBreadthTest(unittest.TestCase):
    def test_loads_and_compares_nested_widths(self) -> None:
        rows: list[str] = []
        for source, regrets in enumerate(((0.3, 0.2, 0.1), (0.0, 0.1, 0.0))):
            for width, regret, kind in zip((15, 24, 40), regrets, ("candidate_limit", "candidate_limit", "stopped")):
                rows.append(
                    "THINKING_CURVE_POINT "
                    f"source_index={source} position={source} game={source} bag=40 "
                    f"plies=6 candidates=40 arm_plays={width} target_nodes=300 "
                    f"final=0 control_kind={kind} iterations=42 nodes=290 "
                    f"selected_rank={width - 1} judge_best_rank=39 "
                    f"judge_regret={regret} judge_win_regret={regret / 2} "
                    f"judge_spread_regret={regret * 10}\n"
                )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "curve.log"
            path.write_text("".join(rows), encoding="utf-8")
            roots = load_roots(path)
            report = analyze(roots, {})
        self.assertEqual(len(roots), 2)
        self.assertIn("widths=15,24,40", report)
        self.assertIn("comparison=top40-minus-top15", report)
        self.assertIn("adjacent=top24-minus-top15", report)

    def test_exactly_decomposes_candidate_and_within_set_regret(self) -> None:
        rows = [
            "THINKING_CURVE_JUDGE_CANDIDATE source_index=0 position=0 "
            "game=0 bag=40 plies=6 rank=2 utility=.7 best=0\n",
            "THINKING_CURVE_JUDGE_CANDIDATE source_index=0 position=0 "
            "game=0 bag=40 plies=6 rank=12 utility=.8 best=0\n",
            "THINKING_CURVE_JUDGE_CANDIDATE source_index=0 position=0 "
            "game=0 bag=40 plies=6 rank=35 utility=.9 best=1\n",
            "THINKING_CURVE_POINT source_index=0 position=0 game=0 bag=40 "
            "plies=6 candidates=40 arm_plays=15 target_nodes=300 final=0 "
            "control_kind=candidate_limit iterations=42 nodes=290 "
            "selected_rank=2 judge_best_rank=35 judge_regret=.2 "
            "judge_win_regret=.1 judge_spread_regret=10\n",
            "THINKING_CURVE_POINT source_index=0 position=0 game=0 bag=40 "
            "plies=6 candidates=40 arm_plays=40 target_nodes=300 final=0 "
            "control_kind=stopped iterations=42 nodes=290 selected_rank=35 "
            "judge_best_rank=35 judge_regret=0 judge_win_regret=0 "
            "judge_spread_regret=0\n",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "curve.log"
            path.write_text("".join(rows), encoding="utf-8")
            root = load_roots(path)[0]
        component = decompose_regret(root, 15)
        self.assertAlmostEqual(component.candidate_generation, 0.1)
        self.assertAlmostEqual(component.within_set_bai, 0.1)
        self.assertAlmostEqual(component.total, 0.2)


if __name__ == "__main__":
    unittest.main()

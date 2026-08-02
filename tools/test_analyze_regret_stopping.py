#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.analyze_regret_stopping import (
    clustered_values,
    load_points,
    pair_points,
    summarize_values,
)


class AnalyzeRegretStoppingTest(unittest.TestCase):
    def test_pairs_exact_requested_budget_and_reads_joint_shadow(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stopping.log"
            lines: list[str] = []
            for source in range(4):
                common = (
                    f"source_index={source} position={source} game={source} "
                    "bag=40 plies=4 final=0 elapsed_ns=1000000000 "
                    "selected_rank=0 bai_status=4 estimated_regret=.001 "
                )
                lines.append(
                    "THINKING_CURVE_POINT "
                    + common
                    + "target_nodes=50 control=0 nodes=40 judge_regret=.003 "
                    "joint_estimated_regret=.002 regret_at_stop=.001 "
                    "joint_regret_at_stop=.002 near_tie_challengers=3 "
                    "near_tie_challengers_at_stop=2\n"
                )
                lines.append(
                    "THINKING_CURVE_POINT "
                    + common
                    + "target_nodes=100 control=0 nodes=60 judge_regret=.002 "
                    "joint_estimated_regret=.002 regret_at_stop=.001 "
                    "joint_regret_at_stop=.002 near_tie_challengers=3 "
                    "near_tie_challengers_at_stop=2\n"
                )
                lines.append(
                    "THINKING_CURVE_POINT "
                    + common
                    + "target_nodes=100 control=1 nodes=100 judge_regret=.001 "
                    "joint_estimated_regret=.001 regret_at_stop=nan "
                    "joint_regret_at_stop=nan near_tie_challengers=1 "
                    "near_tie_challengers_at_stop=-1\n"
                )
            path.write_text("".join(lines), encoding="utf-8")

            pairs = pair_points(load_points(path))

        self.assertEqual(len(pairs), 4)
        self.assertEqual(pairs[0][0].target_nodes, 100)
        self.assertEqual(pairs[0][0].joint_stop_estimate, 0.002)
        self.assertEqual(pairs[0][0].near_ties_at_stop, 2)
        deltas = clustered_values(
            pairs, lambda stopped, control: stopped.judge_regret - control.judge_regret
        )
        self.assertEqual(deltas, [0.001] * 4)
        summary = summarize_values(deltas)
        self.assertAlmostEqual(summary.mean, 0.001)
        self.assertEqual(summary.clusters, 4)
        self.assertEqual(summary.p_value, 0.0)


if __name__ == "__main__":
    unittest.main()

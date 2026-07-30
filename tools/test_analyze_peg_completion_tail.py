#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from analyze_peg_completion_tail import (  # noqa: E402
    conformal_audit,
    quantile,
    work_delta,
)


class AnalyzePegCompletionTailTest(unittest.TestCase):
    def test_nearest_rank_quantile(self):
        self.assertEqual(quantile([1, 2, 3, 4], 0.5), 2)
        self.assertEqual(quantile([1, 2, 3, 4], 0.99), 4)

    def test_work_delta_preserves_portable_components(self):
        start = {
            "cumulative_scenarios": 10,
            "nested_endgame_nodes": 20,
            "completed_seconds": 1.0,
            "process_cpu_seconds": 5.0,
        }
        end = {
            "cumulative_scenarios": 17,
            "nested_endgame_nodes": 31,
            "completed_seconds": 3.0,
            "process_cpu_seconds": 23.0,
        }
        delta = work_delta(start, end, 2)
        self.assertEqual(delta["scenarios"], 7)
        self.assertEqual(delta["nested_nodes"], 11)
        self.assertEqual(delta["candidates"], 2)
        self.assertEqual(delta["scheduled_core_occupancy"], 0.5)

    def test_conformal_audit_uses_heldout_64(self):
        rows = [
            {
                "position": f"p{index:03d}",
                "root_candidate_total": 10 + index,
                "seed_scenarios": 20 + index,
                "pre_stage_scenarios": 20 + index,
                "pre_stage_nested_nodes": index,
                "scenarios": 30 + index,
                "nested_nodes": 1000 + index * 10,
            }
            for index in range(320)
        ]
        audit = conformal_audit(rows)
        self.assertEqual(audit["split"]["heldout"], 64)
        self.assertEqual(audit["conformal_rank"], 128)
        self.assertEqual(audit["flat_bound"]["candidate_overhead"], 2)


if __name__ == "__main__":
    unittest.main()

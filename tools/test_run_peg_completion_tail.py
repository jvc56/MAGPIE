#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_peg_completion_tail import (  # noqa: E402
    DEEP_SCHEDULE,
    arm_is_complete,
    completed_arm,
    interleave_by_bag,
)


class RunPegCompletionTailTest(unittest.TestCase):
    def test_interleave_balances_order_over_adjacent_rounds(self):
        positions = [
            {"position": f"b{bag}-{index}", "bag": bag, "cgp": "x"}
            for bag in range(1, 5)
            for index in range(2)
        ]
        ordered = interleave_by_bag(positions)
        self.assertEqual(
            [position["bag"] for position in ordered],
            [1, 2, 3, 4, 4, 3, 2, 1],
        )

    def test_complete_arm_must_reach_declared_boundary(self):
        arm = {
            "status": "completed",
            "requested_stages": 2,
            "deepest_candidate_total": 2,
            "deepest_completed_candidates": 2,
        }
        self.assertTrue(arm_is_complete(arm, DEEP_SCHEDULE))
        arm["deepest_completed_candidates"] = 1
        self.assertFalse(arm_is_complete(arm, DEEP_SCHEDULE))

    def test_completed_arm_can_use_constant_time_index(self):
        position = {"position": "p", "bag": 2}
        arm = {
            "status": "completed",
            "requested_stages": 2,
            "deepest_candidate_total": 2,
            "deepest_completed_candidates": 2,
        }
        self.assertIs(
            completed_arm(
                [],
                position,
                "deep_16_2",
                DEEP_SCHEDULE,
                {("p", "deep_16_2"): arm},
            ),
            arm,
        )


if __name__ == "__main__":
    unittest.main()

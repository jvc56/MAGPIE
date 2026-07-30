#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_peg_fresh_quality import (  # noqa: E402
    build_deep_endpoint,
    build_policy_map,
    patience_stop,
)


def checkpoint(completed, move):
    return {
        "completed_candidates": completed,
        "move": move,
        "cumulative_scenarios": completed * 10,
        "nested_endgame_nodes": completed * 100,
        "completed_seconds": float(completed),
        "process_cpu_seconds": float(completed * 10),
    }


def event(stage, rank, move, seconds):
    return {
        "stage": stage,
        "rank": rank,
        "move": move,
        "cumulative_scenarios": int(seconds * 10),
        "nested_endgame_nodes": int(seconds * 100),
        "completed_seconds": seconds,
        "process_cpu_seconds": seconds * 10,
    }


class RunPegFreshQualityTest(unittest.TestCase):
    def test_patience_stops_after_stable_completed_candidates(self):
        checkpoints = [
            checkpoint(index, "A" if index < 3 else "B")
            for index in range(13)
        ]
        self.assertEqual(patience_stop(checkpoints, 8, 4), 8)
        self.assertEqual(patience_stop(checkpoints, 8, 8), 11)

    def test_deep_endpoint_uses_returned_3ply_move(self):
        arm = {"position": "p", "label": "deep16", "move": "C"}
        result = build_deep_endpoint(
            arm,
            {
                1: [
                    event(1, rank, "B" if rank == 0 else f"x{rank}", 1 + rank)
                    for rank in range(16)
                ],
                2: [
                    event(
                        2,
                        rank,
                        "B" if rank == 0 else ("C" if rank == 1 else f"y{rank}"),
                        20 + rank,
                    )
                    for rank in range(8)
                ],
            },
            [16, 8],
        )
        self.assertEqual(result["move_2ply"], "B")
        self.assertEqual(result["move_3ply"], "C")
        self.assertEqual(
            result["stage_3ply"]["incremental_nested_endgame_nodes"],
            1100,
        )

    def test_policy_map_deduplicates_nominees(self):
        checkpoints = [
            checkpoint(index, "A" if index < 12 else "B")
            for index in range(33)
        ]
        checkpoint_map = {
            "checkpoints": checkpoints,
            "arm_sequence": 1,
        }
        deep16 = {
            "move_3ply": "B",
            "stage_3ply": {},
            "arm_sequence": 2,
        }
        deep24 = {
            "move_3ply": "C",
            "stage_3ply": {},
            "arm_sequence": 3,
        }
        result = build_policy_map(
            {"position": "p", "bag": 2},
            checkpoint_map,
            deep16,
            deep24,
        )
        self.assertEqual(result["nominees"], ["A", "B", "C"])
        self.assertEqual(result["distinct_nominees"], 3)


if __name__ == "__main__":
    unittest.main()

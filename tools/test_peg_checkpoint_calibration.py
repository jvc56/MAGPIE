#!/usr/bin/env python3

import importlib.util
import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))


def load_module(name):
    path = TOOLS / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_module("run_peg_checkpoint_calibration")
ANALYZER = load_module("analyze_peg_checkpoint_calibration")


def event(sequence, stage, rank, move, win, spread, scenarios, nodes):
    return {
        "kind": "event",
        "sequence": sequence,
        "stage": stage,
        "rank": rank,
        "move": move,
        "win": win,
        "spread": spread,
        "scenarios": scenarios,
        "cumulative_scenarios": (rank + 1) * scenarios,
        "nested_endgame_nodes": nodes,
        "completed_seconds": rank + 1.0,
        "process_cpu_seconds": (rank + 1.0) * 5.0,
    }


def checkpoint_map(position, bag, switch_at=None):
    checkpoints = []
    for k in range(33):
        move = "A" if switch_at is None or k < switch_at else "B"
        checkpoints.append(
            {
                "completed_candidates": k,
                "move": move,
                "cumulative_scenarios": 100 + k * 10,
                "nested_endgame_nodes": k * 1000,
                "completed_seconds": 1.0 + k,
                "process_cpu_seconds": 5.0 + k * 5.0,
            }
        )
    return {
        "kind": "checkpoint_map",
        "position": position,
        "bag": bag,
        "arm_sequence": 1,
        "completed_candidates": 32,
        "candidate_total": 32,
        "full_stage": 1,
        "greedy_move": "A",
        "final_move": checkpoints[-1]["move"],
        "frontier_moves": ["A"] if switch_at is None else ["A", "B"],
        "checkpoints": checkpoints,
    }


def judge_records(sequence, position, label, values):
    records = [
        {
            "kind": "judge",
            "sequence": sequence,
            "position": position,
            "bag": 1,
            "label": label,
            "accepted": 1,
            "nominee_count": len(values),
            "cumulative_scenarios": 20,
            "nested_endgame_nodes": 2000,
            "wall_seconds": 2.0,
            "process_cpu_seconds": 18.0,
            "scheduled_core_occupancy": 0.9,
        }
    ]
    for move, win, spread in values:
        records.append(
            {
                "kind": "value",
                "sequence": sequence,
                "position": position,
                "label": label,
                "move": move,
                "win": win,
                "spread": spread,
                "utility": win + 1.0e-4 * spread,
            }
        )
    return records


class PegCheckpointCalibrationTest(unittest.TestCase):
    def test_frontier_reconstruction_uses_candidate_completion_work(self):
        records = [
            event(1, 0, 0, "A", 0.5, 0.0, 10, 0),
            event(1, 0, 1, "X", 0.4, 0.0, 10, 0),
            event(1, 1, 0, "A", 0.5, 0.0, 10, 100),
            event(1, 1, 1, "X", 0.4, 0.0, 10, 200),
            event(1, 1, 2, "B", 0.6, 0.0, 10, 300),
            event(1, 1, 3, "Y", 0.3, 0.0, 10, 400),
        ]
        arm = {
            "sequence": 1,
            "move": "B",
            "status": "completed",
            "refine_candidate_total": 4,
        }
        result = RUNNER.build_checkpoint_map(
            records, arm, {"position": "p1", "bag": 1}
        )
        self.assertEqual(result["frontier_moves"], ["A", "B"])
        self.assertEqual(result["checkpoints"][2]["move"], "A")
        self.assertEqual(result["checkpoints"][3]["move"], "B")
        self.assertEqual(
            result["checkpoints"][3]["nested_endgame_nodes"], 300
        )
        self.assertEqual(
            result["checkpoints"][0]["process_cpu_seconds"], 10.0
        )

    def test_frontier_includes_exact_running_best_ties(self):
        records = [
            event(1, 0, 0, "A", 0.5, 0.0, 10, 0),
            event(1, 1, 0, "A", 0.5, 0.0, 10, 100),
            event(1, 1, 1, "B", 0.5, 0.0, 10, 200),
            event(1, 1, 2, "C", 0.6, 0.0, 10, 300),
        ]
        arm = {
            "sequence": 1,
            "move": "C",
            "status": "completed",
            "refine_candidate_total": 3,
        }
        result = RUNNER.build_checkpoint_map(
            records, arm, {"position": "p1", "bag": 1}
        )
        self.assertEqual(result["schema_version"], 2)
        self.assertEqual(result["frontier_moves"], ["A", "B", "C"])
        self.assertEqual(
            result["checkpoints"][2]["tied_best_moves"], ["A", "B"]
        )
        self.assertEqual(
            result["checkpoints"][3]["tied_best_moves"], ["C"]
        )

    def test_quality_curve_keeps_exact_agreements_and_uses_oracle_values(self):
        records = [
            {
                "kind": "arm",
                "mode": "arm",
                "label": "checkpoint",
                "sequence": 1,
                "position": "p1",
                "bag": 1,
                "status": "completed",
            },
            {
                "kind": "arm",
                "mode": "arm",
                "label": "checkpoint",
                "sequence": 2,
                "position": "p2",
                "bag": 2,
                "status": "completed",
            },
            checkpoint_map("p1", 1, switch_at=4),
            checkpoint_map("p2", 2),
        ]
        records.extend(
            judge_records(
                10,
                "p1",
                "checkpoint_stride4",
                (("A", 0.40, 0.0), ("B", 0.60, 0.0)),
            )
        )
        records.extend(
            judge_records(
                11,
                "p1",
                "checkpoint_stride2",
                (("A", 0.41, 0.0), ("B", 0.59, 0.0)),
            )
        )
        for index, label in enumerate(("before", "between", "after")):
            records.append(
                {
                    "kind": "arm",
                    "mode": "sentinel",
                    "sequence": 20 + index,
                    "position": "sentinel",
                    "label": label,
                    "nested_endgame_nodes": 1000,
                    "wall_seconds": 10.0,
                    "scheduled_core_occupancy": 0.9,
                }
            )

        result = ANALYZER.analyze(records)
        at_zero = result["checkpoint_curve"]["0"]
        at_four = result["checkpoint_curve"]["4"]
        self.assertEqual(at_zero["gain_vs_greedy"]["utility"]["mean"], 0.0)
        self.assertAlmostEqual(
            at_zero["full_minus_stop"]["utility"]["mean"], 0.1
        )
        self.assertAlmostEqual(
            at_four["gain_vs_greedy"]["utility"]["mean"], 0.1
        )
        self.assertEqual(
            at_four["full_minus_stop"]["utility"]["mean"], 0.0
        )
        self.assertEqual(
            at_zero["conditional_full_minus_stop"]["utility"]["n"], 1
        )
        self.assertEqual(
            result["judge_completion"]["checkpoint_stride4"]["censored"], 0
        )
        self.assertEqual(
            result["sensitivity"]["best_nominee_agreement_rate"], 1.0
        )
        adaptive = result["stopping_policies"]["min8_patience8"]
        self.assertEqual(adaptive["stop_candidates"]["mean"], 10.0)
        self.assertAlmostEqual(
            adaptive["gain_vs_greedy"]["utility"]["mean"], 0.1
        )
        self.assertEqual(
            result["stage_admission"]["minimum_completed_candidates"], 2
        )


if __name__ == "__main__":
    unittest.main()

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


RUNNER = load_module("run_peg_4ply_calibration")
ANALYZER = load_module("analyze_peg_4ply_calibration")


def event(sequence, stage, rank, move, utility, nodes, seconds, position="p1"):
    return {
        "kind": "event",
        "position": position,
        "sequence": sequence,
        "stage": stage,
        "rank": rank,
        "move": move,
        "win": utility,
        "spread": 0.0,
        "scenarios": 10,
        "cumulative_scenarios": int(seconds * 10),
        "nested_endgame_nodes": nodes,
        "completed_seconds": seconds,
        "process_cpu_seconds": seconds * 5.0,
    }


class Peg4CalibrationTest(unittest.TestCase):
    def test_map_and_analyzer_use_completed_stage_boundaries(self):
        records = [
            {
                "kind": "arm",
                "mode": "arm",
                "position": "p1",
                "label": "greedy",
                "sequence": 1,
                "move": "A",
                "status": "completed",
                "cumulative_scenarios": 10,
                "nested_endgame_nodes": 0,
                "wall_seconds": 1.0,
                "process_cpu_seconds": 5.0,
            },
            {
                "kind": "arm",
                "mode": "arm",
                "position": "p1",
                "label": "narrow4",
                "sequence": 2,
                "move": "D",
                "status": "completed",
                "last_completed_stage": 3,
                "partial": 0,
                "wall_seconds": 6.0,
                "process_cpu_seconds": 30.0,
                "scheduled_core_occupancy": 0.5,
            },
        ]
        for stage in range(1, 4):
            records.append(
                {
                    "kind": "stage",
                    "sequence": 2,
                    "stage": stage,
                    "candidate_total": 2,
                    "completed_candidates": 2,
                    "start_seconds": float(stage),
                    "end_seconds": float(stage + 1),
                }
            )
        records.extend(
            [
                event(2, 0, 0, "A", 0.4, 0, 1.0),
                event(2, 1, 0, "A", 0.4, 100, 1.5),
                event(2, 1, 1, "B", 0.5, 200, 2.0),
                # The next-stage rank-zero input is the actual prior winner.
                event(2, 2, 0, "B", 0.5, 300, 2.5),
                event(2, 2, 1, "C", 0.6, 400, 3.0),
                event(2, 3, 0, "C", 0.6, 600, 4.0),
                event(2, 3, 1, "D", 0.7, 900, 5.0),
            ]
        )
        stage_map = RUNNER.build_depth_map(
            records, {"position": "p1", "bag": 2}
        )
        endpoints = {
            endpoint["label"]: endpoint
            for endpoint in stage_map["endpoints"]
        }
        self.assertEqual(endpoints["narrow4_2ply"]["move"], "B")
        self.assertEqual(endpoints["narrow4_3ply"]["move"], "C")
        self.assertEqual(endpoints["narrow4_4ply"]["move"], "D")

        records.append(stage_map)
        records.append(
            {
                "kind": "judge",
                "position": "p1",
                "label": "direct4_stride4",
                "sequence": 10,
                "accepted": 1,
                "nominee_count": 4,
                "judge_ply": 4,
                "wall_seconds": 1.0,
            }
        )
        for move, utility in (
            ("A", 0.4),
            ("B", 0.5),
            ("C", 0.6),
            ("D", 0.7),
        ):
            records.append(
                {
                    "kind": "value",
                    "position": "p1",
                    "label": "direct4_stride4",
                    "sequence": 10,
                    "move": move,
                    "win": utility,
                    "spread": 0.0,
                    "utility": utility,
                }
            )
        result = ANALYZER.analyze(records)
        depth = result["comparisons"]["narrow_3ply_to_4ply"]
        self.assertEqual(depth["paired_completed"], 1)
        self.assertAlmostEqual(depth["gain"]["utility"]["mean"], 0.1)
        admission = result["stage4_admission"]["narrow4"]
        self.assertEqual(admission["observations"], 1)
        self.assertEqual(
            admission["overall"]["nested_endgame_nodes"]["median"], 500
        )
        self.assertEqual(admission["overall"]["scenarios"]["median"], 20)

    def test_combined_runs_do_not_mix_reused_sequence_numbers(self):
        records = []
        for position, scenario_scale in (("p1", 10), ("p2", 100)):
            records.extend(
                [
                    {
                        "kind": "peg4_map",
                        "position": position,
                        "bag": 2,
                        "arm_sequences": {"narrow4": 2},
                        "endpoints": [],
                    },
                    {
                        "kind": "arm",
                        "mode": "arm",
                        "position": position,
                        "label": "narrow4",
                        "sequence": 2,
                        "status": "completed",
                        "last_completed_stage": 3,
                        "partial": 0,
                        "wall_seconds": 6.0,
                        "process_cpu_seconds": 30.0,
                        "scheduled_core_occupancy": 0.5,
                    },
                    event(
                        2,
                        2,
                        0,
                        "A",
                        0.4,
                        scenario_scale,
                        1.0,
                        position,
                    ),
                    event(
                        2,
                        3,
                        0,
                        "A",
                        0.4,
                        scenario_scale * 2,
                        2.0,
                        position,
                    ),
                    event(
                        2,
                        3,
                        1,
                        "B",
                        0.5,
                        scenario_scale * 3,
                        3.0,
                        position,
                    ),
                ]
            )

        result = ANALYZER.analyze(records)
        admission = result["stage4_admission"]["narrow4"]
        self.assertEqual(admission["observations"], 2)
        self.assertEqual(
            admission["overall"]["nested_endgame_nodes"]["median"], 110
        )


if __name__ == "__main__":
    unittest.main()

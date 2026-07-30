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


RUNNER = load_module("run_peg_stage_cap_calibration")
ANALYZER = load_module("analyze_peg_stage_cap_calibration")


def event(sequence, stage, rank, move, win, nodes, seconds):
    return {
        "kind": "event",
        "sequence": sequence,
        "stage": stage,
        "rank": rank,
        "move": move,
        "win": win,
        "spread": 0.0,
        "scenarios": 10,
        "cumulative_scenarios": int(seconds * 10),
        "nested_endgame_nodes": nodes,
        "completed_seconds": seconds,
        "process_cpu_seconds": seconds * 5.0,
    }


class PegStageCapCalibrationTest(unittest.TestCase):
    def test_map_uses_actual_stage1_winner_and_stage_boundaries(self):
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
                "label": "cap16",
                "sequence": 2,
                "move": "C",
                "status": "completed",
                "last_completed_stage": 2,
                "partial": 0,
                "wall_seconds": 5.0,
                "process_cpu_seconds": 25.0,
                "scheduled_core_occupancy": 0.5,
            },
            {
                "kind": "stage",
                "sequence": 2,
                "stage": 1,
                "candidate_total": 2,
                "completed_candidates": 2,
                "end_seconds": 3.0,
            },
            {
                "kind": "stage",
                "sequence": 2,
                "stage": 2,
                "candidate_total": 2,
                "completed_candidates": 2,
                "end_seconds": 5.0,
            },
            event(2, 0, 0, "A", 0.4, 0, 1.0),
            event(2, 1, 0, "A", 0.5, 100, 2.0),
            event(2, 1, 1, "B", 0.5, 200, 3.0),
            # Rank zero is the actual qsort winner carried from stage 1.
            event(2, 2, 0, "B", 0.5, 300, 4.0),
            event(2, 2, 1, "C", 0.6, 500, 5.0),
        ]
        result = RUNNER.build_stage_cap_map(
            records, {"position": "p1", "bag": 1}
        )
        endpoints = {
            endpoint["label"]: endpoint
            for endpoint in result["endpoints"]
        }
        self.assertEqual(endpoints["cap16_2ply"]["move"], "B")
        self.assertEqual(endpoints["cap16_3ply"]["move"], "C")
        self.assertEqual(
            endpoints["cap16_2ply"]["incremental_nested_endgame_nodes"],
            200,
        )
        self.assertEqual(
            endpoints["cap16_3ply"]["incremental_nested_endgame_nodes"],
            300,
        )
        analyzed = ANALYZER.analyze([*records, result])
        admission = analyzed["stage3_admission"]["cap16"]
        self.assertEqual(admission["observations"], 1)
        self.assertEqual(
            admission["overall"]["nested_endgame_nodes"]["median"], 300
        )
        self.assertEqual(admission["overall"]["scenarios"]["median"], 20)

    def test_analyzer_values_completed_3ply_increment_with_oracle(self):
        stage_map = {
            "kind": "stage_cap_map",
            "position": "p1",
            "bag": 1,
            "endpoints": [
                {
                    "label": "greedy",
                    "move": "A",
                    "accepted": 1,
                },
                {
                    "label": "cap16_2ply",
                    "move": "B",
                    "accepted": 1,
                    "incremental_scenarios": 10,
                    "incremental_nested_endgame_nodes": 100,
                    "incremental_seconds": 1.0,
                    "incremental_process_cpu_seconds": 5.0,
                },
                {
                    "label": "cap16_3ply",
                    "move": "C",
                    "accepted": 1,
                    "incremental_scenarios": 20,
                    "incremental_nested_endgame_nodes": 200,
                    "incremental_seconds": 2.0,
                    "incremental_process_cpu_seconds": 10.0,
                },
            ],
        }
        records = [
            stage_map,
            {
                "kind": "judge",
                "sequence": 10,
                "position": "p1",
                "label": "stagecap_stride4",
                "accepted": 1,
                "nominee_count": 3,
                "wall_seconds": 2.0,
                "process_cpu_seconds": 18.0,
                "scheduled_core_occupancy": 0.9,
                "cumulative_scenarios": 30,
                "nested_endgame_nodes": 300,
            },
        ]
        for move, utility in (("A", 0.4), ("B", 0.5), ("C", 0.6)):
            records.append(
                {
                    "kind": "value",
                    "sequence": 10,
                    "position": "p1",
                    "label": "stagecap_stride4",
                    "move": move,
                    "win": utility,
                    "spread": 0.0,
                    "utility": utility,
                }
            )
        result = ANALYZER.analyze(records)
        increment = result["ply3_increment"]["cap16"]
        self.assertEqual(increment["paired_completed"], 1)
        self.assertAlmostEqual(
            increment["gain_3ply_minus_2ply"]["utility"]["mean"], 0.1
        )
        self.assertAlmostEqual(
            result["endpoint_results"]["cap16_3ply"]["gain_vs_greedy"][
                "utility"
            ]["mean"],
            0.2,
        )


if __name__ == "__main__":
    unittest.main()

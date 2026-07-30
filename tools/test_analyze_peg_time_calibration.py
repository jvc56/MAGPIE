#!/usr/bin/env python3

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("analyze_peg_time_calibration.py")
SPEC = importlib.util.spec_from_file_location("peg_analysis", MODULE_PATH)
assert SPEC and SPEC.loader
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


def arm(sequence, position, bag, label, move):
    return {
        "kind": "arm",
        "mode": "arm",
        "sequence": sequence,
        "position": position,
        "bag": bag,
        "label": label,
        "move": move,
        "status": "completed",
        "refine_completed_candidates": 32 if label != "greedy" else 0,
        "cumulative_scenarios": 100,
        "nested_endgame_nodes": 1_000,
        "wall_seconds": 10,
        "process_cpu_seconds": 50,
        "scheduled_core_occupancy": 0.5,
    }


def judge(sequence, position, label, values):
    records = [
        {
            "kind": "judge",
            "sequence": sequence,
            "position": position,
            "bag": 1,
            "label": label,
            "accepted": 1,
            "nominee_count": len(values),
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


class AnalyzePegTimeCalibrationTest(unittest.TestCase):
    def test_agreements_are_zero_and_censored_judges_are_not_substituted(self):
        records = []
        sequence = 0
        for position, bag, moves in (
            ("p1", 1, ("A", "A", "A", "A")),
            ("p2", 2, ("A", "A", "B", "C")),
            ("p3", 3, ("A", "A", "B", "C")),
        ):
            for label, move in zip(("greedy", "30s", "60s", "120s"), moves):
                records.append(arm(sequence, position, bag, label, move))
                sequence += 1
        records.extend(
            [
                {
                    "kind": "agreement",
                    "position": "p1",
                    "bag": 1,
                    "label": "exact",
                    "accepted": 1,
                },
                {
                    "kind": "judge",
                    "sequence": 20,
                    "position": "p2",
                    "bag": 2,
                    "label": "stride4",
                    "accepted": 1,
                    "nominee_count": 3,
                },
                {
                    "kind": "value",
                    "sequence": 20,
                    "position": "p2",
                    "label": "stride4",
                    "move": "A",
                    "win": 0.50,
                    "spread": 0.0,
                    "utility": 0.50,
                },
                {
                    "kind": "value",
                    "sequence": 20,
                    "position": "p2",
                    "label": "stride4",
                    "move": "B",
                    "win": 0.55,
                    "spread": 10.0,
                    "utility": 0.551,
                },
                {
                    "kind": "value",
                    "sequence": 20,
                    "position": "p2",
                    "label": "stride4",
                    "move": "C",
                    "win": 0.54,
                    "spread": 20.0,
                    "utility": 0.542,
                },
                {
                    "kind": "judge",
                    "sequence": 21,
                    "position": "p3",
                    "bag": 3,
                    "label": "stride4",
                    "accepted": 0,
                    "nominee_count": 3,
                },
            ]
        )
        for label, wall in (("before", 10), ("between", 10), ("after", 10)):
            records.append(
                {
                    "kind": "arm",
                    "mode": "sentinel",
                    "sequence": sequence,
                    "position": "sentinel",
                    "label": label,
                    "nested_endgame_nodes": 1_000,
                    "wall_seconds": wall,
                    "scheduled_core_occupancy": 0.7,
                }
            )
            sequence += 1

        result = ANALYSIS.analyze(records)
        first = result["marginals"]["30s_to_60s"]
        self.assertEqual(first["accepted"], 2)
        self.assertEqual(first["censored_disagreements"], 1)
        self.assertAlmostEqual(first["all_accepted"]["utility"]["mean"], 0.0255)
        self.assertAlmostEqual(
            first["conditional_disagreement"]["utility"]["mean"], 0.051
        )
        second = result["marginals"]["60s_to_120s"]
        self.assertAlmostEqual(
            second["conditional_disagreement"]["utility"]["mean"], -0.009
        )
        self.assertEqual(result["judge_completion"]["stride4_censored"], 1)

    def test_sensitivity_treats_tied_best_nominees_as_a_set(self):
        records = [
            arm(index, "p1", 1, label, move)
            for index, (label, move) in enumerate(
                (
                    ("greedy", "A"),
                    ("30s", "B"),
                    ("60s", "B"),
                    ("120s", "B"),
                )
            )
        ]
        records.extend(
            judge(
                10,
                "p1",
                "stride4",
                (("A", 0.50, 0.0), ("B", 0.50, 0.0)),
            )
        )
        records.extend(
            judge(
                11,
                "p1",
                "stride2",
                (("A", 0.49, 0.0), ("B", 0.51, 0.0)),
            )
        )

        result = ANALYSIS.analyze(records)
        self.assertEqual(result["sensitivity"]["accepted_positions"], 1)
        self.assertEqual(
            result["sensitivity"]["best_nominee_agreement_rate"], 1.0
        )
        self.assertEqual(
            result["sensitivity"]["best_disagreement_positions"], []
        )

    def test_sentinels_are_summarized_per_source(self):
        records = []
        for source, throughputs in (
            ("first", (100.0, 100.0, 100.0)),
            ("second", (200.0, 200.0, 200.0)),
        ):
            for sequence, (label, throughput) in enumerate(
                zip(("before", "between", "after"), throughputs)
            ):
                records.append(
                    {
                        "kind": "arm",
                        "mode": "sentinel",
                        "sequence": sequence,
                        "position": f"{source}-sentinel",
                        "label": label,
                        "nested_endgame_nodes": throughput * 10.0,
                        "wall_seconds": 10.0,
                        "scheduled_core_occupancy": 0.9,
                        "_source": source,
                    }
                )

        result = ANALYSIS.analyze(records)
        self.assertEqual(set(result["monitoring"]["runs"]), {"first", "second"})
        self.assertEqual(
            result["monitoring"]["runs"]["first"]["sentinels"][0][
                "throughput_nodes_per_second"
            ],
            100.0,
        )
        self.assertEqual(
            result["monitoring"]["runs"]["second"]["sentinels"][0][
                "throughput_nodes_per_second"
            ],
            200.0,
        )


if __name__ == "__main__":
    unittest.main()

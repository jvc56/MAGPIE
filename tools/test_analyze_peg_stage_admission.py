import copy
import json
import pathlib
import unittest

from analyze_peg_stage_admission import (
    replay_stage_one,
    replay_stages,
    summarize,
    summarize_by_stage,
)


class CompleteStageAdmissionAnalysisTest(unittest.TestCase):
    def setUp(self) -> None:
        root = pathlib.Path(__file__).resolve().parent.parent
        self.artifact = json.loads(
            (
                root
                / "tools/peg_time_calibration/complete_stage_admission_v1_20260801.json"
            ).read_text()
        )
        self.depth_artifact = json.loads(
            (
                root
                / "tools/peg_time_calibration/adaptive_complete_stage_admission_v6_20260801.json"
            ).read_text()
        )
        self.v3_depth_artifact = json.loads(
            (
                root
                / "tools/peg_time_calibration/complete_stage_admission_v3_20260801.json"
            ).read_text()
        )

    @staticmethod
    def records(*, budget: float, completed: bool) -> list[dict]:
        return [
            {
                "kind": "invocation",
                "sequence": 0,
                "mode": "arm",
                "position": "synthetic",
                "bag": 1,
                "threads": 10,
                "budget_seconds": budget,
                "schedule": [2],
            },
            {
                "kind": "stage",
                "sequence": 0,
                "stage": 0,
                "start_seconds": 0.0,
                "end_seconds": 0.1,
            },
            {
                "kind": "event",
                "sequence": 0,
                "stage": 0,
                "move": "A",
                "win": 0.6,
                "spread": 1.0,
                "scenarios": 7,
                "nested_endgame_nodes": 0,
            },
            {
                "kind": "event",
                "sequence": 0,
                "stage": 0,
                "move": "B",
                "win": 0.5,
                "spread": 0.0,
                "scenarios": 7,
                "nested_endgame_nodes": 0,
            },
            {
                "kind": "stage",
                "sequence": 0,
                "stage": 1,
                "start_seconds": 0.1,
                "end_seconds": min(budget, 1.0),
                "candidate_total": 2,
                "completed_candidates": 2 if completed else 1,
            },
            {
                "kind": "event",
                "sequence": 0,
                "stage": 1,
                "move": "A",
                "win": 0.6,
                "spread": 1.0,
                "scenarios": 7,
                "nested_endgame_nodes": 100,
            },
            *(
                [
                    {
                        "kind": "event",
                        "sequence": 0,
                        "stage": 1,
                        "move": "B",
                        "win": 0.5,
                        "spread": 0.0,
                        "scenarios": 7,
                        "nested_endgame_nodes": 200,
                    }
                ]
                if completed
                else []
            ),
            {
                "kind": "arm",
                "sequence": 0,
                "last_completed_stage": 1 if completed else 0,
                "partial": 0 if completed else 1,
                "deepest_completed_candidates": 2 if completed else 1,
            },
        ]

    def test_physical_deadline_controls_admission(self) -> None:
        ample = replay_stage_one(self.records(budget=100.0, completed=True), self.artifact)
        self.assertTrue(ample[0]["admitted"])
        self.assertFalse(ample[0]["false_start"])

        tight = replay_stage_one(self.records(budget=0.2, completed=False), self.artifact)
        self.assertFalse(tight[0]["admitted"])
        self.assertFalse(tight[0]["false_start"])

    def test_gate_needs_independent_admitted_evidence(self) -> None:
        row = replay_stage_one(self.records(budget=100.0, completed=True), self.artifact)[0]
        summary = summarize([row] * 58, self.artifact)
        self.assertFalse(summary["gate_passed"])
        self.assertLess(summary["target_quantile_evidence"], 0.95)

    def test_completion_after_its_bound_is_a_false_start(self) -> None:
        records = self.records(budget=1000.0, completed=True)
        stage = next(
            row
            for row in records
            if row.get("kind") == "stage" and row.get("stage") == 1
        )
        stage["end_seconds"] = 900.0
        row = replay_stage_one(records, self.artifact)[0]
        self.assertTrue(row["completed"])
        self.assertTrue(row["admitted"])
        self.assertFalse(row["completion_bound_covered_actual"])
        self.assertTrue(row["false_start"])

    def test_deeper_stage_uses_selected_previous_nodes(self) -> None:
        records = self.records(budget=100.0, completed=True)
        records[0]["schedule"] = [2, 2]
        records[-1].update(
            {
                "last_completed_stage": 2,
                "deepest_completed_candidates": 2,
            }
        )
        previous_events = [
            row
            for row in records
            if row.get("kind") == "event" and row.get("stage") == 1
        ]
        previous_events[0]["nested_endgame_nodes"] = 100
        previous_events[1]["nested_endgame_nodes"] = 200
        records.extend(
            [
                {
                    "kind": "stage",
                    "sequence": 0,
                    "stage": 2,
                    "start_seconds": 1.0,
                    "end_seconds": 2.0,
                    "candidate_total": 2,
                    "completed_candidates": 2,
                },
                {
                    "kind": "event",
                    "sequence": 0,
                    "stage": 2,
                    "move": "A",
                    "win": 0.6,
                    "spread": 1.0,
                    "scenarios": 7,
                    "nested_endgame_nodes": 1000,
                },
                {
                    "kind": "event",
                    "sequence": 0,
                    "stage": 2,
                    "move": "B",
                    "win": 0.5,
                    "spread": 0.0,
                    "scenarios": 7,
                    "nested_endgame_nodes": 2000,
                },
            ]
        )
        # The synthetic stage-one record needs the same accounting fields as
        # prospective traces now that the generic audit checks every join.
        stage_one = next(
            row
            for row in records
            if row.get("kind") == "stage" and row.get("stage") == 1
        )
        stage_one.update(candidate_total=2, completed_candidates=2)
        rows = replay_stages(records, self.depth_artifact)
        deeper = next(row for row in rows if row["stage_index"] == 2)
        self.assertEqual(deeper["selected_previous_stage_nodes"], 300)
        self.assertGreaterEqual(deeper["completion_bound_nodes"], 300 * 16)
        grouped = summarize_by_stage(rows, self.depth_artifact)
        self.assertEqual(set(grouped["stages"]), {"1", "2"})

    def test_v4_keeps_bag_residual_after_runtime_rate_warms(self) -> None:
        warmup = self.records(budget=1000.0, completed=True)
        measured = copy.deepcopy(warmup)
        for record in measured:
            if "sequence" in record:
                record["sequence"] = 1
            if record.get("kind") == "invocation":
                record["bag"] = 2
        combined = warmup + measured

        v3_rows = replay_stages(combined, self.v3_depth_artifact, {1})
        v4_rows = replay_stages(combined, self.depth_artifact, {1})
        v3_measured = next(row for row in v3_rows if row["sequence"] == 1)
        v4_measured = next(row for row in v4_rows if row["sequence"] == 1)

        self.assertTrue(v4_measured["runtime_rate_ready"])
        self.assertGreater(
            v4_measured["completion_bound_seconds"],
            2.0 * v3_measured["completion_bound_seconds"],
        )

    def test_zero_node_deepest_stage_keeps_shallower_runtime_rate(self) -> None:
        warmup = self.records(budget=1000.0, completed=True)
        warmup[0]["schedule"] = [2, 2]
        warmup[-1].update(
            {
                "last_completed_stage": 2,
                "deepest_completed_candidates": 2,
            }
        )
        warmup.extend(
            [
                {
                    "kind": "stage",
                    "sequence": 0,
                    "stage": 2,
                    "start_seconds": 1.0,
                    "end_seconds": 2.0,
                    "candidate_total": 2,
                    "completed_candidates": 2,
                },
                {
                    "kind": "event",
                    "sequence": 0,
                    "stage": 2,
                    "move": "A",
                    "win": 0.6,
                    "spread": 1.0,
                    "scenarios": 7,
                    "nested_endgame_nodes": 0,
                },
                {
                    "kind": "event",
                    "sequence": 0,
                    "stage": 2,
                    "move": "B",
                    "win": 0.5,
                    "spread": 0.0,
                    "scenarios": 7,
                    "nested_endgame_nodes": 0,
                },
            ]
        )
        measured = self.records(budget=1000.0, completed=True)
        for record in measured:
            if "sequence" in record:
                record["sequence"] = 1

        rows = replay_stages(warmup + measured, self.depth_artifact, {1})
        measured_stage = next(row for row in rows if row["sequence"] == 1)
        self.assertTrue(measured_stage["runtime_rate_ready"])
        self.assertEqual(measured_stage["runtime_rate_observations"], 1)


if __name__ == "__main__":
    unittest.main()

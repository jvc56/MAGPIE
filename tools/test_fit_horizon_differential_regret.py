#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.fit_horizon_differential_regret import (
    HorizonRow,
    cross_fit,
    fit_model,
    load_horizon_model_artifact,
    predict,
    read_horizon_panel,
    replay_rule_of_zero,
)


def make_row(
    game_index: int,
    nodes: int,
    mismatch: int,
    improvement: float,
    *,
    near_ties: int = 0,
    stable: int = 4,
) -> HorizonRow:
    iterations = max(1, nodes // 7)
    horizon_regret = 0.001
    return HorizonRow(
        source_index=game_index,
        game=f"game-{game_index}",
        policy="static" if game_index % 2 == 0 else "playchooser-g3000ms",
        bag=10 + game_index % 70,
        bag_band=(
            "middle_late_16_35" if game_index % 2 else "middle_early_36_60"
        ),
        width=60,
        checkpoint=nodes // 1_000,
        iterations=iterations,
        nodes=nodes,
        horizon_iterations=428_571,
        horizon_nodes=3_000_000,
        landmark_nodes=nodes,
        static_cutoff_gap=12.0,
        estimated_regret=0.0002,
        joint_estimated_regret=0.0003,
        bai_gap=0.01,
        near_tie_challengers=near_ties,
        stable_checkpoints=stable,
        stable_iterations=max(0, iterations - 256),
        selected_switches=2,
        selected_rank=1 if mismatch else 0,
        horizon_selected_rank=0,
        current_judge_regret=horizon_regret + improvement,
        horizon_judge_regret=horizon_regret,
        horizon_mismatch=mismatch,
        signed_horizon_improvement=improvement,
    )


class FitHorizonDifferentialRegretTest(unittest.TestCase):
    def test_reads_signed_horizon_labels_and_keeps_horizon_label_only(self) -> None:
        manifest = {
            "mapping": [
                {
                    "position": 7,
                    "source_seed": 123,
                    "start": 4,
                    "trajectory_policy": "static",
                    "bag": 29,
                    "bag_band": "middle_late_16_35",
                }
            ]
        }
        position = (
            "THINKING_CURVE_POSITION source_index=0 position=7 game=0 bag=29 "
            "plies=6 candidates=60 max_nodes=100000 "
            "static_primary_gap=18.5\n"
        )
        template = (
            "THINKING_CURVE_CHECKPOINT source_index=0 position=7 game=0 "
            "bag=29 plies=6 arm_plays=60 checkpoint={checkpoint} "
            "iterations={iterations} nodes={nodes} selected_rank={rank} "
            "selected_id={selected_id} stable_checkpoints={stable} "
            "stable_iterations={stable_iterations} selected_switches=1 "
            "full_selected_rank=0 estimated_best=.53 "
            "estimated_challenger=.51 estimated_regret=.0002 "
            "joint_estimated_regret=.0003 near_tie_challengers=2 "
            "judge_regret={regret} judge_win_regret=0 "
            "judge_spread_regret=0\n"
        )
        checkpoints = template.format(
            checkpoint=0,
            iterations=100,
            nodes=25_000,
            rank=1,
            selected_id=11,
            stable=1,
            stable_iterations=0,
            regret=".010",
        ) + template.format(
            checkpoint=1,
            iterations=200,
            nodes=50_000,
            rank=0,
            selected_id=10,
            stable=2,
            stable_iterations=100,
            regret=".002",
        )
        horizon = (
            "THINKING_CURVE_POINT source_index=0 position=7 game=0 bag=29 "
            "plies=6 arm_plays=60 target_nodes=100000 nodes=100000 "
            "iterations=1000 final=0 control_kind=stopped selected_rank=0 "
            "judge_regret=.002\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "thinking-curves.log"
            selection = root / "selection-manifest.json"
            log.write_text(position + checkpoints + horizon, encoding="utf-8")
            selection.write_text(json.dumps(manifest), encoding="utf-8")
            landmarks, all_rows = read_horizon_panel(
                log, selection, (25_000, 50_000)
            )
        self.assertEqual(len(all_rows), 2)
        self.assertEqual([row.horizon_mismatch for row in landmarks], [1, 0])
        self.assertAlmostEqual(landmarks[0].signed_horizon_improvement, 0.008)
        self.assertAlmostEqual(landmarks[1].signed_horizon_improvement, 0.0)
        self.assertEqual(landmarks[0].horizon_selected_rank, 0)

    def test_complete_game_cross_fit_predicts_probability_and_signed_value(self) -> None:
        rows = [
            make_row(
                game,
                nodes,
                int(nodes <= 100_000 and game % 3 != 0),
                0.004 if nodes <= 100_000 and game % 3 != 0 else 0.0,
                near_ties=2 if nodes <= 100_000 else 0,
            )
            for game in range(30)
            for nodes in (50_000, 100_000, 300_000)
        ]
        landmarks, replay = cross_fit(rows, rows, 5, 82631, 12.0, 8.0)
        self.assertEqual(len(landmarks), len(rows))
        self.assertEqual(len(replay), len(rows))
        self.assertEqual(len({item.fold for item in landmarks}), 5)
        for item in landmarks:
            self.assertGreaterEqual(item.mismatch_probability, 0.0)
            self.assertLessEqual(item.mismatch_probability, 1.0)
            self.assertAlmostEqual(
                item.expected_signed_improvement,
                item.mismatch_probability
                * item.conditional_signed_improvement,
            )

    def test_signed_severity_is_not_monotone_clamped(self) -> None:
        rows = [make_row(game, 50_000, 1, -0.002) for game in range(10)]
        model = fit_model(rows, 4.0, 4.0)
        item = predict(model, rows[0], 0)
        self.assertLess(item.conditional_signed_improvement, 0.0)
        self.assertEqual(item.stopping_score, 0.0)

    def test_loads_frozen_shadow_model_without_labels(self) -> None:
        artifact = {
            "artifact_kind": "cross_fitted_same_arm_horizon_differential_model",
            "schema_version": 1,
            "mismatch_model": [
                {"level": 0, "key": [], "mean": 0.25, "observations": 20}
            ],
            "signed_improvement_model": [
                {"level": 0, "key": [], "mean": 0.004, "observations": 5}
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "model.json"
            path.write_text(json.dumps(artifact), encoding="utf-8")
            model = load_horizon_model_artifact(path)
        item = predict(model, make_row(0, 100_000, 0, 0.0), -1)
        self.assertAlmostEqual(item.mismatch_probability, 0.25)
        self.assertAlmostEqual(item.conditional_signed_improvement, 0.004)
        self.assertAlmostEqual(item.stopping_score, 0.001)

    def test_rule_of_zero_waits_for_safe_checkpoint(self) -> None:
        rows = [
            make_row(0, 100_000, 0, 0.0),
            make_row(0, 200_000, 0, 0.0),
            make_row(1, 100_000, 1, 0.004, near_ties=2),
            make_row(1, 200_000, 0, 0.0),
        ]
        [summary] = replay_rule_of_zero(rows, (100_000,), (2,))
        self.assertEqual(summary["choice_mismatches"], 0)
        self.assertEqual(summary["stopped_before_horizon"], 2)
        self.assertTrue(summary["offline_gate_pass"])


if __name__ == "__main__":
    unittest.main()

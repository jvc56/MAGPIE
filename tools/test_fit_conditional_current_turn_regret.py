#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.fit_conditional_current_turn_regret import (
    apply_calibration_cells,
    cross_fit,
    fit_calibration_cells,
    apply_isotonic_calibrator,
    fit_isotonic_calibrator,
    nested_cross_fit_with_bounds,
    read_checkpoint_rows,
    read_combined_rows,
)
from tools.fit_total_current_turn_regret import Row


def make_row(game_index: int, width: int = 60) -> Row:
    event = game_index % 7 == 0
    within = 0.002 + 0.0001 * (game_index % 3) if event else 0.0
    candidate = 0.004 if width == 15 and game_index % 19 == 0 else 0.0
    return Row(
        source_index=game_index,
        game=f"game-{game_index}",
        policy="static" if game_index % 2 == 0 else "playchooser-g3000ms",
        bag=10 + game_index % 80,
        bag_band=(
            "late_5_15" if game_index % 4 == 0 else "middle_late_16_35"
        ),
        width=width,
        target_nodes=3_000_000,
        static_cutoff_gap=20.0,
        estimated_regret=0.0005 if event else 0.0,
        joint_estimated_regret=0.0007 if event else 0.0,
        bai_gap=0.01,
        near_tie_challengers=2 if event else 0,
        candidate_generation_regret=candidate,
        within_set_bai_regret=within,
        total_current_turn_regret=candidate + within,
    )


class FitConditionalCurrentTurnRegretTest(unittest.TestCase):
    def test_two_part_predictions_are_probabilistic_and_additive(self) -> None:
        rows = [
            make_row(game, width)
            for game in range(80)
            for width in (15, 60)
        ]
        predictions = cross_fit(
            rows,
            folds=5,
            seed=82605,
            event_prior_strength=12.0,
            severity_prior_strength=6.0,
            positive_epsilon=1.0e-15,
        )
        self.assertEqual(len(predictions), len(rows))
        for item in predictions:
            self.assertGreaterEqual(item.candidate.event_probability, 0.0)
            self.assertLessEqual(item.candidate.event_probability, 1.0)
            self.assertGreaterEqual(item.within.event_probability, 0.0)
            self.assertLessEqual(item.within.event_probability, 1.0)
            self.assertAlmostEqual(
                item.predicted_total_current_turn_regret,
                item.candidate.event_probability
                * item.candidate.positive_severity
                + item.within.event_probability * item.within.positive_severity,
            )

    def test_clustered_upper_bound_is_nonnegative_and_fail_closed(self) -> None:
        rows = [make_row(game) for game in range(80)]
        predictions = cross_fit(
            rows,
            folds=5,
            seed=82605,
            event_prior_strength=12.0,
            severity_prior_strength=6.0,
            positive_epsilon=1.0e-15,
        )
        cells = fit_calibration_cells(
            predictions,
            deployment_width=60,
            coverage=0.9,
            minimum_games=8,
        )
        calibrated = apply_calibration_cells(predictions, cells)
        self.assertTrue(any(cell.level == "global" for cell in cells))
        for item in calibrated:
            self.assertGreaterEqual(
                item.upper_total_current_turn_regret,
                item.predicted_total_current_turn_regret,
            )
            self.assertGreaterEqual(item.calibration_games, 8)

    def test_nested_bound_excludes_outer_fold_labels(self) -> None:
        rows = [
            make_row(game, width)
            for game in range(100)
            for width in (15, 60)
        ]
        bounded, final_cells, isotonic_blocks = nested_cross_fit_with_bounds(
            rows,
            folds=5,
            inner_folds=4,
            seed=82605,
            event_prior_strength=12.0,
            severity_prior_strength=6.0,
            positive_epsilon=1.0e-15,
            deployment_width=60,
            coverage=0.9,
            minimum_games=8,
            isotonic_prior_strength=2.0,
        )
        self.assertEqual(len(bounded), len(rows))
        self.assertTrue(final_cells)
        self.assertTrue(isotonic_blocks)
        self.assertTrue(
            all(
                item.calibration_level != "none"
                and item.calibration_games >= 8
                for item in bounded
            )
        )

    def test_isotonic_calibration_is_monotone_and_preserves_additivity(self) -> None:
        rows = [
            make_row(game, width)
            for game in range(80)
            for width in (15, 60)
        ]
        predictions = cross_fit(
            rows,
            folds=5,
            seed=82605,
            event_prior_strength=12.0,
            severity_prior_strength=6.0,
            positive_epsilon=1.0e-15,
        )
        blocks = fit_isotonic_calibrator(
            predictions, deployment_width=60, prior_strength=2.0
        )
        self.assertEqual(
            [block.calibrated_regret for block in blocks],
            sorted(block.calibrated_regret for block in blocks),
        )
        calibrated = apply_isotonic_calibrator(
            predictions, blocks, deployment_width=60
        )
        for item in calibrated:
            self.assertAlmostEqual(
                item.predicted_total_current_turn_regret,
                item.predicted_candidate_generation_regret
                + item.predicted_within_set_bai_regret,
            )

    def test_reads_corrected_combined_panel_and_rejects_invalid_marker(self) -> None:
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
            "plies=6 candidates=60 max_nodes=3000000 "
            "static_primary_gap=18.5\n"
        )
        point = (
            "THINKING_CURVE_POINT source_index=0 position=7 game=0 bag=29 "
            "plies=6 arm_plays=60 target_nodes=1000000 nodes=999999 final=0 "
            "control=0 control_kind=stopped selected_rank=0 "
            "judge_regret=0.0012 estimated_best=0.53 estimated_challenger=0.51 "
            "estimated_regret=0.0002 joint_estimated_regret=0.0003 "
            "regret_at_stop=0.00025 joint_regret_at_stop=0.00035 "
            "near_tie_challengers=2 near_tie_challengers_at_stop=3\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "thinking-curves.log"
            selection = root / "selection-manifest.json"
            log.write_text(position + point, encoding="utf-8")
            selection.write_text(json.dumps(manifest), encoding="utf-8")
            rows = read_combined_rows(log, selection, "fresh", 100)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0].source_index, 100)
            self.assertEqual(rows[0].width, 60)
            self.assertEqual(rows[0].near_tie_challengers, 3)
            self.assertAlmostEqual(rows[0].joint_estimated_regret, 0.00035)
            self.assertAlmostEqual(rows[0].within_set_bai_regret, 0.0012)
            (root / "INVALID_PROTOCOL.txt").write_text("invalid\n")
            with self.assertRaisesRegex(ValueError, "invalid protocol"):
                read_combined_rows(log, selection, "fresh", 100)

    def test_reads_fixed_checkpoint_landmarks_with_stability(self) -> None:
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
            "plies=6 candidates=60 max_nodes=3000000 "
            "static_primary_gap=18.5\n"
        )
        checkpoint_template = (
            "THINKING_CURVE_CHECKPOINT source_index=0 position=7 game=0 "
            "bag=29 plies=6 arm_plays=60 checkpoint={checkpoint} "
            "iterations={iterations} nodes={nodes} selected_rank=0 "
            "selected_id=20 stable_checkpoints={stable} "
            "stable_iterations={stable_iterations} selected_switches=1 "
            "full_selected_rank=0 estimated_best=.53 "
            "estimated_challenger=.51 estimated_regret=.0002 "
            "joint_estimated_regret=.0003 near_tie_challengers=2 "
            "judge_regret={regret} judge_win_regret=0 "
            "judge_spread_regret=0\n"
        )
        checkpoints = "".join(
            checkpoint_template.format(
                checkpoint=index,
                iterations=iterations,
                nodes=nodes,
                stable=index + 1,
                stable_iterations=index * 100,
                regret=regret,
            )
            for index, (iterations, nodes, regret) in enumerate(
                (
                    (100, 300_000, ".001"),
                    (200, 450_000, ".0005"),
                    (300, 600_000, "0"),
                )
            )
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "thinking-curves.log"
            selection = root / "selection-manifest.json"
            log.write_text(position + checkpoints, encoding="utf-8")
            selection.write_text(json.dumps(manifest), encoding="utf-8")
            rows = read_checkpoint_rows(log, selection, "audit", 200)
        self.assertEqual(len(rows), 2)
        self.assertEqual([row.target_nodes for row in rows], [300_000, 600_000])
        self.assertEqual([row.stable_checkpoints for row in rows], [1, 3])
        self.assertEqual(rows[1].selected_switches, 1)
        self.assertAlmostEqual(rows[0].within_set_bai_regret, 0.001)


if __name__ == "__main__":
    unittest.main()

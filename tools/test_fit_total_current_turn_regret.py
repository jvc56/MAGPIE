#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path
import tempfile

from tools.fit_total_current_turn_regret import (
    CANDIDATE_LEVELS,
    Row,
    WITHIN_LEVELS,
    cross_fit,
    fit_component_model,
    predict_component,
    write_runtime_model,
)


def make_row(game_index: int, width: int) -> Row:
    candidate = 0.02 if width == 15 else 0.004
    within = 0.001 * (game_index % 4)
    return Row(
        source_index=game_index,
        game=f"game-{game_index}",
        policy="static" if game_index % 2 == 0 else "playchooser-g3000ms",
        bag=10 + game_index % 80,
        bag_band=f"band-{game_index % 4}",
        width=width,
        target_nodes=3_000_000,
        static_cutoff_gap=3.0 if width == 15 else 20.0,
        estimated_regret=within / 2,
        joint_estimated_regret=within,
        bai_gap=0.01,
        near_tie_challengers=game_index % 4,
        candidate_generation_regret=candidate,
        within_set_bai_regret=within,
        total_current_turn_regret=candidate + within,
    )


class FitTotalCurrentTurnRegretTest(unittest.TestCase):
    def test_component_models_are_nonnegative_and_additive(self) -> None:
        rows = [
            make_row(game_index, width)
            for game_index in range(100)
            for width in (15, 60)
        ]
        predictions = cross_fit(rows, folds=5, seed=82604, prior_strength=8.0)
        self.assertEqual(len(predictions), len(rows))
        for prediction in predictions:
            self.assertGreaterEqual(
                prediction.predicted_candidate_generation_regret, 0.0
            )
            self.assertGreaterEqual(prediction.predicted_within_set_bai_regret, 0.0)
            self.assertAlmostEqual(
                prediction.predicted_total_current_turn_regret,
                prediction.predicted_candidate_generation_regret
                + prediction.predicted_within_set_bai_regret,
            )

    def test_wider_static_screen_gets_smaller_candidate_prediction(self) -> None:
        rows = [
            make_row(game_index, width)
            for game_index in range(20)
            for width in (15, 60)
        ]
        model = fit_component_model(
            rows,
            CANDIDATE_LEVELS,
            lambda row: row.candidate_generation_regret,
            prior_strength=4.0,
        )
        self.assertGreater(
            predict_component(model, rows[0], CANDIDATE_LEVELS),
            predict_component(model, rows[1], CANDIDATE_LEVELS),
        )
        within_model = fit_component_model(
            rows,
            WITHIN_LEVELS,
            lambda row: row.within_set_bai_regret,
            prior_strength=4.0,
        )
        self.assertGreaterEqual(
            predict_component(within_model, rows[0], WITHIN_LEVELS), 0.0
        )

    def test_runtime_model_has_fixed_auditable_columns(self) -> None:
        rows = [
            make_row(game_index, width)
            for game_index in range(20)
            for width in (15, 60)
        ]
        candidate_model = fit_component_model(
            rows,
            CANDIDATE_LEVELS,
            lambda row: row.candidate_generation_regret,
            prior_strength=4.0,
        )
        within_model = fit_component_model(
            rows,
            WITHIN_LEVELS,
            lambda row: row.within_set_bai_regret,
            prior_strength=4.0,
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime.tsv"
            write_runtime_model(output, candidate_model, within_model)
            lines = output.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "SIM_TOTAL_REGRET_MODEL\t1")
        self.assertTrue(any(line.startswith("C\t4\t") for line in lines))
        self.assertTrue(any(line.startswith("W\t5\t") for line in lines))
        for line in lines[1:]:
            fields = line.split("\t")
            self.assertEqual(len(fields), 8 if fields[0] == "C" else 9)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.fit_horizon_differential_regret import load_horizon_model_artifact
from tools.run_rule_zero_panel import (
    decorate_shadow_scores,
    validate_rule_zero_chunk_accounting,
)


def synthetic_chunk() -> str:
    position = (
        "THINKING_CURVE_POSITION source_index=1 position=1 game=1 bag=40 "
        "plies=6 candidates=60 max_nodes=3000000 uniform_floor_per_mille=100 "
        "judge_plies=10 judge_samples=100000 judge_risk_plays=8 "
        "panel_best_rank=0 static_best_equity=50 static_primary_gap=12 "
        "trajectory_policy=static rule_zero=1 cgp=x\n"
    )
    checkpoint_template = (
        "THINKING_CURVE_RULE_ZERO_CHECKPOINT source_index=1 position=1 game=1 "
        "bag=40 plies=6 arm_plays=60 checkpoint={checkpoint} iterations={iterations} "
        "nodes={nodes} selected_rank=0 selected_id=99 stable_checkpoints={stable} "
        "stable_iterations={stable_iterations} selected_switches=0 "
        "full_selected_rank=0 estimated_best=.52 estimated_challenger=.50 "
        "estimated_regret=.0001 joint_estimated_regret=.0002 "
        "near_tie_challengers=0 counters_valid=1 rule_eligible={eligible} "
        "rule_selected={selected}\n"
    )
    checkpoints = checkpoint_template.format(
        checkpoint=0,
        iterations=15000,
        nodes=100000,
        stable=1,
        stable_iterations=0,
        eligible=0,
        selected=0,
    ) + checkpoint_template.format(
        checkpoint=1,
        iterations=16000,
        nodes=110000,
        stable=2,
        stable_iterations=1000,
        eligible=1,
        selected=1,
    )
    rule = (
        "THINKING_CURVE_RULE_ZERO source_index=1 position=1 game=1 bag=40 "
        "plies=6 arm_plays=60 checkpoints=2 checkpoint_interval=256 "
        "minimum_nodes=100000 minimum_stable_checkpoints=2 stop_checkpoint=1 "
        "stopped_iterations=16000 stopped_nodes=110000 stopped_rank=0 "
        "stopped_id=99 stable_checkpoints=2 stable_iterations=1000 "
        "selected_switches=0 near_tie_challengers=0 capped=0 "
        "full_iterations=428571 full_nodes=3000000 full_rank=0 full_id=99 "
        "equal_slice_nodes=3000000 equal_iterations=428571 equal_nodes=3000000 "
        "equal_rank=0 equal_id=99 matched_control=0 matched_target_nodes=0 "
        "matched_iterations=0 matched_nodes=0 matched_rank=-1 matched_id=0 "
        "matched_min_play_iterations=0 matched_sampling_rule=top_two_ids "
        "matched_modulus=3 matched_remainder=0 audit_selected=0 "
        "audit_modulus=10 audit_remainder=0 judge_performed=0 "
        "horizon_mismatch=0 equal_horizon_mismatch=0 matched_mismatch=0\n"
    )
    point_template = (
        "THINKING_CURVE_POINT source_index=1 position=1 game=1 bag=40 plies=6 "
        "arm_plays=60 target_nodes={target} final=0 control_kind={kind} "
        "iterations={iterations} nodes={nodes} selected_rank=0 "
        "min_play_iterations=71 bai_status={status}\n"
    )
    points = point_template.format(
        target=110000,
        kind="stopped",
        iterations=16000,
        nodes=110000,
        status=4,
    ) + point_template.format(
        target=3000000,
        kind="full",
        iterations=428571,
        nodes=3000000,
        status=3,
    ) + point_template.format(
        target=3000000,
        kind="equal_horizon",
        iterations=428571,
        nodes=3000000,
        status=3,
    )
    done = (
        "THINKING_CURVE_POSITION_DONE source_index=1 position=1 plies=6 "
        "events=4 dropped=0 final_iterations=16000 final_nodes=110000 "
        "judge_candidates=0 judge_iterations=0 judge_nodes=0 judge_seconds=0 "
        "judge_forced=1 judge_performed=0 matched_control=0 "
        "combined_checkpoints=2 rule_zero=1\n"
        "THINKING_CURVE_DONE plies=6 evaluated=1 skip_positions=1 next_skip=2\n"
    )
    return position + checkpoints + rule + points + done


class RunRuleZeroPanelTest(unittest.TestCase):
    def test_audits_rule_and_appends_label_free_shadow_rows(self) -> None:
        artifact = {
            "artifact_kind": "cross_fitted_same_arm_horizon_differential_model",
            "schema_version": 1,
            "mismatch_model": [
                {"level": 0, "key": [], "mean": 0.1, "observations": 50}
            ],
            "signed_improvement_model": [
                {"level": 0, "key": [], "mean": 0.003, "observations": 5}
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chunk = root / "chunk.log"
            model_path = root / "model.json"
            chunk.write_text(synthetic_chunk(), encoding="utf-8")
            model_path.write_text(json.dumps(artifact), encoding="utf-8")
            model = load_horizon_model_artifact(model_path)
            accepted = validate_rule_zero_chunk_accounting(
                chunk,
                plies=6,
                max_nodes=3_000_000,
                minimum_nodes=100_000,
                minimum_stable_checkpoints=2,
                equal_slice_nodes=3_000_000,
                checkpoint_interval=256,
                matched_modulus=3,
                matched_remainder=0,
                audit_modulus=10,
                audit_remainder=0,
                judge_samples=100_000,
                judge_risk_plays=8,
            )
            self.assertEqual(accepted, [1])
            decorate_shadow_scores(chunk, model, "abc")
            validate_rule_zero_chunk_accounting(
                chunk,
                plies=6,
                max_nodes=3_000_000,
                minimum_nodes=100_000,
                minimum_stable_checkpoints=2,
                equal_slice_nodes=3_000_000,
                checkpoint_interval=256,
                matched_modulus=3,
                matched_remainder=0,
                audit_modulus=10,
                audit_remainder=0,
                judge_samples=100_000,
                judge_risk_plays=8,
                shadow_model=model,
                shadow_model_sha256="abc",
            )
            text = chunk.read_text(encoding="utf-8")
        self.assertEqual(text.count("THINKING_CURVE_HORIZON_SHADOW "), 2)
        self.assertIn("stopping_score=0.00030000000000000003", text)

    def test_rejects_nonfirst_selected_checkpoint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chunk.log"
            path.write_text(
                synthetic_chunk().replace(
                    "rule_eligible=0 rule_selected=0",
                    "rule_eligible=1 rule_selected=0",
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "rule_eligible"):
                validate_rule_zero_chunk_accounting(
                    path,
                    plies=6,
                    max_nodes=3_000_000,
                    minimum_nodes=100_000,
                    minimum_stable_checkpoints=2,
                    equal_slice_nodes=3_000_000,
                    checkpoint_interval=256,
                    matched_modulus=3,
                    matched_remainder=0,
                    audit_modulus=10,
                    audit_remainder=0,
                    judge_samples=100_000,
                    judge_risk_plays=8,
                )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.run_stratified_thinking_curve_panel import (
    completed_by_plies,
    eligible_indices,
    validate_chunk_accounting,
    validate_prefix,
)


class RunStratifiedThinkingCurvePanelTest(unittest.TestCase):
    def test_eligible_indices_and_completed_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "panel.positions"
            corpus.write_text(
                "".join(
                    "TIME_VALUE_POSITION "
                    f"position={index} game=0 source_seed=1 start=0 "
                    f"turn={index + 1} player={index % 2} bag={bag} "
                    "own_rack=7 opp_rack=7 spread=0 predicted_future_turns=1 "
                    "trajectory_policy=static cgp=15 A/B 0/0 0\n"
                    for index, bag in enumerate((86, 12, 4, 0))
                ),
                encoding="utf-8",
            )
            log = root / "curve.log"
            log.write_text(
                "THINKING_CURVE_POSITION_DONE source_index=0 position=0 "
                "plies=2 events=8 dropped=0 final_iterations=1 final_nodes=3\n"
                "THINKING_CURVE_FORCED_POSITION source_index=1 position=1 "
                "game=0 bag=12 plies=2 candidates=1 regret=0 cgp=x\n",
                encoding="utf-8",
            )
            eligible = eligible_indices(corpus, 5, 100)
            completed = completed_by_plies(log)[2]
        self.assertEqual(eligible, [0, 1])
        self.assertEqual(completed, {0, 1})
        self.assertEqual(validate_prefix(eligible, completed), 2)

    def test_rejects_nonprefix_completion(self) -> None:
        with self.assertRaises(ValueError):
            validate_prefix([0, 2, 4], {0, 4})

    @staticmethod
    def _matched_chunk(matched_iterations: int = 30) -> str:
        rows = [
            "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
            "plies=6 target_nodes=300 final=0 control=0 control_kind=stopped "
            "iterations=30 nodes=200 min_play_iterations=1 bai_status=4 "
            "regret_at_stop=.001 joint_regret_at_stop=.005\n",
            "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
            "plies=6 target_nodes=210 final=0 control=1 control_kind=matched "
            f"iterations={matched_iterations} nodes=205 min_play_iterations=1 "
            "bai_status=3 regret_at_stop=nan joint_regret_at_stop=nan\n",
            "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
            "plies=6 target_nodes=300 final=0 control=1 control_kind=full "
            "iterations=42 nodes=290 min_play_iterations=1 bai_status=3 "
            "regret_at_stop=nan joint_regret_at_stop=nan\n",
            "THINKING_CURVE_REGRET_PANEL source_index=0 position=0 game=9 "
            "bag=40 plies=6 target_nodes=300 risk_count=2 paired_rows=4 "
            "probe_iterations=8\n",
            "THINKING_CURVE_REGRET_ARM source_index=0 plies=6 "
            "target_nodes=300 rank=0\n",
            "THINKING_CURVE_REGRET_ARM source_index=0 plies=6 "
            "target_nodes=300 rank=1\n",
        ]
        for pair_row in range(4):
            for rank in range(2):
                rows.append(
                    "THINKING_CURVE_REGRET_SAMPLE source_index=0 plies=6 "
                    f"target_nodes=300 pair_row={pair_row} rank={rank}\n"
                )
        rows.extend(
            [
                "THINKING_CURVE_POSITION_DONE source_index=0 position=0 "
                "plies=6 events=6 dropped=0 final_iterations=30 "
                "final_nodes=200 judge_candidates=3 judge_iterations=3000 "
                "judge_forced=0 control=1 control_iterations=42 "
                "control_nodes=290 matched_control=1 matched_target_nodes=210 "
                f"matched_iterations={matched_iterations} matched_nodes=205\n",
                "THINKING_CURVE_DONE plies=6 evaluated=1\n",
            ]
        )
        return "".join(rows)

    def test_validates_exact_matched_control_and_regret_trace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chunk.log"
            path.write_text(self._matched_chunk(), encoding="utf-8")
            accepted = validate_chunk_accounting(
                path,
                plies=6,
                targets=(300,),
                max_nodes=300,
                num_plays=3,
                judge_samples=1000,
                regret_stop_target=0.01,
                regret_stop_use_joint=True,
                regret_trace=True,
                regret_risk_plays=2,
                regret_paired_samples=4,
                fixed_control=True,
                matched_control=True,
            )
        self.assertEqual(accepted, [0])

    def test_rejects_mismatched_iteration_control(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chunk.log"
            path.write_text(self._matched_chunk(29), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "matched iterations differ"):
                validate_chunk_accounting(
                    path,
                    plies=6,
                    targets=(300,),
                    max_nodes=300,
                    num_plays=3,
                    judge_samples=1000,
                    regret_stop_target=0.01,
                    regret_stop_use_joint=True,
                    regret_trace=True,
                    regret_risk_plays=2,
                    regret_paired_samples=4,
                    fixed_control=True,
                    matched_control=True,
                )

    def test_validates_candidate_limit_control(self) -> None:
        text = "".join(
            [
                "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
                "plies=6 candidates=40 arm_plays=40 target_nodes=300 final=0 "
                "control=0 control_kind=stopped iterations=42 nodes=295 "
                "min_play_iterations=1 bai_status=3\n",
                "THINKING_CURVE_POINT source_index=0 position=0 game=9 bag=40 "
                "plies=6 candidates=40 arm_plays=15 target_nodes=300 final=0 "
                "control=1 control_kind=candidate_limit iterations=42 nodes=290 "
                "min_play_iterations=1 bai_status=3\n",
                "THINKING_CURVE_POSITION_DONE source_index=0 position=0 "
                "plies=6 events=4 dropped=0 final_iterations=42 final_nodes=295 "
                "judge_candidates=3 judge_iterations=3000 judge_forced=0 "
                "control=0 control_iterations=0 control_nodes=0 "
                "matched_control=0 matched_target_nodes=0 matched_iterations=0 "
                "matched_nodes=0 candidate_control=1 candidate_control_plays=15 "
                "candidate_control_iterations=42 candidate_control_nodes=290\n",
                "THINKING_CURVE_DONE plies=6 evaluated=1\n",
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chunk.log"
            path.write_text(text, encoding="utf-8")
            accepted = validate_chunk_accounting(
                path,
                plies=6,
                targets=(300,),
                max_nodes=300,
                num_plays=40,
                judge_samples=1000,
                regret_stop_target=0.0,
                regret_stop_use_joint=False,
                regret_trace=False,
                regret_risk_plays=8,
                regret_paired_samples=4,
                fixed_control=False,
                matched_control=False,
                candidate_control_plays=15,
            )
        self.assertEqual(accepted, [0])


if __name__ == "__main__":
    unittest.main()

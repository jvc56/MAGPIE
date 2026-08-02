#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.fit_sim_checkpoint_regret import (
    Point,
    cross_fit,
    matched_depth_summaries,
    nonzero_regret_summary,
    read_metadata,
    read_points,
)


class FitSimCheckpointRegretTest(unittest.TestCase):
    @staticmethod
    def point(
        game: str,
        position: int,
        plies: int,
        regret: float,
        selected_rank: int,
        *,
        bag: int = 40,
        policy: str = "static",
    ) -> Point:
        return Point(
            game=game,
            position=position,
            bag=bag,
            spread=0,
            policy=policy,
            plies=plies,
            target_nodes=1000,
            actual_nodes=1000,
            candidates=15,
            selected_rank=selected_rank,
            estimated_regret=0.0,
            estimated_gap=0.01,
            challenger_valid=True,
            actual_regret=regret,
            actual_win_regret=regret / 2.0,
            actual_spread_regret=regret * 100.0,
        )

    def test_cross_fitted_state_curves_are_monotone(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corpus = root / "panel.positions"
            log = root / "curves.log"
            corpus_lines: list[str] = []
            log_lines: list[str] = []
            for position in range(20):
                bag = 10 if position % 2 == 0 else 70
                policy = "static" if position < 10 else "playchooser-g3000ms"
                corpus_lines.append(
                    "TIME_VALUE_POSITION "
                    f"position={position} game={position} source_seed={1000 + position} "
                    f"start={position % 2} turn=1 player=0 bag={bag} own_rack=7 "
                    "opp_rack=7 spread=0 predicted_future_turns=5 "
                    f"trajectory_policy={policy} cgp=15 A/B 0/0 0\n"
                )
                for nodes, regret in ((100, 0.030), (1000, 0.010)):
                    observed = regret + 0.001 * (position % 3)
                    log_lines.append(
                        "THINKING_CURVE_POINT "
                        f"source_index={position} position={position} game={position} "
                        f"bag={bag} plies=4 candidates=15 target_nodes={nodes} "
                        "final=0 control=0 selected_rank=1 nodes="
                        f"{nodes} estimated_best=0.55 estimated_challenger=0.54 "
                        f"estimated_regret={observed / 2:.9f} "
                        f"judge_regret={observed:.9f} "
                        f"judge_win_regret={observed / 2:.9f} "
                        f"judge_spread_regret={observed * 100:.9f}\n"
                    )
                log_lines.append(
                    "THINKING_CURVE_POSITION_DONE "
                    f"source_index={position} position={position} plies=4 "
                    "events=4 dropped=0 final_iterations=1 final_nodes=1000\n"
                )
            corpus.write_text("".join(corpus_lines), encoding="utf-8")
            log.write_text("".join(log_lines), encoding="utf-8")
            points = read_points(log, read_metadata(corpus))
            predictions = cross_fit(points, 5, 17, 5.0)

        self.assertEqual(len(predictions), 40)
        by_position: dict[int, list[tuple[int, float]]] = {}
        for prediction in predictions:
            self.assertGreaterEqual(prediction.state_expected_regret, 0.0)
            self.assertGreaterEqual(prediction.checkpoint_expected_regret, 0.0)
            by_position.setdefault(prediction.point.position, []).append(
                (
                    prediction.point.target_nodes,
                    prediction.state_expected_regret,
                )
            )
        for curve in by_position.values():
            curve.sort()
            self.assertGreaterEqual(curve[0][1], curve[1][1])

    def test_zero_event_bound_counts_source_games(self) -> None:
        points = [
            self.point("a", 0, 4, 0.0, 0),
            self.point("a", 1, 4, 0.0, 0),
            self.point("b", 2, 4, 0.0, 0),
            self.point("c", 3, 4, 0.0, 0),
            self.point("d", 4, 4, 0.0, 0),
        ]
        summary = nonzero_regret_summary(points)

        self.assertEqual(summary["positions"], 5)
        self.assertEqual(summary["games"], 4)
        self.assertEqual(summary["nonzero_games"], 0)
        self.assertAlmostEqual(
            summary["one_sided_upper95_if_zero"],
            1.0 - 0.05 ** (1.0 / 4.0),
        )

    def test_matched_depth_comparison_never_selects_by_oracle(self) -> None:
        points = [
            self.point("a", 0, 4, 0.03, 1, bag=10, policy="static"),
            self.point("a", 0, 6, 0.01, 2, bag=10, policy="static"),
            self.point(
                "b", 1, 4, 0.01, 3, bag=70, policy="playchooser-g3000ms"
            ),
            self.point(
                "b", 1, 6, 0.02, 3, bag=70, policy="playchooser-g3000ms"
            ),
        ]
        summary = matched_depth_summaries(points)
        row = summary["overall"][0]

        self.assertEqual(row["shallow_plies"], 4)
        self.assertEqual(row["deep_plies"], 6)
        self.assertEqual(row["positions"], 2)
        self.assertAlmostEqual(
            row["utility_regret_reduction"]["mean"], 0.005
        )
        self.assertAlmostEqual(row["choice_change"]["mean"], 0.5)
        self.assertEqual(
            row["selection_counts"],
            {"deep_better": 1, "shallow_better": 1, "tied": 0},
        )
        self.assertEqual(len(summary["by_bag"]), 2)
        self.assertEqual(len(summary["by_policy"]), 2)


if __name__ == "__main__":
    unittest.main()

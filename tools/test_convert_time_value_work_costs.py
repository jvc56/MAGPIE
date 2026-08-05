#!/usr/bin/env python3

from __future__ import annotations

import unittest

from tools.convert_time_value_work_costs import RateProfile, convert_rows, row_cost


PROFILE = RateProfile(
    identifier=2,
    name="test",
    sim_seconds_per_node=1.0e-6,
    endgame_seconds_per_node=2.0e-6,
    peg_fixed_seconds_per_chunk=0.01,
    peg_seconds_per_scenario=0.001,
    peg_seconds_per_endgame_node=3.0e-6,
    peg_seconds_per_candidate=0.02,
)


class ConvertTimeValueWorkCostsTest(unittest.TestCase):
    def test_mode_native_costs_remain_separate(self) -> None:
        self.assertAlmostEqual(
            row_cost(
                {
                    "mode": "sim",
                    "nodes": "100000",
                    "fixed_seconds": "0.02",
                },
                PROFILE,
            ),
            0.12,
        )
        self.assertAlmostEqual(
            row_cost(
                {
                    "mode": "peg",
                    "nodes": "100000",
                    "scenarios": "20",
                    "candidates": "4",
                    "fixed_seconds": "0.02",
                },
                PROFILE,
            ),
            0.43,
        )

    def test_conversion_marks_rate_profile(self) -> None:
        rows = [
            {
                "game": "g",
                "turn": "0",
                "player": "0",
                "mode": "endgame",
                "nodes": "500000",
                "regret": "0.1",
            }
        ]
        converted = convert_rows(rows, [PROFILE])
        self.assertEqual(converted[0]["rate_profile"], "2")
        self.assertEqual(converted[0]["rate_profile_name"], "test")
        self.assertEqual(float(converted[0]["cost"]), 1.0)

    def test_peg_incremental_coordinates_do_not_reprice_root_candidates(self) -> None:
        profile = RateProfile(
            identifier=4,
            name="incremental-peg",
            sim_seconds_per_node=0.0,
            endgame_seconds_per_node=0.0,
            peg_fixed_seconds_per_chunk=0.01,
            peg_seconds_per_scenario=0.001,
            peg_seconds_per_endgame_node=0.000003,
            peg_seconds_per_candidate=0.02,
            peg_seconds_per_greedy_scenario=0.0001,
            peg_seconds_per_added_scenario=0.0,
        )
        self.assertAlmostEqual(
            row_cost(
                {
                    "mode": "peg",
                    "nodes": "999999",
                    "scenarios": "999999",
                    "candidates": "999999",
                    "greedy_scenarios": "100",
                    "added_scenarios": "50",
                    "added_endgame_nodes": "1000",
                    "added_candidates": "4",
                },
                profile,
            ),
            0.103,
        )

    def test_per_ply_sim_rate_overrides_generic_rate(self) -> None:
        profile = RateProfile(
            identifier=3,
            name="ply-aware",
            sim_seconds_per_node=1.0e-6,
            endgame_seconds_per_node=2.0e-6,
            peg_fixed_seconds_per_chunk=0.0,
            peg_seconds_per_scenario=0.0,
            peg_seconds_per_endgame_node=0.0,
            peg_seconds_per_candidate=0.0,
            sim_seconds_per_node_p4=3.0e-6,
        )
        self.assertAlmostEqual(
            row_cost(
                {
                    "mode": "sim",
                    "plies": "4",
                    "nodes": "100000",
                },
                profile,
            ),
            0.3,
        )
        self.assertAlmostEqual(
            row_cost(
                {
                    "mode": "sim",
                    "plies": "2",
                    "nodes": "100000",
                },
                profile,
            ),
            0.1,
        )


if __name__ == "__main__":
    unittest.main()

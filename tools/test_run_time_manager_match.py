#!/usr/bin/env python3
"""Focused tests for TimeManager match regret accounting."""

from __future__ import annotations

import math
import unittest

from tools.run_time_manager_match import (
    add_realized_path_rest_regret,
    turn_regret,
)


def turn(player: int, valid: bool, regret: float = math.nan) -> dict[str, str]:
    return {
        "player": str(player),
        "regret_valid": "1" if valid else "0",
        "expected_utility_regret": str(regret),
    }


class TimeManagerMatchRegretTest(unittest.TestCase):
    def test_turn_regret_keeps_unknown_distinct_from_zero(self) -> None:
        self.assertIsNone(turn_regret(turn(0, False)))
        self.assertEqual(turn_regret(turn(0, True, 0.0)), 0.0)
        self.assertAlmostEqual(turn_regret(turn(0, True, 0.125)), 0.125)

    def test_realized_path_reverse_sum_is_per_player(self) -> None:
        game_turns = [
            turn(0, True, 0.10),
            turn(1, False),
            turn(0, True, 0.20),
            turn(1, True, 0.05),
            turn(0, False),
        ]
        add_realized_path_rest_regret({1: game_turns})

        self.assertEqual(
            game_turns[0]["realized_path_rest_game_expected_regret"],
            "0.300000000000",
        )
        self.assertEqual(
            game_turns[0]["realized_path_rest_game_estimated_turns"], "2"
        )
        self.assertEqual(
            game_turns[0]["realized_path_rest_game_unknown_turns"], "1"
        )
        self.assertEqual(
            game_turns[1]["realized_path_rest_game_expected_regret"],
            "0.050000000000",
        )
        self.assertEqual(
            game_turns[1]["realized_path_rest_game_estimated_turns"], "1"
        )
        self.assertEqual(
            game_turns[1]["realized_path_rest_game_unknown_turns"], "1"
        )
        self.assertEqual(
            game_turns[4]["realized_path_rest_game_expected_regret"],
            "0.000000000000",
        )
        self.assertEqual(
            game_turns[4]["realized_path_rest_game_unknown_turns"], "1"
        )


if __name__ == "__main__":
    unittest.main()

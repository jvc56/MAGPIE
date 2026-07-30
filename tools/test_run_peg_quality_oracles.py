#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from run_peg_quality_oracles import (  # noqa: E402
    judge_is_accepted,
    judge_label,
)


class RunPegQualityOraclesTest(unittest.TestCase):
    def test_judge_label(self):
        self.assertEqual(judge_label(4, 2), "fresh_direct4_stride2")

    def test_acceptance_requires_depth_and_all_nominees(self):
        judge = {
            "accepted": 1,
            "judge_ply": 4,
            "nominee_count": 3,
        }
        self.assertTrue(judge_is_accepted(judge, 4, 3))
        self.assertFalse(judge_is_accepted(judge, 3, 3))
        self.assertFalse(judge_is_accepted(judge, 4, 2))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

from __future__ import annotations

import unittest

from tools.clustered_inference import (
    regularized_incomplete_beta,
    student_t_critical,
    student_t_two_sided_p_value,
)


class ClusteredInferenceTest(unittest.TestCase):
    def test_regularized_beta_identity(self) -> None:
        self.assertAlmostEqual(regularized_incomplete_beta(1.0, 1.0, 0.3), 0.3)

    def test_student_t_reference_values(self) -> None:
        critical = student_t_critical(0.95, 19)
        self.assertAlmostEqual(critical, 2.093024054, places=8)
        self.assertAlmostEqual(
            student_t_two_sided_p_value(critical, 19), 0.05, places=10
        )
        self.assertAlmostEqual(student_t_critical(0.95, 1), 12.706204736, places=8)


if __name__ == "__main__":
    unittest.main()

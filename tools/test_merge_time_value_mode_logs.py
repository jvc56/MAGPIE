import unittest

from tools.merge_time_value_mode_logs import line_key, merged_lines


class MergeTimeValueModeLogsTest(unittest.TestCase):
    def test_line_key(self) -> None:
        self.assertEqual(
            line_key("EGCURVEPOINT pos=7 depth=2 nodes=99\n"),
            ("endgame", 0, 7),
        )
        self.assertEqual(
            line_key("PEGSTRUCTORACLE bag=3 pos=8 complete=1\n"),
            ("peg", 3, 8),
        )
        self.assertIsNone(line_key("PEGSTRUCTCFG start=8 bags=3..3\n"))

    def test_only_resolved_roots_replace_base(self) -> None:
        base = [
            "header\n",
            "EGCURVEPOINT pos=1 depth=1 nodes=10\n",
            "EGCURVETRUNC pos=1 depth=1\n",
            "PEGSTRUCTSTAGE bag=2 pos=4 requested_stage=0\n",
            "PEGSTRUCTPARTIAL bag=2 pos=4 partial=1\n",
            "PEGSTRUCTSTAGE bag=2 pos=5 requested_stage=0\n",
            "PEGSTRUCTPARTIAL bag=2 pos=5 partial=1\n",
        ]
        overlay = [
            "MODECONT_ACCEPT mode=endgame bag=0 index=1 resolved=1\n",
            "EGCURVEPOINT pos=1 depth=1 nodes=20\n",
            "EGCURVEPOS pos=1 points=1\n",
            "MODECONT_ACCEPT mode=peg bag=2 index=4 resolved=1\n",
            "PEGSTRUCTSTAGE bag=2 pos=4 requested_stage=0\n",
            "PEGSTRUCT bag=2 pos=4 bestw=0.5\n",
            "MODECONT_ACCEPT mode=peg bag=2 index=5 resolved=0\n",
            "PEGSTRUCTSTAGE bag=2 pos=5 requested_stage=0\n",
            "PEGSTRUCTPARTIAL bag=2 pos=5 partial=1\n",
        ]
        result = "".join(merged_lines(base, overlay))
        self.assertNotIn("nodes=10", result)
        self.assertIn("nodes=20", result)
        self.assertNotIn("PEGSTRUCTPARTIAL bag=2 pos=4", result)
        self.assertIn("PEGSTRUCT bag=2 pos=4", result)
        self.assertEqual(result.count("PEGSTRUCTPARTIAL bag=2 pos=5"), 1)

    def test_resolved_root_requires_complete_terminal(self) -> None:
        with self.assertRaisesRegex(ValueError, "complete terminal"):
            merged_lines(
                ["EGCURVETRUNC pos=3 depth=1\n"],
                [
                    "MODECONT_ACCEPT mode=endgame bag=0 index=3 resolved=1\n",
                    "EGCURVEPOINT pos=3 depth=1 nodes=20\n",
                ],
            )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3

import pathlib
import sys
import unittest


TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from freeze_peg_autoplay_panel import choose_balanced, parse_record  # noqa: E402


class FreezePegAutoplayPanelTest(unittest.TestCase):
    def test_parse_record_preserves_cgp_spaces(self):
        row = parse_record(
            "PEG_PANEL\tsource_index=2\tsource=g.gcg\tevent=8\tturn=7"
            "\tbag=3\tcgp=15/15 ABC/DEF 10/9 0 -lex CSW24"
        )
        self.assertEqual(row["bag"], 3)
        self.assertEqual(
            row["cgp"], "15/15 ABC/DEF 10/9 0 -lex CSW24"
        )

    def test_balancing_uses_each_source_at_most_once(self):
        rows = []
        for source in range(12):
            for bag in range(1, 5):
                rows.append(
                    {
                        "source_index": source,
                        "source": f"g{source}.gcg",
                        "event": bag,
                        "turn": bag,
                        "bag": bag,
                        "cgp": f"cgp-{source}-{bag}",
                    }
                )
        selected = choose_balanced(rows, 2)
        self.assertEqual(len(selected), 8)
        self.assertEqual(
            len({row["source"] for row in selected}), len(selected)
        )
        self.assertEqual(
            {
                bag: sum(row["bag"] == bag for row in selected)
                for bag in range(1, 5)
            },
            {1: 2, 2: 2, 3: 2, 4: 2},
        )

    def test_matching_recovers_from_overlapping_bag_sources(self):
        rows = []
        bag_sources = {
            1: ["a", "b"],
            2: ["a", "c"],
            3: ["c", "d"],
            4: ["b", "e"],
        }
        for bag, sources in bag_sources.items():
            for source_index, source in enumerate(sources):
                rows.append(
                    {
                        "source_index": source_index,
                        "source": f"{source}.gcg",
                        "event": bag,
                        "turn": bag,
                        "bag": bag,
                        "cgp": f"cgp-{source}-{bag}",
                    }
                )
        selected = choose_balanced(rows, 1)
        self.assertEqual(len(selected), 4)
        self.assertEqual(
            len({row["source"] for row in selected}), len(selected)
        )


if __name__ == "__main__":
    unittest.main()

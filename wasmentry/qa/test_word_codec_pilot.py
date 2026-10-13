"""Focused correctness checks for the offline cover optimizer."""
import importlib.util
import itertools
import math
from pathlib import Path
import random
import unittest

spec = importlib.util.spec_from_file_location('pilot', Path(__file__).with_name('word-codec-pilot.py'))
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


def cost(selected, masks, costs, occupied, literal_bits):
    for index in selected:
        occupied &= ~masks[index]
    return sum(costs[index] for index in selected) + literal_bits*occupied.bit_count()


class CoverTests(unittest.TestCase):
    def test_excluding_greedy_choice_finds_smaller_cover(self):
        masks, costs = [0b0110, 0b0011, 0b1100], [10, 12, 12]
        greedy, _ = pilot.optimize_cover(masks, costs, 15, 14, 0)
        selected, stats = pilot.optimize_cover(masks, costs, 15, 14, 100)
        self.assertEqual(cost(greedy, masks, costs, 15, 14), 34)
        self.assertEqual(cost(selected, masks, costs, 15, 14), 24)
        self.assertTrue(stats['complete'])
        self.assertEqual(stats['saved_bits'], 10)

    def test_complete_search_matches_exhaustive_subsets(self):
        rng = random.Random(1234)
        for _ in range(80):
            masks = [rng.randrange(1, 256) for _ in range(8)]
            costs = [rng.randrange(1, 60) for _ in masks]
            for literal_bits in (5, 14):
                selected, stats = pilot.optimize_cover(masks, costs, 255, literal_bits, 1000)
                optimum = min(cost([index for index, take in enumerate(bits) if take],
                                   masks, costs, 255, literal_bits)
                              for bits in itertools.product((False, True), repeat=len(masks)))
                self.assertTrue(stats['complete'])
                self.assertEqual(cost(selected, masks, costs, 255, literal_bits), optimum)

    def test_timeout_retains_valid_incumbent(self):
        clock = itertools.count(step=.001).__next__
        masks, costs = [6, 3, 12], [10, 12, 12]
        chosen, stats = pilot.optimize_cover(masks, costs, 15, 14, .5, clock)
        self.assertFalse(stats['complete'])
        self.assertEqual(stats['nodes'], 0)
        self.assertEqual(cost(chosen, masks, costs, 15, 14), 34)
        for bad in (-1, math.inf, math.nan):
            with self.assertRaises(ValueError):
                pilot.optimize_cover(masks, costs, 15, 14, bad)

    def test_mixed_axes_and_literals_round_trip(self):
        cells = ['']*225
        # Horizontal ABC crosses vertical DBE, plus an isolated unknown letter.
        for square, letter in {15:'A',16:'B',17:'C',1:'D',31:'E',224:'Z'}.items():
            cells[square] = letter
        codes, tree = pilot.codebook({'ABC':10, 'DBE':10})
        for bitmap in (False, True):
            baseline, _, _ = pilot.cover(cells, codes, bitmap)
            encoded, words, literals = pilot.cover(cells, codes, bitmap, 100)
            self.assertLessEqual(len(encoded), len(baseline))
            self.assertEqual(pilot.uncover(encoded, tree, bitmap), cells)
            self.assertEqual(words, 2)
            self.assertEqual(literals, 1)
        for empty in (['']*225, ['Q']+['']*224):
            encoded, _, _ = pilot.cover(empty, codes, False, 100)
            self.assertEqual(pilot.uncover(encoded, tree, False), empty)


if __name__ == '__main__':
    unittest.main()

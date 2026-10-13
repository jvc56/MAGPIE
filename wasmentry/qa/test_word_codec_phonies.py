"""Cycle 10% of board letters in freshly generated native CSW24 positions.

The native regression requires word-frequency-pilot and CSW24.txt; see the
study's reproduction instructions. Small mutation checks need neither.
"""
from collections import Counter
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

from test_word_codec_pilot import pilot

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / 'bin/word-frequency-pilot'
LEXICON = ROOT / 'data/lexica/CSW24.txt'


def cycle_letters(cells, rng):
    occupied = [index for index, letter in enumerate(cells) if letter]
    selected = rng.sample(occupied, (len(occupied)+9)//10)
    mutated = cells.copy()
    # Move the complete tile, including a lowercase blank designation. Exactly
    # ceil(10%) squares participate, although duplicate letters can look unchanged.
    for index, destination in enumerate(selected):
        mutated[destination] = cells[selected[index-1]]
    return mutated, selected


class PhonyMutationTests(unittest.TestCase):
    def test_round_up_and_cycle_preserves_inventory_and_blanks(self):
        for count in (0, 1, 9, 10, 11, 20, 21, 99, 100):
            cells = (list('aBCDEfGHIJ')*10)[:count] + ['']*(225-count)
            mutated, selected = cycle_letters(cells, random.Random(42))
            self.assertEqual(len(selected), (count+9)//10)
            self.assertEqual(len(set(selected)), len(selected))
            self.assertEqual(Counter(mutated), Counter(cells))
            self.assertEqual([bool(tile) for tile in mutated], [bool(tile) for tile in cells])
            for index, square in enumerate(selected):
                self.assertEqual(mutated[square], cells[selected[index-1]])
            for square in set(range(225))-set(selected):
                self.assertEqual(mutated[square], cells[square])
            if len(selected) == 1:
                self.assertEqual(mutated, cells)  # A one-letter cycle is a no-op.

    @unittest.skipUnless(GENERATOR.is_file() and LEXICON.is_file(),
                         'build word-frequency-pilot and export CSW24.txt to run native phony regression')
    def test_native_positions_with_ten_percent_letters_cycled(self):
        words = set(LEXICON.read_text().upper().split())
        with tempfile.TemporaryDirectory(prefix='magpie-phony-codec-') as directory:
            counts_path = Path(directory)/'counts.tsv'
            positions_path = Path(directory)/'positions.cgp'
            # Independent of the million training games and prior held-out seeds.
            subprocess.run([str(GENERATOR), '1020000', '1000', str(counts_path),
                            str(positions_path), 'CSW24'], cwd=ROOT, check=True,
                           capture_output=True, text=True, timeout=120)
            # Freeze a model from the unmodified games; never teach it mutations.
            counts = {word:int(count) for word, count in
                      (line.split('\t') for line in counts_path.read_text().splitlines())}
            codes, tree = pilot.codebook(counts)
            positions = positions_path.read_text().splitlines()
            self.assertGreaterEqual(len(positions), 20)
            rng = random.Random(20261013)
            phony_positions = unknown_positions = literal_positions = round_trips = 0
            blank_positions = 0
            for position_index, cgp in enumerate(positions):
                cells, _, _, _ = pilot.parse(cgp)
                self.assertTrue(all(word in words for _, word in pilot.runs(cells)))
                mutated, selected = cycle_letters(cells, rng)
                self.assertEqual(len(selected), (sum(bool(tile) for tile in cells)+9)//10)
                self.assertEqual(Counter(mutated), Counter(cells))
                self.assertEqual([bool(tile) for tile in mutated], [bool(tile) for tile in cells])
                phony_positions += any(word not in words for _, word in pilot.runs(mutated))
                unknown_positions += any(word not in codes for _, word in pilot.runs(mutated))
                blank_positions += any(letter.islower() for letter in mutated)
                used_literals = False
                for bitmap in (False, True):
                    for budget in (0, 10):
                        with self.subTest(position=position_index, bitmap=bitmap, budget=budget):
                            encoded, _, literals = pilot.cover(mutated, codes, bitmap, budget)
                            decoded = pilot.uncover(encoded, tree, bitmap)
                            self.assertEqual(decoded, [letter.upper() for letter in mutated])
                            # Board words encode letter identity; the existing wire
                            # format stores blank locations separately.
                            for square, letter in enumerate(mutated):
                                if letter.islower():
                                    decoded[square] = decoded[square].lower()
                            self.assertEqual(decoded, mutated)
                            used_literals |= literals > 0
                            round_trips += 1
                literal_positions += used_literals
            # Ensure this corpus really exercised phonies (checked against the
            # lexicon, not merely absence from the frequency codebook), fallbacks,
            # and blanks. A random rotation can occasionally leave all words legal.
            self.assertGreater(phony_positions, len(positions)//2)
            self.assertGreater(unknown_positions, 0)
            self.assertGreater(literal_positions, 0)
            self.assertGreater(blank_positions, 0)
            print(f'Native mutation regression: {len(positions)} positions, '
                  f'{phony_positions} with phonies, {literal_positions} using literals, '
                  f'{blank_positions} with blanks, {round_trips} round trips')


if __name__ == '__main__':
    unittest.main()

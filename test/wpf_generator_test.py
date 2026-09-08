"""Run with: python3 -m unittest discover -s test -p wpf_generator_test.py"""

from collections import defaultdict
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

GENERATOR_PATH = Path(__file__).resolve().parents[1] / "tools/generate_wpf.py"
SPEC = importlib.util.spec_from_file_location("generate_wpf", GENERATOR_PATH)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


def write_wit(path, words, dimension=5):
    """Small independent v3 trie encoder; reverse IDs test nonalphabetic layout."""
    output = bytearray(struct.pack("<BBHQ", 3, dimension, 0, 123))
    for length in range(1, dimension + 1):
        selected = sorted(word for word in words if len(word) == length)
        value_ids = {word: value for value, word in enumerate(reversed(selected))}
        trie = {}
        for word in selected:
            node = trie
            for letter in word:
                node = node.setdefault(letter, {})
        nodes = [(0, 0, 0, -1)]

        def flatten(branch, prefix):
            if not branch:
                return 0
            first = len(nodes)
            letters = sorted(branch)
            nodes.extend([None] * len(letters))
            for offset, letter in enumerate(letters):
                word = prefix + letter
                child = flatten(branch[letter], word)
                nodes[first + offset] = (ord(letter) - 64, int(offset == len(letters) - 1),
                                         child, value_ids.get(word, -1))
            return first

        root = flatten(trie, "")
        output.extend(struct.pack("<III", len(nodes), root, len(selected)))
        output.extend(bytes(node[0] for node in nodes))
        output.extend(bytes(node[1] for node in nodes))
        output.extend(struct.pack("<" + "I" * len(nodes), *(node[2] for node in nodes)))
        output.extend(struct.pack("<" + "i" * len(nodes), *(node[3] for node in nodes)))
        output.extend(bytes(len(selected) * (dimension - length + 1) * 4))
    path.write_bytes(output)


class WpfGeneratorTest(unittest.TestCase):
    WORDS = {"AT", "CAT", "SCAT", "CHAT", "CATC", "ABA", "ABABA", "ATAT", "TATA", "TATAT"}

    def test_every_tiny_cell_against_literal_oracle(self):
        # Includes repeated letters, multiple occurrences of the same base,
        # adjacent/gapped floaters on either side, zero cells and no length4 rows.
        maps = {2: {"AT": 0}, 3: {"ABA": 0}, 4: {}}
        tables = GENERATOR.construct(sorted(self.WORDS), 5, maps)
        by_length = defaultdict(list)
        for word in self.WORDS:
            by_length[len(word)].append(word)
        for length, keys in maps.items():
            for base, row in keys.items():
                for final_length in range(length + 1, 6):
                    for delta in range(-5, 6):
                        for floater in range(1, 27):
                            cell = GENERATOR.cell_index(length, final_length, delta, floater)
                            if cell is None:
                                continue
                            actual = tables[length][row * GENERATOR.cells_per_base(5, length) + cell]
                            expected = GENERATOR.reference_mask(by_length, base, chr(64 + floater), delta, final_length)
                            self.assertEqual(actual, expected, (base, final_length, delta, floater))
        self.assertEqual(tables[2][GENERATOR.cell_index(2, 4, -1, 3)], (1 << 19) | (1 << 3))
        self.assertEqual(tables[2][GENERATOR.cell_index(2, 4, -2, 3)], 1 << 8)
        self.assertEqual(tables[3][GENERATOR.cell_index(3, 5, 3, 2)], 1 << 1)

    def test_sparse_rows_keep_wit_order_and_zero_distinct_from_missing(self):
        maps = {2: {"AT": 1, "AA": 0}, 3: {"ABA": 0}, 4: {}}
        selected = GENERATOR.select_bases(maps, {"AT", "ABA"})
        self.assertEqual(selected, {2: {"AT": 0}, 3: {"ABA": 0}, 4: {}})
        tables = GENERATOR.construct([], 5, selected)
        with tempfile.TemporaryFile() as stream:
            GENERATOR.write_table(stream, 5, 123, 456, maps, selected, tables)
            stream.seek(0)
            data = stream.read()
        self.assertEqual(GENERATOR.HEADER.unpack_from(data), (b"WPFM2LE\0", 5, 2, 4, 0, 123, 456))
        cursor = GENERATOR.HEADER.size
        self.assertEqual(GENERATOR.SECTION.unpack_from(data, cursor), (2, 2, 312, 1))
        cursor += GENERATOR.SECTION.size
        self.assertEqual(struct.unpack_from("<ii", data, cursor), (-1, 0))
        self.assertEqual(data[cursor + 8:cursor + 8 + 312 * 4], bytes(312 * 4))

    def test_dictionary_mismatch_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wit = root / "tiny.wit"
            dictionary = root / "tiny.txt"
            output = root / "tiny.wpf"
            write_wit(wit, self.WORDS)
            dimension, fingerprint, _, maps, words = GENERATOR.read_wit(wit)
            self.assertEqual((dimension, fingerprint, words), (5, 123, self.WORDS))
            self.assertEqual(maps[4]["TATA"], 0)
            command = [sys.executable, str(GENERATOR_PATH), "--wit", str(wit),
                       "--dictionary", str(dictionary), "--output", str(output)]
            for bad_words in (self.WORDS - {"CAT"}, self.WORDS | {"DOG"}):
                dictionary.write_text("\n".join(sorted(bad_words)) + "\n")
                if output.exists():
                    output.unlink()
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("complete WIT terminal set", result.stderr)
                self.assertFalse(output.exists())
                output.write_bytes(b"preserve existing table")
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_bytes(), b"preserve existing table")
            dictionary.write_text("\n".join(sorted(self.WORDS)) + "\n")
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            expected = output.read_bytes()
            dictionary.write_text("\n".join(sorted(self.WORDS, reverse=True)) + "\nAT\n")
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(output.read_bytes(), expected)
            self.assertFalse(list(root.glob("*.tmp")))

    def test_truncated_wit_and_unknown_bases(self):
        with tempfile.TemporaryDirectory() as directory:
            wit = Path(directory) / "tiny.wit"
            write_wit(wit, self.WORDS)
            complete = wit.read_bytes()
            for size in (0, 11, 20, len(complete) - 1):
                wit.write_bytes(complete[:size])
                with self.assertRaises(ValueError):
                    GENERATOR.read_wit(wit)
        with self.assertRaises(ValueError):
            GENERATOR.select_bases({2: {"AT": 0}, 3: {}, 4: {}}, {"CAT"})


if __name__ == "__main__":
    unittest.main()

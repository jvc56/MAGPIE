#!/usr/bin/env python3
"""Generate an English WordPlusFloater table from a matching text lexicon/WIT.

Uses only the Python standard library. See tools/WPF.md for usage and format.
"""

import argparse
import array
from collections import Counter, defaultdict
import hashlib
import json
import mmap
from pathlib import Path
import random
import struct
import sys
import tempfile

MIN_BASE_LENGTH = 2
MAX_BASE_LENGTH = 4
ALPHABET_SIZE = 26
HEADER = struct.Struct("<8sIIIIQQ")
SECTION = struct.Struct("<IIII")
FNV_INITIAL = 1469598103934665603
MASK64 = (1 << 64) - 1


def fnv_bytes(state, data):
    for byte in data:
        state = ((state ^ byte) * 1099511628211) & MASK64
    return state


def cells_per_base(dimension, length):
    extension = dimension - length
    return ALPHABET_SIZE * extension * (extension + 1)


def cell_index(base_length, final_length, delta, floater):
    """Index one signed position, or None for an overlapping/outside floater."""
    extension = final_length - base_length
    if not 1 <= floater <= ALPHABET_SIZE or extension < 1:
        return None
    if delta < 0:
        if delta < -extension:
            return None
        offset = delta + extension
    else:
        if delta < base_length or delta >= final_length:
            return None
        offset = extension + delta - base_length
    return (extension * (extension - 1) + offset) * ALPHABET_SIZE + floater - 1


def read_wit(path):
    """Read terminal IDs and fingerprints; ordinary addable masks are unused."""
    with path.open("rb") as stream:
        if stream.seek(0, 2) < 12:
            raise ValueError("Truncated WIT header")
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            version, dimension, reserved, kwg_hash = struct.unpack_from("<BBHQ", data)
            if version != 3 or reserved or not kwg_hash or dimension < MAX_BASE_LENGTH:
                raise ValueError("Expected a fingerprinted v3 WIT with board dimension >= 4")
            cursor = 12
            key_maps = {}
            all_words = set()
            layout_hash = FNV_INITIAL
            for length in range(1, dimension + 1):
                if cursor + 12 > len(data):
                    raise ValueError("Truncated WIT section")
                nodes, root, values = struct.unpack_from("<III", data, cursor)
                cursor += 12
                tiles_pos = cursor
                lasts_pos = tiles_pos + nodes
                children_pos = lasts_pos + nodes
                values_pos = children_pos + nodes * 4
                arrays_end = values_pos + nodes * 4
                cursor = arrays_end + values * (dimension - length + 1) * 4
                if not nodes or root >= nodes or cursor > len(data):
                    raise ValueError("Invalid or truncated WIT trie")
                if MIN_BASE_LENGTH <= length <= MAX_BASE_LENGTH:
                    layout_hash = fnv_bytes(layout_hash, struct.pack("<IIII", length, nodes, root, values))
                    layout_hash = fnv_bytes(layout_hash, data[tiles_pos:arrays_end])
                mapping = {}
                visited = set()
                pending = [(root, "")] if root else []
                while pending:
                    node, prefix = pending.pop()
                    while True:
                        if not 0 < node < nodes or node in visited:
                            raise ValueError("Invalid or cyclic WIT node reference")
                        visited.add(node)
                        letter = data[tiles_pos + node]
                        if not 1 <= letter <= ALPHABET_SIZE:
                            raise ValueError("Only English machine letters A-Z (1..26) are supported")
                        word = prefix + chr(64 + letter)
                        value = struct.unpack_from("<i", data, values_pos + node * 4)[0]
                        if value >= 0:
                            if len(word) != length or value >= values or word in mapping:
                                raise ValueError("Invalid WIT terminal")
                            mapping[word] = value
                            all_words.add(word)
                        child = struct.unpack_from("<I", data, children_pos + node * 4)[0]
                        if child:
                            if len(word) >= length:
                                raise ValueError("WIT key exceeds its section length")
                            pending.append((child, word))
                        last = data[lasts_pos + node]
                        if last not in (0, 1):
                            raise ValueError("Invalid WIT sibling flag")
                        if last:
                            break
                        node += 1
                if sorted(mapping.values()) != list(range(values)):
                    raise ValueError("WIT terminal IDs must be a dense permutation")
                if MIN_BASE_LENGTH <= length <= MAX_BASE_LENGTH:
                    key_maps[length] = mapping
            if cursor != len(data):
                raise ValueError("Unexpected trailing WIT data")
    return dimension, kwg_hash, layout_hash, key_maps, all_words


def read_words(path):
    words = set()
    for line_number, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        word = line.strip()
        if not word:
            continue
        if not all("A" <= letter <= "Z" for letter in word):
            raise ValueError(f"{path}:{line_number}: expected an uppercase A-Z word")
        words.add(word)
    return words


def select_bases(key_maps, selected):
    known = {word for mapping in key_maps.values() for word in mapping}
    if selected is None:
        selected = known
    if not selected <= known:
        raise ValueError("Base list contains words absent from WIT lengths 2..4: " +
                         ", ".join(sorted(selected - known)[:10]))
    # Rows always follow original WIT value IDs, independent of input list order.
    return {length: {word: row for row, word in enumerate(
        word for word, _ in sorted(mapping.items(), key=lambda item: item[1])
        if word in selected)} for length, mapping in key_maps.items()}


def construct(words, dimension, key_maps):
    """Union letters outside each fixed base occurrence and floater occurrence."""
    tables = {length: array.array("I", [0]) * (len(keys) * cells_per_base(dimension, length))
              for length, keys in key_maps.items()}
    if any(table.itemsize != 4 for table in tables.values()):
        raise ValueError("Host unsigned int must be 32 bits")
    base_counts = {base: Counter(base) for keys in key_maps.values() for base in keys}
    for word in words:
        final_length = len(word)
        prefix = [0]
        for letter in word:
            prefix.append(prefix[-1] | (1 << (ord(letter) - 64)))
        suffix = [0] * (final_length + 1)
        for position in range(final_length - 1, -1, -1):
            suffix[position] = suffix[position + 1] | (1 << (ord(word[position]) - 64))
        word_counts = Counter(word)
        for length, keys in key_maps.items():
            if final_length <= length:
                continue
            table = tables[length]
            stride = cells_per_base(dimension, length)
            for start in range(final_length - length + 1):
                base = word[start:start + length]
                row = keys.get(base)
                if row is None:
                    continue
                residual = prefix[start] | suffix[start + length]
                counts = base_counts[base]
                for position, letter in enumerate(word):
                    if start <= position < start + length:
                        continue
                    floater = ord(letter) - 64
                    cell = cell_index(length, final_length, position - start, floater)
                    mask = residual
                    # Remove this occurrence only: another matching letter
                    # outside the fixed base and floater remains addable.
                    if word_counts[letter] - counts.get(letter, 0) == 1:
                        mask &= ~(1 << floater)
                    table[row * stride + cell] |= mask
    return tables


def reference_mask(words_by_length, base, floater, delta, final_length):
    """Independent literal-position oracle; intentionally does not use counts."""
    mask = 0
    for word in words_by_length.get(final_length, []):
        for start in range(final_length - len(base) + 1):
            position = start + delta
            if not 0 <= position < final_length or word[position] != floater:
                continue
            if word[start:start + len(base)] != base:
                continue
            for other, letter in enumerate(word):
                if other != position and not start <= other < start + len(base):
                    mask |= 1 << (ord(letter) - 64)
    return mask


def verify_sample(words, dimension, key_maps, tables, count=12):
    random_source = random.Random(619624)
    by_length = defaultdict(list)
    for word in words:
        by_length[len(word)].append(word)
    checked = 0
    for length, keys in key_maps.items():
        if not keys or length == dimension:
            continue
        for _ in range(count):
            base = random_source.choice(sorted(keys))
            final_length = random_source.randrange(length + 1, dimension + 1)
            extension = final_length - length
            delta = random_source.choice(list(range(-extension, 0)) + list(range(length, final_length)))
            floater = random_source.randrange(1, ALPHABET_SIZE + 1)
            index = keys[base] * cells_per_base(dimension, length) + cell_index(length, final_length, delta, floater)
            expected = reference_mask(by_length, base, chr(64 + floater), delta, final_length)
            if tables[length][index] != expected:
                raise ValueError(f"Reference mismatch: {base}, {final_length}, {delta}, {floater}")
            checked += 1
    return checked


def write_table(stream, dimension, kwg_hash, layout_hash, original_maps, selected_maps, tables):
    stream.write(HEADER.pack(b"WPFM2LE\0", dimension, MIN_BASE_LENGTH, MAX_BASE_LENGTH,
                             0, kwg_hash, layout_hash))
    for length, originals in original_maps.items():
        selected = selected_maps[length]
        stride = cells_per_base(dimension, length)
        if len(selected) > (1 << 31) - 1 or len(tables[length]) * 4 > (1 << 32) - 1:
            raise ValueError("WPF section exceeds the supported 32-bit size")
        stream.write(SECTION.pack(length, len(originals), stride, len(selected)))
        remap = array.array("i", [-1]) * len(originals)
        if remap.itemsize != 4:
            raise ValueError("Host signed int must be 32 bits")
        for word, row in selected.items():
            remap[originals[word]] = row
        for values in (remap, tables[length]):
            if sys.byteorder != "little":
                values = values[:]
                values.byteswap()
            values.tofile(stream)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wit", type=Path, required=True, help="Matching fingerprinted .wit")
    parser.add_argument("--dictionary", type=Path, required=True, help="Complete uppercase A-Z text lexicon")
    parser.add_argument("--bases", type=Path, help="One selected length2..4 base per line; default: all bases")
    parser.add_argument("--output", type=Path, required=True, help="Output NAME.wpf")
    args = parser.parse_args()
    inputs = [args.wit, args.dictionary] + ([args.bases] if args.bases else [])
    if args.output.resolve() in {path.resolve() for path in inputs}:
        parser.error("Output must differ from all inputs")
    try:
        dimension, kwg_hash, layout_hash, original_maps, wit_words = read_wit(args.wit)
        words = read_words(args.dictionary)
        # Longer words are outside this board's dictionary, as in WIT creation.
        words = sorted(word for word in words if len(word) <= dimension)
        if set(words) != wit_words:
            raise ValueError("Text lexicon differs from the complete WIT terminal set")
        selected_maps = select_bases(original_maps, read_words(args.bases) if args.bases else None)
        tables = construct(words, dimension, selected_maps)
        verified = verify_sample(words, dimension, selected_maps, tables)
        # Replace atomically so an interrupted generator cannot leave a partial
        # optional table where the engine will try to load it.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=args.output.name + ".",
                                             suffix=".tmp", delete=False) as stream:
                temporary = Path(stream.name)
                write_table(stream, dimension, kwg_hash, layout_hash, original_maps, selected_maps, tables)
            temporary.replace(args.output)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        print(json.dumps(dict(output=str(args.output), sha256=sha256(args.output),
                              bytes=args.output.stat().st_size, dimension=dimension,
                              kwg_hash=kwg_hash, layout_hash=layout_hash,
                              bases={length: len(keys) for length, keys in selected_maps.items()},
                              reference_queries=verified), indent=2))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"WPF generation failed: {error}\n")


if __name__ == "__main__":
    main()

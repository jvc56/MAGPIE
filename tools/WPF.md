# WordPlusFloater tables

WPF adds an optional positional filter to WIT move generation. A cell fixes a
contiguous base word, one other board letter at a signed offset, and the final
word length. Its mask contains letters occurring outside those fixed tile
occurrences in any matching dictionary word. Intersecting these masks with the
ordinary WIT mask can rule out racks and subracks before wordmap lookup.

## Generation

The native C maker follows the WMP and RIT conversion path. It reads the KWG's
complete DAWG and the matching WIT, then writes `NAME.wpf`. Every dictionary
base of lengths two through four receives a complete row; no frequency list
or cutoff is needed.

First rebuild ordinary WIT with this engine's residual-letter masks. The
release build preserves an existing WIT, so regenerate it explicitly:

```sh
make magpie BUILD=no_pgo_release
printf 'convert kwg2wit CSW24\nconvert kwg2wpf CSW24\n' | \
  bin/magpie 'set -lex CSW24 -wmp true -rit false -wit false'
make release
```

The generator checks KWG identity and the complete WIT terminal set, including
word lengths outside the indexed bases. It supports machine letters 1..26;
words longer than the build's board dimension are outside its dictionary.
The output is published by renaming a temporary sibling file, so validation
and write failures preserve an existing table. An old WPF is not loaded during
conversion and cannot prevent its replacement. No separate text lexicon or
Python runtime is required.

Put `NAME.wpf` beside the lexicon data under the configured data paths. The
engine loads it when that lexicon's WIT is used. Missing or foreign-lexicon
WPF files retain ordinary WIT behavior. A matching but malformed file reports
a load error. Regenerate WPF whenever regenerating WIT, and build/train PGO
with the same table that will be used for production work.

The 15-square CSW24 table covers all 7,140 two-to-four-letter bases and uses
102,050,416 bytes (97.32 MiB) of shared mask memory. With the matching WIT
layout, the file is 102,050,504 bytes with SHA-256
`91b5216948a30e567e528b8f6f184bd2bd9b799a1c5839c99f915b1ce11457bc`.
Its KWG fingerprint is `9283346135631466225` and WIT key-layout fingerprint is
`12770297558071208977`. A different valid trie layout can produce different
bytes; WPF always uses the matching WIT's value IDs.

Run the native generator, loader and positional-filter tests with:

```sh
make magpie_test BUILD=no_pgo_release
bin/magpie_test wpfmaker wit wmg cv
```

The maker tests compare every cell of a small dictionary against an independent
literal-position oracle, including repeated occurrences and board-edge offsets.
These tests are also in the normal C test suite.

## File format

All integers are little-endian. The generator writes dense M1 format:

| Field | Type |
| --- | --- |
| Magic `WPFM1LE` followed by NUL | 8 bytes |
| Board dimension, minimum base length (2), maximum base length (4), reserved (0) | 4 × uint32 |
| KWG fingerprint, WIT key-layout fingerprint | 2 × uint64 |

Each base length has a section of four uint32 values: base length, WIT value
count, cells per row, and reserved (0). It is followed by a row of uint32 masks
for every WIT value, in original value-ID order. Bit 1 denotes machine letter 1,
bit 26 denotes machine letter 26, and bit 0 is unused. All-zero rows remain
present: a loaded zero cell is an exact empty addable set, whereas a missing
table supplies no positional constraint.

For base length `b`, final length `n`, and extension `e = n - b`, valid floater
offsets relative to the base start are `[-e, -1]` and `[b, n - 1]`. Offsets inside
the base are excluded. Their local offset index is `delta + e` on the left or
`e + delta - b` on the right. The cell index is
`(e * (e - 1) + offset_index) * 26 + machine_letter - 1`.
A row has `26 * (dimension - b) * (dimension - b + 1)` cells.

The layout fingerprint uses the engine's FNV recurrence, initial state
`1469598103934665603`, multiplier `1099511628211`, modulo 2^64. For each complete
WIT length-2 through length-4 trie, hash its length, node count, root and value
count as four little-endian uint32 values, followed by its tile-byte array,
sibling-last-byte array, uint32 children and int32 value IDs. Ordinary WIT
addable masks are excluded. The engine verifies the board dimension, KWG and
layout fingerprints before using any row.

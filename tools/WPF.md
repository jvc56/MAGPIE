# WordPlusFloater tables

WordPlusFloater adds an optional positional filter to WIT move generation. A
cell fixes a contiguous base word, one other board letter at a signed offset,
and the final word length. Its mask contains letters occurring outside those
fixed tile occurrences in any matching dictionary word. Intersecting these
masks with the ordinary WIT mask can rule out racks and subracks before wordmap
lookup.

## Generation

The native C maker follows the WMP and RIT conversion path. A single
`convert kwg2wit NAME` reads the KWG's complete DAWG and builds both ordinary
and positional masks in `NAME.wit`. Every dictionary base of lengths two
through four receives a complete positional row; no frequency list or cutoff
is needed.

The release build creates a WIT when missing and preserves an existing one.
To upgrade an existing version-3 WIT, regenerate it explicitly before training
PGO:

```sh
make magpie BUILD=no_pgo_release
printf 'convert kwg2wit CSW24\n' | \
  bin/magpie 'set -lex CSW24 -wmp true -rit false -wit false'
make release
```

Version-3 WITs remain readable and retain ordinary WIT filtering. Version-4
WITs contain the positional masks as an optional section in the same file;
there is no separate positional file to generate, distribute or match. The
maker omits that section for supported ordinary-WIT alphabets outside machine
letters 1..26, retaining ordinary WIT behavior. Words longer than the build's board dimension are
outside its playable dictionary.

The maker checks the KWG identity and complete WIT terminal set, including
lengths outside the indexed bases. Conversion creates both from the same KWG
and publishes the complete file by renaming a temporary sibling, so validation
and write failures preserve an existing table. An old WIT is not loaded during
conversion and cannot prevent its replacement. No separate text lexicon or
Python runtime is required. Build/train PGO with the same WIT that will be
used for production work.

The 15-square CSW24 positional section covers all 7,140 two-to-four-letter
bases and uses 102,050,416 bytes (97.32 MiB) of shared mask memory. Both kinds
of mask use the WIT's terminal IDs and one KWG fingerprint.

Run the native maker, combined-file loader and positional-filter tests with:

```sh
make magpie_test BUILD=no_pgo_release
bin/magpie_test wpfmaker wit wmg cv
```

The maker tests compare every cell of a small dictionary against an independent
literal-position oracle, including repeated occurrences and board-edge offsets.
The file tests cover ordinary and positional roundtrips, version-3 fallback,
zero rows, malformed/truncated files and preserving existing output on failure.
These tests are also in the normal C test suite.

## WIT version 4 file format

All integers are little-endian. The 12-byte header is:

| Field | Type |
| --- | --- |
| Version (4), board dimension, flags, reserved (0) | 4 × uint8 |
| KWG fingerprint | uint64 |

Flag bit 0 indicates complete positional data for base lengths 2 through 4.
All other flag bits must be zero. The ordinary trie sections follow unchanged
from version 3, one per length 1 through the board dimension. Each begins with
node count, root and value count (three uint32 values), then tile bytes,
sibling-last bytes, uint32 children, int32 terminal IDs and uint32 ordinary
masks. An ordinary row has `dimension - base_length + 1` masks.

If flag bit 0 is set, append the positional rows for lengths 2, 3 and 4, in that
order. Each uses its trie's original value-ID order. The ordinary trie already
defines the number and identity of rows, so there are no duplicate section
headers, layout fingerprints or remapping tables. Missing base lengths take no
space. Bit 1 denotes machine letter 1, bit 26 denotes machine letter 26, and
bit 0 is unused. All-zero rows remain present: a loaded zero cell is an exact
empty addable set, whereas an absent section supplies no positional constraint.

For base length `b`, final length `n`, and extension `e = n - b`, valid floater
offsets relative to the base start are `[-e, -1]` and `[b, n - 1]`. Offsets inside
the base are excluded. Their local offset index is `delta + e` on the left or
`e + delta - b` on the right. The cell index is
`(e * (e - 1) + offset_index) * 26 + machine_letter - 1`.
A row has `26 * (dimension - b) * (dimension - b + 1)` cells.

The loader validates the version, flags, dimension, trie structure and exact
payload size before exposing a table. The normal lexicon loader pairs the WIT
with its KWG fingerprint. Unknown flags, malformed tries, truncated sections
and trailing bytes are load errors; a valid legacy WIT or version-4 WIT without
the optional section keeps ordinary filtering.

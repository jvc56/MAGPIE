# Native XSP / XSG codecs

MAGPIE's `conv` commands create canonical word codebooks, encode CGP positions
as XSP, and encode GCG games as XSG. Output tokens use **unpadded base64url**:
`A-Z a-z 0-9 - _`. No escaping is necessary in a URL parameter value.

These are new native wire formats: **XSP version 5** and **XSG version 2**.
The earlier browser packed-position links and offline research-pilot tokens
are not these formats. Do not replace a browser decoder with this decoder
without explicitly supporting its old versions. Unsupported versions return
an error; they are never guessed or decoded using the current model.

## Build and commands

No additional libraries, Python, or environment variables are required.
CRC32 and bounded gzip/DEFLATE reading are implemented in C. The codebook
writer emits deterministic stored DEFLATE blocks: the canonical JSON and
Huffman lengths reproduce the published model, but the larger gzip wrapper
differs from the original compressed assets. Keep using the published assets
for registered profiles; their full file hashes remain pinned.

```sh
make magpie BUILD=no_pgo_release
./bin/magpie conv counts2xsw csw24-counts.tsv.gz /tmp/CSW24.xsw.json.gz CSW24
./bin/magpie conv counts2xsw nwl23-counts.tsv.gz /tmp/NWL23.xsw.json.gz NWL23
./bin/magpie conv cgp2xsp position.cgp position.xsp CSW24 20
./bin/magpie conv xsp2cgp position.xsp restored.cgp
./bin/magpie conv gcg2xsg game.gcg.gz game.xsg CSW24
./bin/magpie conv xsg2gcg game.xsg restored.gcg
```

All paths above are explicit file paths, including the output path. Encoding
profiles are `CSW24` (default), `NWL23`, or `none`. `none` produces a standalone
position bitmap or an explicit-move game requiring no XS assets. Word-based
positions use the requested codebook even when their rules lexicon differs.
Games can similarly use CSW24 ranks for NWL23 games, and explicitly encode a
move that is unavailable in the ranking lexicon.

The optional final `cgp2xsp` argument is the additional optimization budget in
milliseconds (0–60000, default 20). A greedy incumbent is always available;
a bounded search considers mixed horizontal/vertical covers, and the encoder
chooses the smaller of word-cover and packed-board representations. Time-based
search may find different equally valid encodings on different machines.
Decoding has no optimization step or time budget. Use 0 for reproducible
encoding. The budget excludes codebook loading and initial candidate creation.

`counts2xsw` accepts plain or gzip-compressed `WORD<TAB>COUNT` lines. Words must
be unique uppercase English words of length 2–15; counts are positive uint64
integers whose sum fits uint64. Input need not be sorted. The source SHA256 is
over the **exact uncompressed TSV**, including its original order and newlines.
The Huffman priority queue breaks weight ties by the alphabetically smallest
word in a subtree. Code lengths over 32 are rejected. Output uses the
`xs-word-codebook` JSON version 1 contract documented in MAGPIE-DATA.

The builder can generate candidate models from arbitrary counts; naming one
`en-CSW24-v1` does **not** register it as the published model. Runtime loaders
accept only the registered asset checksums. New published models need new
immutable paths and new codec IDs; never replace the registered v1 data.

## Data contract and installation

The only added codec data are the six assets in
[MAGPIE-DATA #17](https://github.com/jvc56/MAGPIE-DATA/pull/17):

- `xs/english-v1/CSW24.xsw.json.gz` and `NWL23.xsw.json.gz` for word-based XSP.
- `xs/english-v1/CSW24.kwg`, `CSW24.klv2`, `NWL23.kwg`, `NWL23.klv2` for ranked XSG.

`xs/manifest.json` documents their lengths and SHA256 hashes. Runtime loaders
verify their registered checksums before use. KLV/KWG aliases initially point
to `../../lexica/…`; distribution bundles must include those targets or
replace the aliases with the exact frozen bytes. Ordinary KLV updates must
first preserve the old XS bytes. No WMP, WIT, RIT, separate draw table, or new
board/distribution file is required. Ranking rules embed the standard board
and English distribution in the core codec.

Until a data release includes XS, use a MAGPIE-DATA checkout explicitly:

```sh
git clone https://github.com/jvc56/MAGPIE-DATA.git /tmp/magpie-xs-data
git -C /tmp/magpie-xs-data checkout c4216b80098e8c7e88262cb5d04d0bc033bc4d46
./bin/magpie conv cgp2xsp position.cgp position.xsp \
  -path /tmp/magpie-xs-data/data:data
./bin/magpie_test xsdata:/tmp/magpie-xs-data/data
```

**Do not expect `download_data.sh`'s existing versioned archives to contain
XS.** The dedicated CI job checks out the merged commit directly. Native tests
load and verify all six pinned assets without an external validation script.

## Common wire primitives

Fields are bit-packed most significant bit first. The packet is three ASCII
magic bytes (`XSP` or `XSG`), the body, zero bits to the next byte boundary,
and a four-byte big-endian IEEE CRC32 over everything preceding the CRC.
CRC detects accidental damage; it is not authentication. Base64url padding,
nonzero unused base64 bits, trailing body bytes, and nonzero body padding are
rejected. Limits apply before allocation (8 MiB encoded input/body, 4096 bytes
per game string, 1000 game events). Text is UTF-8 without embedded NULs.

Unsigned varints are minimal unsigned LEB128 (at most five bytes / uint32).
Signed varints use ZigZag over int32. Order-k Exp-Golomb ranks write
`floor(log2((rank >> k) + 1))` zero bits, that quotient-plus-one in binary,
then the low k rank bits. Current draw ranks use k=4; move ranks use k=2.

Metadata starts with a 4-bit language ID, then a 5-bit lexicon ID scoped to
that language. Zero in either field escapes to a length byte and UTF-8 text
(language at most 63 bytes; lexicon at most 255). IDs are append-only:

| Language ID | Language | Lexicon IDs in order, starting at 1 |
| --- | --- | --- |
| 1 | en | CSW24, NWL23, CSW07, CSW12, CSW15, CSW21, NWL20, OSW1, OSW2, OSW3, OSW4, OSWI, TWL06, TWL14, TWL98 |
| 2 | ca | DISC2 |
| 3 | nl | DSW25 |
| 4 | fr | FRA20 |
| 5 | de | RD29 |
| 6 | pl | OSPS49 |

The metadata registry is language-aware, but the current board/rack payload
supports **standard 15×15 English games only**. Other languages and board
variants must be rejected until a defined payload/profile supports them.
Unknown English lexicon names are retained literally. An empty lexicon literal
means no lexicon was specified; decoding does not invent one. Metadata strings
reject whitespace, ASCII control bytes, and invalid UTF-8.

## XSP body version 5

1. Version (4 bits), language/lexicon metadata.
2. Two scores: 10 bits each for 0–1022; sentinel 1023 followed by a signed
   varint otherwise. The entire int32 range is supported.
3. Consecutive scoreless turns (3 bits), two rack lengths (3 bits each).
4. Board mode (1 bit).
   - 0: 225 occupancy bits in row-major order, then a 5-bit uppercase letter
     for each occupied square (A=0 … Z=25).
   - 1: codebook ID (2 bits: 1=CSW24 v1, 2=NWL23 v1), token count (8 bits).
     Each token starts with type (1 bit) and row-major square index (8 bits).
     Type 0 is a literal 5-bit letter. Type 1 is direction (0=across, 1=down)
     and a canonical Huffman word. Overlap must agree on the uppercase letter;
     every token must fit within the board. Maximal words in either direction
     are encoder candidates, with individual-letter fallback for phonies and
     uncovered letters.
5. Designated-blank count (2 bits, at most 2), then strictly increasing
   7-bit indices into the row-major list of occupied squares.
6. Rack tiles: 5 bits each (A=0 … Z=25, blank=26), in recorded order.

The board and racks must fit the physical English tile inventory. Blanks keep
their assigned letters; phonies are allowed. CGP whitespace and empty-run
spelling are normalized. Both `-lex CSW24` and CGP-spec `lex CSW24;` forms
are accepted (also `lang`). Standard `ld english`, `bdn standard15` or
`bdn CrosswordGame`, `bb 50`, and `var classic` options are validated and
omitted. Unsupported variants or unknown options return an error.

## XSG body version 2

Version (4 bits), metadata, rank-profile ID (2 bits: 0=none, 1=CSW24 v1,
2=NWL23 v1). Then, for each player: nickname string (varint byte length plus
bytes), one bit saying whether full name equals nickname, and a full-name
string if different. Names are retained, including internal spaces. Nicknames
must be distinct and cannot contain whitespace or `:`.

Event count is an unsigned varint; zero events is valid. Each event contains a player bit and a
3-bit type: 0 placement, 1 pass, 2 known-tile exchange, 3 count-only exchange,
4 withdrawn play, 5 other scored action. Types 6–7 are reserved.

For placements, one bit selects:

- Explicit: start square (8 bits), direction (1), span minus one (4), then
  6 bits per **new** tile: A–Z=0–25, designated a–z=26–51. Existing board
  letters are inferred. The rack follows with played tiles implied.
- Ranked: rack first (no implied played tiles), then order-2 move rank.

The encoder tries both and chooses the smaller; ties choose explicit. Rank
mode is optional per turn, so phonies and partial racks retain an explicit
fallback. Known exchanges store count (3 bits) and sorted 5-bit tile codes
(`?`=0, A=1 … Z=26); these tiles are implied in the rack. Count-only exchanges
store only a 3-bit count (1–7). Other scored actions store a UTF-8 string.
Passes and withdrawals have no action payload. All then store the rack.

Rack representation: reset bit, 3-bit extra-tile count, and (if nonempty) a
ranked/literal bit. The base is the multiset maximum of proved played/exchanged
tiles and that player's previous known leave. Reset discards that leave when
it conflicts with the current recorded rack. Extra tiles are added to this
base. Literal extras use sorted 5-bit tile codes. Ranked extras (up to three)
use an order-4 probability rank: enumerate sorted multisets from standard
English distribution minus board tiles and the known base, clamping remaining
counts at zero; weight each by the product of binomial coefficients; sort by
descending weight then ASCII tile string. These are inferred unknown-pool
probabilities, not knowledge of the opponent's actual hidden rack. No external
frequency table is needed. Count-only exchanges clear known carryover.

Finally each event stores signed score delta, a total-override bit, and signed
total if it differs from previous total plus delta. Intermediate addition uses
int64 and decoded totals must fit int32. Withdrawals restore the preceding
placement's board and require the same player. After all events, each player
has a final-rack-present bit and optional sorted rack string.

GCG notes, arbitrary headers, and rack ordering are omitted. Standard GCG
output omits the nonstandard `#language` header; English remains explicit
in the XSG metadata. Missing rack tiles
proved by a placement/exchange are retained. Passes, challenge withdrawals,
scored annotations, known/count-only exchanges, final racks, player names,
score deltas, and score totals survive normalization. Nonstandard game/board
headers are rejected; `CrosswordGame` is accepted as the standard board alias,
and explicit tile distributions must be `english`. This codec preserves
imperfect records rather than enforcing all game legality: turn order, score
correctness, and physical inventory in a GCG are not guaranteed. Input must
be UTF-8; convert legacy encodings first.

### Frozen ranking contract

Profiles 1 and 2 use the respective pinned KWG/KLV2, standard English tile
counts/scores, the standard 15×15 premium layout, and a 50-point bingo bonus.
Only tile placements are ranked; passes and exchanges have their own forms.
For the recorded rack and board, enumerate legal placements, normalize
play-throughs to `.`, and sort by:

1. Descending `move score + KLV leave value` in integer millipoints.
2. Ascending row-major start square, then direction (across before down).
3. ASCII spelling with `.` play-throughs and lowercase designated blanks.

If enumeration reaches the 100,000-candidate limit (possible with two blanks),
the turn is encoded explicitly; a truncated list never defines ranks.
Identical candidates are deduplicated; ranks 0–1023 are available. There is no
PAT, opening adjustment, endgame adjustment, win percentage, WMP, WIT, or RIT
term in this profile. Engine move generation is used to enumerate candidates,
but its ordinary output ordering is not used. Profile conformance fixtures
must remain unchanged. If a future rule or move-generation correction changes
this ordered set, retain the old profile implementation or allocate a new
profile/format version; do not silently change ranks under these IDs.

## Validation

```sh
make magpie_test
./bin/magpie_test xs
./bin/magpie_test xsdata:/tmp/magpie-xs-data/data
```

`xs` checks standalone formats, codebook construction through `conv`, signed
score boundaries, phony/blank games, partial racks, exchanges, withdrawals,
malformed codebooks, corruption, and truncation. `xsdata` exercises both
published books and rank profiles, plus cyclic mutation of ceil(10%) of board
letters to disrupt words while preserving physical inventory. Published
codebooks must reproduce the exact frozen canonical lengths from the original
training counts; the native implementation has been compared against both
million-game inputs.

## Reproduce the played-word frequencies in C

`tools/word_frequencies.c` contains the native trainer used for the original
million-game CSW24 and NWL23 corpora. `src/impl/word_counts.c` counts each newly
formed maximal word of length 2–15 in both directions, including crosswords.
Existing unchanged words are not counted again; designated blanks normalize
to uppercase. These are static-selfplay frequencies, not human frequencies.
Both players use static equity through game end, with WMP on and WIT/RIT off.
The tool requires a standard 15x15 / seven-tile build and KWG, KLV2 and WMP data.

```sh
make word_frequencies magpie BUILD=no_pgo_release
./bin/word_frequencies 0      250000 /tmp/train-0.tsv /tmp/train-0.cgp CSW24
./bin/word_frequencies 250000 250000 /tmp/train-1.tsv /tmp/train-1.cgp CSW24
./bin/word_frequencies 500000 250000 /tmp/train-2.tsv /tmp/train-2.cgp CSW24
./bin/word_frequencies 750000 250000 /tmp/train-3.tsv /tmp/train-3.cgp CSW24
./bin/word_frequencies merge /tmp/csw24-counts.tsv /tmp/train-0.tsv /tmp/train-1.tsv /tmp/train-2.tsv /tmp/train-3.tsv
./bin/magpie conv counts2xsw /tmp/csw24-counts.tsv /tmp/CSW24.generated.json.gz CSW24
```

Repeat with `NWL23` and distinct output paths for the NWL corpus. The optional
argument after the lexicon supplies the engine data search path; default is
`data`. Use frozen KWG/KLV2 bytes, matching WMP, and the original engine behavior
at `dab7e7f2` to reproduce the historical model. Changes to evaluation or move
generation may change future training counts; never replace a registered book.
Game index `i` uses seed `0x5eed + i * 0x9e3779b9` and starts player `i % 2`.
Sharding changes neither games nor counts. The C `merge` command sums counts
with overflow checks and writes alphabetically sorted `WORD<TAB>COUNT<LF>`.
It accepts plain TSV shards; no shell sort or Python aggregation is needed.
The extra CGP file samples every hundredth local game at turns 8, 16, 24 and
game end (once when checkpoints coincide); samples do not affect counts.

The original aggregate CSW24 TSV SHA256 is
`73bab4fa5ba8ffba6ce6b6d3381f1329e6f6e5368eb7a00b54539430171699d2`:
1,000,000 games, 48,537,742 occurrences, 172,959 distinct words.
The original NWL23 TSV SHA256 is
`6ec046adb58d73939fbae20be91eefad46c6acf21d5c13084367b4680b805218`:
1,000,000 games, 46,850,830 occurrences, 129,591 distinct words.
The exact aggregate TSV hashes are also embedded as `source_counts_sha256`
in the published codebooks. Training inputs are unnecessary at runtime.

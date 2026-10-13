# XSG game codec pilot

`wasmentry/qa/xsg_pilot.py` is an offline encoder, decoder, and Woogles corpus
benchmark. It chooses explicit placement or native MAGPIE static rank **for each
move independently**, including both forms in the same game. It is an experimental
format, not yet a browser URL loader or a stable interchange specification.

## Results

The pilot sampled 1,000 distinct classic games from the local Woogles corpus
(436,039 GCG/GCG.gz files). File-name SHA256 ordering selects candidates;
normalized-content hashes deduplicate and split whole games into 498 training
games and 502 held-out games. Ten candidates were rejected: six nonstandard game
types, three invalid played-letter records, and one incomplete game. No
Cross-Tables games were used. The sample includes short and unfinished games.

Training selects order-k Exp-Golomb parameters from orders 0 through 6: draw
order 4 and move order 2. Training draw ranks include both candidate forms of a
placement; the encoder still compares their actual final sizes on every turn.
Only training games tune these parameters. All four codec variants passed both
structured round-trip and normalized GCG reparse checks on every held-out game
(2,008 game round-trips).

Sizes are unpadded Base64url characters, including names and format overhead:

| Encoding | Mean | Median | 95th percentile |
| --- | ---: | ---: | ---: |
| Normalized GCG, zlib level 9 + Base64url | 693.72 | 723 | 903 |
| Explicit moves, played tiles implied | 360.33 | 366.5 | 514 |
| Plus rack carryover | 327.83 | 338 | 438 |
| Plus probability-ranked small draws | 321.61 | 331 | 431 |
| Plus mixed static-rank/explicit moves | **267.30** | **270** | **364** |

The mixed codec is 61.5% smaller than compressed normalized GCG on average.
It uses ranked form for 11,494 of 12,010 placements; **182 held-out games actually
use both placement forms**. Native generation matched 11,791 placements within
the first 1,024 static candidates, with no command errors. Of those matches,
3,268 are rank 1 and 7,767 are in the top 10. A match need not use ranked form:
the required rack and profile overhead can make explicit form shorter.

Games with a nickname containing `bot` average 251.36 characters (343 games);
the remaining 159 average 301.70. These are nickname-based groups, not verified
human/bot classifications. Aggregate measurements and profile identity are in
[`xsg-game-pilot-results.json`](xsg-game-pilot-results.json). Raw corpus games and
player-level results are not checked into the repository.

## What is retained

- Both player nicknames and full names, including Unicode.
- Game lexicon and language identity (English only in this pilot).
- Board placements, designated blanks, phonies, passes, known-tile exchanges,
  count-only exchanges, and withdrawn plays.
- Recorded partial racks as multisets, augmented only by tiles proved by the
  corresponding play or known exchange.
- Score deltas and cumulative totals, including corrections and final/time
  adjustments. Final rack headers are retained as text.

Notes, rack ordering, original whitespace, and other headers are discarded.
This preserves the normalized game, not the original GCG bytes. Unsupported
languages, lexica, variants, and malformed records fail explicitly in the pilot.

## Encoding

The envelope contains `XSG` magic, bit-packed body, and CRC32, encoded with
unpadded Base64url (`A-Z`, `a-z`, `0-9`, `_`, `-`). It needs no escaping as a URL
parameter value. CRC32 detects accidental corruption; it is not authentication.

The body starts with a 4-bit experimental version, 4-bit language ID, 5-bit
language-local game lexicon ID, two 3-bit rank-code orders, and a rank-profile
flag. The pilot uses the English registry IDs from the compact position work.
A ranked stream additionally records its independent rank lexicon ID and a
128-bit engine/data fingerprint. All English games currently use **CSW24** for
static ranking, including games whose rules lexicon is NWL23. The codebook
lexicon does not change the recorded rules lexicon.

Names are length-prefixed UTF-8, with a bit to avoid repeating a full name equal
to the nickname. An unsigned LEB128 event count precedes the events. Each event
starts with a player bit and 3-bit event type.

### Placements: choose either mode on every turn

Each placement has a mode bit:

1. **Explicit:** starting square (8 bits), orientation (1), span length (4), and
   new letters (6 each, including blank designations). Previously occupied
   squares require no letter payload. The rack follows; played tiles are already
   known and do not need to be repeated there.
2. **Static rank:** the rack comes first, followed by an Exp-Golomb rank. The
   decoder regenerates the same static candidate list from the board and rack.
   This order avoids a circular dependency between the rack and ranked move.

The encoder measures the complete move-plus-rack bit cost of both candidates
and picks the shorter. Phonies absent from CSW24, unmatched placements, and
expensive ranks use explicit form. Exchanges and passes currently use explicit
event forms only. At game level, the CLI also compares against an entirely
explicit stream to avoid profile overhead when it outweighs the rank savings.

### Rack carryover and draws

Known carryover and implied played tiles combine using multiset maximum, not
addition: the same tile can satisfy both observations. Only extra tiles are
encoded. A reset bit handles an inconsistent or partial annotation that omits
previously known tiles. Count-only exchanges discard carryover certainty because
the identities exchanged are unknown. The codec never invents a complete rack.

For up to three extra tiles, enumerate multisets and rank by descending weight
`product(comb(pool[letter], count[letter]))`, with lexical tie-breaking. The
common probability denominator cancels. The pool is the English distribution
minus public board tiles and known rack base: an **unseen-pool model**, not the
true hidden bag. Rank form is used only when shorter than literal 5-bit tile IDs.
Four-to-seven-tile additions and impossible pool combinations use literals.
Extending efficient probability ranking to larger draws remains future work.

### Scores and other events

Score deltas use canonical ZigZag/unsigned LEB128 with signed 32-bit bounds.
Cumulative totals normally follow from the preceding score plus the delta;
a flag and explicit signed value preserve discrepancies. Other GCG action text
is length-prefixed, keeping time penalties and challenge adjustments without
needing an exhaustive action taxonomy.

## Rank compatibility and limitations

The native profile uses equity ordering, the first 1,024 generated candidates,
zero scores, an unknown opponent rack, and no WMP/WIT/RIT. A fingerprint binds
the native library binary, profile marker, CSW24 KWG, and KLV2. The decoder
refuses a different profile instead of silently decoding a different move.

This is deliberately stricter than a production format should be: rebuilding
the library can change its fingerprint. Before browser integration, define an
immutable, portable ranking contract, including deterministic tie order and
all board/distribution/evaluation inputs, and retain compatible decoders.
The current experiment assumes the standard English board and distribution.
It does not establish cross-platform equivalence of native ranks.

The Python reader checks corruption, format IDs, lengths, ranks, coordinates,
tile values, canonical varints/Base64, and padding. It is a research decoder,
not yet a hardened public URL parsing endpoint. Synthetic tests cover partial
racks, carryover overlap, blanks, phonies, withdrawals, count exchanges, score
corrections, malformed packets, and a single game mixing a native-ranked play
with an explicit phony.

## Reproduce

From the repository root, with MAGPIE data installed:

```sh
make libmagpie
XSG_NATIVE_LIBRARY=bin/libmagpie.dylib python3 -m unittest discover \
  -s wasmentry/qa -p test_xsg_pilot.py -v

python3 wasmentry/qa/xsg_pilot.py benchmark \
  --corpus woogles=/path/to/cgpbot/testgen/gcg \
  --games 1000 --library bin/libmagpie.dylib --data data \
  --output /tmp/xsg-woogles-results.json

python3 wasmentry/qa/xsg_pilot.py encode game.gcg \
  --library bin/libmagpie.dylib --data data
python3 wasmentry/qa/xsg_pilot.py decode TOKEN \
  --library bin/libmagpie.dylib --data data
```

Use the platform's shared-library filename on Linux. Omit `--library` from
`encode` for a standalone explicit/carryover/draw token that needs no native
engine to decode. The benchmark JSON includes source-file hashes and the split
manifest for local reproduction; only aggregate results belong in the repo.

# Compact position links

Use **Game → Copy position link** (or the desktop file controls). Opening the
link loads that position for analysis without the new-game dialog. The link
keeps the current release path and uses `?p=TOKEN`. No server storage, dictionary,
or optional word maps are required. If clipboard access is denied, a dialog
lets the user copy the link manually.

This is CGP state: board, designated blanks, both racks including rack order,
scores relative to the player on turn, consecutive scoreless turns, and lexicon.
It does not include names, clocks, history, tile ownership, or analysis results.
Like Copy CGP, sharing is disabled during a live human game to protect hidden
racks. Malformed links open the CGP import dialog with a recoverable message;
the user can paste a valid position or close the dialog and start a game.

## Version 1 wire format

Unsigned fields are written most-significant bit first, in this order:

| Field | Bits | Meaning |
|---|---:|---|
| Version | 4 | 1 |
| Lexicon | 1 | 0 = CSW24, 1 = NWL23; fixed IDs |
| Scores | 2 × 15 | Each score plus 10,000; range −10,000…10,000 |
| Scoreless turns | 3 | 0…6 |
| Rack lengths | 2 × 3 | 0…7 each |
| Occupancy | 225 | Row-major 15 × 15, 1 for occupied |
| Board letters | 5 each | Occupied squares in row-major order; A=0…Z=25 |
| Board blank count | 2 | 0…2 |
| Blank locations | 7 each | Ascending indices into the occupied-square list |
| Rack letters | 5 each | First rack then second; A=0…Z=25, ?=26 |
| Padding | 0…7 | Zeros to next byte |
| Checksum | 16 | CRC-16/CCITT-FALSE over payload bytes, big-endian |

CRC polynomial 0x1021, initial value 0xffff, no reflection or final XOR.
Encode bytes using unpadded base64url. This is a corruption check, not a signature.
At most 100 physical English tiles yields at most 785 payload bits, 99 payload
bytes, plus two checksum bytes: **135 URL characters maximum**. The initial
132-character estimate excluded this checksum.

Unknown versions, duplicate `p` parameters, bad base64/checksums, truncation,
invalid letter IDs, out-of-range values, impossible tile inventories, duplicate
blank indices, trailing data, and noncanonical padding are rejected. Input
length is bounded before decoding. Word legality is deliberately not checked:
phonies and incomplete edited positions round-trip. New codecs must use a new
version; do not reorder the version-1 lexicon IDs.

Fixed empty CSW24 position (both racks empty, scores 0/0, zero scoreless turns):

```
EnEE4gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAANW3
```

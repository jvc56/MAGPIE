# Compact position links

Use **Game → Copy position link** (or the desktop file controls). Opening the
link loads that position for analysis without the new-game dialog. The link
keeps the current release path and uses `?p=TOKEN`. No server storage, dictionary,
or optional word maps are required. If clipboard access is denied, a dialog
lets the user copy the link manually.

This is CGP state: board, designated blanks, both racks including rack order,
scores relative to the player on turn, consecutive scoreless turns, language, and lexicon.
It does not include names, clocks, history, tile ownership, or analysis results.
Like Copy CGP, sharing is disabled during a live human game to protect hidden
racks. Malformed links open the CGP import dialog with a recoverable message;
the user can paste a valid position or close the dialog and start a game.

## Version 3 wire format

Unsigned fields are written most-significant bit first, in this order:

| Field | Bits | Meaning |
|---|---:|---|
| Version | 4 | 3 |
| Language / lexicon | 9 or more | Shared header below; language first, then language-local lexicon ID |
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
At most 100 physical English tiles and a registered language/lexicon pair yield
at most 793 payload bits, 100 payload bytes, plus two checksum bytes:
**136 URL characters maximum**. Escaped metadata is bounded by 563 characters
including both maximum-length literal fields. No extra percent-escaping is
needed: base64url uses only letters, digits, `-`, and `_`, without `=` padding.

Unknown versions, duplicate `p` parameters, bad base64/checksums, truncation,
invalid letter IDs, out-of-range values, impossible tile inventories, duplicate
blank indices, trailing data, and noncanonical padding are rejected. Input
length is bounded before decoding. Word legality is deliberately not checked:
phonies and incomplete edited positions round-trip. New codecs must use a new
version; do not reinterpret existing IDs.

Version 1 remains readable: its one-bit lexicon field means CSW24 (0) or NWL23
(1), and its language is implicitly English. Version 2 was used only by the
offline word-codec example; it is not accepted by this browser decoder. The
new language header uses version 3 rather than silently changing either format.

Fixed empty CSW24 position (both racks empty, scores 0/0, zero scoreless turns):

```
MQpxBOIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABBlg
```


## Shared XSP / XSG language and lexicon header

`wasmentry/lexicon-name.mjs` supplies the reusable header. Write it once per
position (XSP) or game (XSG), after the enclosing format version:

1. **Language ID: 4 bits.** Values 1–15 are registered language tags. Zero
   escapes to an 8-bit byte count and literal UTF-8 tag (1–63 bytes).
2. **Lexicon ID: 5 bits, interpreted within that language.** Values 1–31 are
   registered names. Zero escapes to an 8-bit byte count and literal UTF-8
   name (1–255 bytes).

The tag has bounded BCP-47-style syntax: 1–8 ASCII letters followed by optional
hyphen-separated 1–8 ASCII alphanumeric subtags. No online registry lookup is
needed. Exact spelling is preserved; a variant such as `en-US` is currently a
literal tag. Lexicon literals preserve arbitrary Unicode, spaces, punctuation,
and case. Malformed UTF-8, empty/oversized literals, truncation, and unassigned
numeric IDs are rejected. Unknown names are data, never engine command text.

| Language ID | Tag | Lexicon IDs, starting at 1 in the order shown |
|---|---|---|
| 1 | en | CSW24, NWL23, CSW07, CSW12, CSW15, CSW21, NWL20, OSW1, OSW2, OSW3, OSW4, OSWI, TWL06, TWL14, TWL98 |
| 2 | ca | DISC2 |
| 3 | nl | DSW25 |
| 4 | fr | FRA20 |
| 5 | de | RD29 |
| 6 | pl | OSPS49 |

For example, `en + CSW24` is `0001 00001`, `en + NWL23` is `0001 00010`,
and `fr + FRA20` is `0100 00001`: **9 bits** each. All 20 production lexicons
in MAGPIE-DATA at commit `ee46dfdb7d878584ab48e7158edc516aba3bdfc1`
are included. Test fixtures and `_super21.klv2` leave tables are not lexicons.
Language assignments match MAGPIE's letter distributions.

Both tables are append-only. Never reorder, rename, or reuse an ID. When a
fixed-width table fills, further entries must remain literals or use a new
format version. Decoders accept literal names/tags even if they have gained
registered IDs since the link was created. An unregistered name is scoped to
its explicit language; names are not globally reserved across languages.

This header identifies the position/game language and rules lexicon. Each
compression codebook belongs to a lexicon. The encoder should normally use a
codebook from the largest available lexicon in that language: currently CSW24
for English, including NWL23 games. The game lexicon stays NWL23 in that case.

Word-aware XSP and move-aware XSG must also record the **codebook lexicon**
(using the same language-local IDs) and its immutable revision/model ID. Do
not have the decoder dynamically choose whichever lexicon is largest today:
new dictionaries and retrained frequencies must never change old links. A
future format may omit duplicate fields with a defined “same as game lexicon”
flag, but the codebook identity must still be unambiguous. The dictionary-free
version-3 position codec has no codebook field because it uses none.

The header codec supports all listed languages; the current **position body
codec remains English-only** (A–Z, two blanks, English inventory, 15×15).
It explicitly rejects other languages rather than validating them with the
wrong tile distribution. Existing CGP callers without a language use the known
lexicon's language, defaulting to English for an unknown name. The browser can
currently load only CSW24 and NWL23; other English names decode but show a
recoverable import error. Supporting another language's board requires a new
body codec with that language's alphabet and tile distribution.

XSG is not yet implemented. Its planned envelope uses this same shared header;
there is no XSG encoder/decoder or multilingual board support implied by these
metadata changes. The offline word-codec study and its old example sizes have
not been retroactively changed to this header.

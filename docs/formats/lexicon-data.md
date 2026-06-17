# Lexicon & Board Data

> **Status:** outline — prose to be written.
> **Sources:** `src/ent/letter_distribution.h`, `src/ent/board_layout.h`, `src/impl/convert.c`
> **Related:** [Data Setup](../getting-started/data-setup.md)

## Letter distribution CSV

<!-- NOTE: document the row format:
       upper,lower,frequency,score,is_vowel[,fullwidth_upper,fullwidth_lower]
     The optional full-width display columns. One row per tile. -->

## Board layout files

<!-- NOTE: first line = start square `row,col`; then BOARD_DIM rows of bonus
     characters. Document the bonus glyphs table:
       ' ' none, ' (DL), - (DW), " (TL), = (TW), ^ (QL), ~ (QW), # (brick). -->

| Glyph | Meaning |
| ----- | ------- |
| (space) | No bonus |
| `'` | Double letter |
| `-` | Double word |
| `"` | Triple letter |
| `=` | Triple word |
| `^` | Quadruple letter |
| `~` | Quadruple word |
| `#` | Brick (unplayable) |

## Strategy tables

<!-- NOTE: the win-percentage lookup tables in data/strategy used by simulation. -->

## Conversion pipeline

<!-- NOTE: text → kwg → wmp (and klv). `convert text2kwg`, `convert
     text2wordmap`; the convert_lexica.sh helper. One-word-per-line uppercase
     input. -->

# CGP — Crossword Game Position

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/cgp.c`
> **Related:** [Annotating Games](../user-guide/annotating-games.md), [Pre-Endgame](../user-guide/pre-endgame.md)

## Purpose

<!-- NOTE: a single-line string encoding a full position — board, both racks,
     scores, and metadata. The FEN of crossword. Used by `cgp` and `peg`. -->

## Grammar

<!-- NOTE: break down the fields of the string (board rows with run-length empty
     squares, racks, scores, turn/bag count, trailing settings like `-lex`).
     Annotate the README PEG example position field by field. -->

```text
15/3Q7U3/3U2TAURINE2/... ENOSTXY/ACEISUY 356/378 0 -lex NWL20
```

## Compatibility

<!-- NOTE: relationship to Macondo's CGP spec; link out in References. -->

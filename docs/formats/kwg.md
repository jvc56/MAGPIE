# KWG — Kurnia Word Graph

> **Status:** outline — prose to be written.
> **Sources:** `src/ent/kwg.h`, `src/def/kwg_defs.h`, `src/impl/kwg_maker.c`
> **References:** [wolges](https://github.com/andy-k/wolges) (canonical KWG spec).
> **Algorithm:** [Word Graphs](../algorithms/word-graphs.md).

## Purpose

<!-- NOTE: compact binary word graph holding both DAWG and GADDAG; the on-disk
     form MAGPIE loads as `.kwg`. -->

## On-disk layout

<!-- NOTE: header (if any) + node array. Document a node as a 32-bit word and its
     bit fields (tile / arc index / is_end / accepts). A small byte/bit table or
     diagram here. Defer to wolges for the authoritative spec; document what
     MAGPIE relies on. -->

## Endianness & versioning

<!-- NOTE: byte order, any version/magic, compatibility with wolges-produced
     files. -->

## How MAGPIE reads it

<!-- NOTE: mmap vs. read into memory; entry point in kwg.h. -->

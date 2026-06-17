# Word Graphs: KWG, DAWG & GADDAG

> **Status:** outline — prose to be written.
> **Sources:** `src/ent/kwg.h`, `src/impl/kwg_maker.c`, `src/def/kwg_defs.h`
> **References:** [wolges](https://github.com/andy-k/wolges). On-disk layout: [KWG format](../formats/kwg.md).

## DAWG and GADDAG, briefly

<!-- NOTE: DAWG = minimized trie of words (one direction). GADDAG = a trie that
     stores reversed-prefix + suffix so a word can be built outward from any
     letter — essential for crossword move generation. Cite the GADDAG origin
     (Steven Gordon) in References. -->

## The Kurnia Word Graph (KWG)

<!-- NOTE: the KWG packs both DAWG and GADDAG into one node array. Describe a
     node's bit fields (tile, arc index, end/accept flags) and how arcs are
     followed. -->

## Construction

<!-- NOTE: kwg_maker — building and minimizing the graph from a word list.
     Mention determinism and dedup of equivalent subtries. -->

## Traversal in practice

<!-- NOTE: how move generation and word pruning walk the graph; accept vs.
     is_end semantics. -->

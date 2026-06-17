# Move Generation & Shadow Play

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/move_gen.c`, `src/impl/wmp_move_gen.h`
> **References:** [wolges](https://github.com/andy-k/wolges) (shadow play);
> "The Anagram Method" \[GonzalezAlquezar18\] (WMP). See [References](../about/references.md).

## The problem

<!-- NOTE: enumerate every legal play (and rank it) from a rack + board. Why
     naive enumeration is too slow; the role of anchors and cross-sets. -->

## Anchors and cross-sets

<!-- NOTE: define anchor squares and the cross-set (bitmask of letters that can
     legally start the perpendicular word at a square). Link to the board
     entity. -->

## Shadow play

<!-- NOTE: the key idea adopted from wolges — precompute the maximum achievable
     score at each anchor before committing to full word enumeration, so the
     generator can prune by an upper bound. Walk through the traversal. -->

## Walking the word graph

<!-- NOTE: how the KWG (DAWG for one direction, GADDAG for arbitrary placement)
     is traversed during generation. Link to Word Graphs. -->

## The WMP fast path

<!-- NOTE: wmp_move_gen — the anagram-method word map accelerates subrack→word
     lookups, with per-thread caching of subrack enumerations to avoid redundant
     KLV descents and hash lookups during simulation rollouts. Cite the Anagram
     Method paper. -->

## Performance notes

<!-- NOTE: hot paths, allocation strategy, what was measured. -->

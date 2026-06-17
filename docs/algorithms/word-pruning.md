# Word Pruning

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/word_prune.c`

## Why prune

<!-- NOTE: in the endgame, most of the lexicon is irrelevant — only words
     playable given the remaining tiles and board constraints matter. A smaller
     word set means a smaller, faster search. -->

## Building the position-pruned set

<!-- NOTE: traverse rows and columns to find feasible playthrough blocks and
     letter constraints, collect the possible words, and build a pruned KWG
     containing only them. -->

## Use in the endgame

<!-- NOTE: how the endgame/PEG solver consumes the pruned graph; correctness
     argument (no playable word is dropped). -->

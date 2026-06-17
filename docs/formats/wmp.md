# WMP — Word Map

> **Status:** outline — prose to be written.
> **Sources:** `src/ent/wmp.h`, `src/impl/wmp_maker.c`
> **References:** "The Anagram Method" \[GonzalezAlquezar18\] — the key ideas
> behind the WMP. See [References](../about/references.md).

## Purpose

<!-- NOTE: a hash-based map from a (sorted) set of tiles to the words that can be
     formed — fast anagram/subrack lookup that accelerates move generation. The
     `.wmp` file. -->

## The anagram method

<!-- NOTE: summarize the GonzalezAlquezar18 idea the format implements (indexing
     words by their letter multiset) and why it's fast. Cite the paper. -->

## On-disk / in-memory layout

<!-- NOTE: words bucketed by length and blank count; packed/inline storage for
     common cases; blank-letter bitvectors for feasibility checks. Document the
     packed entry structure from wmp.h. -->

## How MAGPIE reads and uses it

<!-- NOTE: the wmp_move_gen fast path and per-thread caching. Link to the move-
     generation algorithm page. -->

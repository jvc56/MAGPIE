# Endgame Solver

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/endgame.c`, `src/impl/endgame_time.c`
> **User guide:** [Endgame](../user-guide/endgame.md)

## Perfect-information search

<!-- NOTE: once the bag is empty both racks are knowable, so the endgame is a
     finite two-player zero-sum game solvable exactly. The value is the final
     spread (or win/loss/draw). -->

## Alpha-beta with a transposition table

<!-- NOTE: minimax/alpha-beta over move sequences; the TT caches position values
     to avoid re-solving transposed states. Note the TT memory guidance (≤50% of
     RAM; split when comparing two solvers). -->

## Iterative deepening & time management

<!-- NOTE: endgame_time — deepening with a turn-limit/EBF-based budget; returns
     the best line found when a soft/hard limit hits. Per-ply PV callbacks. -->

## Heuristics & modes

<!-- NOTE: stuck-tile cutoffs, greedy leaf playout for non-empty leaves,
     first-win mode (win/loss/draw, faster pruning), dynamic worker injection. -->

## Validation

<!-- NOTE: how endgame correctness/quality is checked under time budgets; link
     to dev-guide testing and the endgame benchmarking notes. -->

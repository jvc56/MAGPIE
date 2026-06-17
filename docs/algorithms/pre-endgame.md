# Pre-Endgame Solver

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/peg.c`, `src/impl/peg_pool.c`, `src/impl/peg_combinatorics.h`
> **User guide:** [Pre-Endgame (PEG)](../user-guide/pre-endgame.md)

## The pre-endgame problem

<!-- NOTE: 1–4 tiles in the bag — not yet perfect information. For each candidate
     move, enumerate the possible bag/opponent-rack orderings (binomial
     combinatorics), evaluate each, and aggregate into win% and spread. -->

## Scenario enumeration

<!-- NOTE: peg_combinatorics — how orderings are generated and counted; the
     `-pegstride` sampling for bag ≥ 3 with reweighting. -->

## Leaf evaluation

<!-- NOTE: exact `endgame_solve` for bag-emptying scenarios; greedy playout
     (averaged over leftover-bag orderings) otherwise. So the result is a strong
     estimate, not a literal proof. -->

## The halving cascade

<!-- NOTE: stage 0 greedily scores all moves; each subsequent stage keeps the
     top-K (`-pegtopk`, default 32,16,8,4,2) and re-evaluates at deeper fidelity.
     `all`/`0` = exhaustive. `-pnoprune` shields moves from the cut. -->

## Opponent models

<!-- NOTE: rational (best reply) vs. pessimistic (`-pegpess`, worst-for-mover →
     guaranteed-win). -->

## Worker pooling

<!-- NOTE: peg_pool — thread-local endgame contexts reused across scenarios. -->

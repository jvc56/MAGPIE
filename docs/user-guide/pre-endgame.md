# Pre-Endgame (PEG)

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/peg.c`
> **Deep dive:** [Pre-Endgame Solver](../algorithms/pre-endgame.md)

<!-- NOTE: the README has an unusually complete PEG section — adapt it here and
     keep it current. PEG = 1–4 tiles in the bag. For each candidate move it
     enumerates bag/opponent-rack orderings, solves the resulting endgames, and
     reports win% and spread. -->

## Solving a pre-endgame

<!-- NOTE: load with `cgp`, run `peg`. Show the README example position. -->

## Opponent models

<!-- NOTE: rational (best-equity reply, default) vs. pessimistic
     (`-pegpess true`, worst-for-you reply → guaranteed-win analysis). -->

## Restricting and protecting moves

<!-- NOTE: `-pegonly <moves>` (comma-separated UCGI, no spaces; coord.tiles;
     `pass`; exchanges invalid). `-pnoprune <moves>` shields moves from the
     halving cut. Use `-` to clear. -->

## Speed vs. accuracy

<!-- NOTE: the four knobs — `-pegstride` (scenario sampling), `-pegtopk`
     (halving schedule; `all`/`0` = exhaustive), `-tlim` (time), `-threads`.
     Reproduce the fast / default / exhaustive examples and the caveat that the
     leaf eval is exact for bag-emptying scenarios but greedy otherwise. -->

```text
magpie> peg -pegstride 7 -tlim 5    # fast: sampled, 5s cap
magpie> peg                         # default: full enumeration, halving cascade
magpie> peg -pegtopk all            # exhaustive: every play, full-depth, no caps
```

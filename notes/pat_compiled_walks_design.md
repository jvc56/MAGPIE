# Design: compiled PAT walks with cross-turn reuse

Status: design only, not implemented.

## Where the time goes (measured, CSW21, X self-play, 10 threads)

- **Share of runtime.** No-PAT static runs at 13,459 games/s, X at 4,337. PAT is about 2/3 of
  X's runtime.
- **Profile of PAT's time.** From an optimized build with symbols:
  - Context load, which walks every premium unit every turn: about 1/2.
  - Per-candidate rescans: about 1/2.
- **Per-turn counts.**
  - About 120 units per load, since X weights TWS, DWS, TLS and DLS in both directions.
  - About 5.7 exact candidate evaluations per turn, each rescanning about 30 units.
- **What didn't work** (branch `claude/pat-perf`, commit 7ba19181, games identical):
  - a per-position cross-info cache (89% hit rate);
  - one-sided unit rescans (70% of rescans);
  - byte-table subset sums for flexibility and score exposure.

  Together they were about 3% *slower* at 10 threads. They add about 160 KB to each thread's
  context, and the per-square arithmetic they removed was not the cost.

Conclusion: the cost is the structural walk itself, not the arithmetic on each square. The walk
covers stepping lanes, reading letters, cross sets and bonuses, following floater runs, doing
through-table lookups, and function-call overhead. A speedup has to walk less.

## Idea

Split a unit's walk into two stages.

1. **Compile (board-only).** Walk the unit once and emit a compact list of contribution
   records. Each record is one of:
   - **hook:** distance bin, letter set, cross score, letter multiplier, word multiplier, premium
     word multiplier;
   - **floater run:** bin, run tile-score sum, extension letter set, through-table counts and
     scores (board-only);
   - **triple-triple or double-double span:** its flags.

   Also emit the walk's extent and the premium's own hook record.
2. **Evaluate (per position).** Turn the records into the unit's feature row, or directly into
   its penalty, using the position's unseen counts and hyper scale. This stage needs only subset
   sums over letter sets and a few multiplications per record.

Records depend only on the board, not on the mover's rack or the unseen pool. A unit's records
stay valid until a move places a tile inside the unit's halo × extent region, the same test
`pat_move_affected_units` already applies.

## Reuse across turns

- **Where the cache lives.** In the Game (next to the Board), not in MoveGen. Autoplay workers
  alternate between the two games of a mirrored pair, so a per-thread cache would thrash.
- **What it holds.** For each unit: its records, its extent, and a valid bit.
- **Invalidation.** `play_move`, or the PAT load given the moves since the last load, clears the
  valid bit of every unit whose region the move touched. It also clears units whose premium the
  move covered; those drop out of `pat_find_tws`.
  - Cross sets change only next to placed tiles, inside the halo, so nothing outside the region
    changes.
  - Undo (endgame and sim paths) invalidates the whole game cache, or the cache is simply not
    attached there.
- **What the load does.**
  - Recompile only invalid units, expected about 25% of them.
  - Evaluate every unit from its records.
  - Build the masks and bounds as today.
- **Per-candidate rescans** stay as they are: the move's overlay changes the records only in the
  touched region. Later they could recompile only the touched side from records.

## Expected benefit

- **Estimate.** The load is about 1/3 of X's runtime. Recompiling about 25% of units, plus
  evaluating records for all of them (assume about 1/4 of a full walk's cost), gives
  `0.25 + 0.75 × 0.25 ≈ 0.44` of today's load. That saves about 19% of X's runtime,
  **roughly +20–25% games/s**. Treat this as an upper-end estimate until prototyped: the
  evaluate-from-records cost is the key unknown.
- **Footprint.** Records per unit are small, about 20–40 bytes each and a few per unit.
  120 units × about 100 bytes is about 12 KB per game, much smaller than the failed per-thread
  cache.

## Correctness plan

- **Bit-exact** with the current load: identical unit feature rows. Assert it in a test that
  compiles and evaluates alongside the existing walk on seeded positions, including after
  sequences of moves, to cover invalidation.
- **Game identity:** same turn counts and scores on a 100K-pair seed, as used for every PAT change.
- **Flag off:** no cost when the player has no PAT. The cache is only allocated and touched when
  PAT is active.

## First step (cheap, decides go / no-go)

Prototype compile and evaluate within a single position, without cross-turn reuse, and time
`evaluate(records)` against a full walk for all units. If evaluation is not at least about 3×
cheaper than walking, cross-turn reuse cannot pay off, and the design stops there.

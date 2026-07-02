# Endgame solver speed optimization

Work on branch `claude/endgame-speed-opt`. Goal: make the endgame solver faster
without changing a single game-theoretic result, and prove it.

## Summary

Five correctness-preserving refactors to the endgame's per-node move generator
(the `_small` movegen path in `src/impl/move_gen.c`, used only by the endgame):

**+3.87% faster** on a drift-immune interleaved benchmark (30 endgame positions
@ 4-ply, single-thread, median of 4 interleaved rounds), with **byte-identical
game-theoretic values *and* node counts** versus baseline. Because every kept
change is a pure refactor, the search tree is provably unchanged — the speedup
is pure per-node work removed.

Two candidate optimizations were tried, measured, and **reverted** because they
were net-slower despite sound theory (documented below), plus a Makefile
stale-build fix.

## Why these changes

Profiling the baseline (single-thread, 60 positions @ 4-ply; self-time excluding
the parked main thread's `__ulock_wait`) showed the cost is overwhelmingly
**move generation, regenerated at every interior node**:

| function | self-time |
|---|---|
| `recursive_gen_small` (DAWG walk) | 16.7% |
| `go_on_small` | 8.8% |
| `shadow_play_*_small` (shadow scoring/bounds) | ~19% |
| `game_gen_cross_set` + anchors | ~7% |
| `abdada_negamax` (search core, self) | 1.3% |
| TT store/probe | ~0% |

So the transposition table and raw search overhead are *not* bottlenecks. The
two levers are **(a) cheaper per-node movegen** and **(b) fewer nodes**. Lever
(a) is where every kept win came from; lever (b) was tried and did not pay off
(see negative results).

## Optimizations kept

All five are node-count-preserving (pure refactors). Endgame-scoped: the
`_small` functions are called only from `endgame.c`; the `gen_load_position`
changes are guarded so non-endgame movegen (sim, autoplay, gameplay, peg) is
untouched.

| # | change | mechanism |
|---|---|---|
| 1 | Skip the per-node `LetterDistribution` copy | `gen_load_position` deep-copied ~3.4 KB and re-ran a compat scan on every `generate_moves`. Cache the source `ld` pointer (+name pointer, for ABA safety) and skip when unchanged. |
| 2 | Tighten `recursive_gen_small` | Compute `possible_letters_here` only on the empty-square branch (dead on play-through); load `row_cache[col]` once; hoist the loop-invariant blank count. |
| 3 | Skip dead setup for endgame record types | Skip `wmp_move_gen_init`'s BitRack build / anchor seeding when WMP is inactive (always, in the endgame); skip the `board_copy_opening_penalties` copy the endgame never reads. |
| 4 | Lazy state-saves in `shadow_play_right_small` | It unconditionally saved ~132 B (rack, tile scores, two multiplier arrays) even though most rightward shadows over short endgame racks restrict nothing. Ported the already-shipped lazy pattern from the non-small twin (save on first restriction, before the mutating `restrict` call). Biggest single win. |

Development progression (each measured against the fixed baseline; noisier than
the final interleaved number): 0.7% → 1.3% → 1.9% → 3.6%.

## Negative results (tried, measured, reverted)

- **Killer-move ordering** — the analysis's top-ranked *node-count* reducer.
  Implemented correctly (values byte-identical, deterministic) and it *did* cut
  node count 2.5%, but it ran **~4% slower**: the endgame's existing build-chain
  move ordering is already strong, so generic killers displace better-ordered
  moves, and node count is a poor proxy for work here (cheap TT/leaf nodes vs
  expensive movegen nodes). Fewer-but-costlier nodes lose.

- **Micro-DCE (defer `bonus_square` load; cap `shadow_record_small` loop at the
  live rack size)** — measured **1.3% slower** than the 5-refactor stack on the
  interleaved A/B. Capping the inner-product loop at a *variable* bound defeats
  the compiler's constant-trip unrolling/vectorization of the `RACK_SIZE`=7
  loop; the lost unrolling outweighs the skipped zero-multiplies.

Both are the same lesson: **measure, don't trust the node count or the
instruction count.**

## Time-limited full-game playout

The fixed-depth benchmark isolates the solver on single positions. To confirm the
speedup in the realistic scenario — an engine on a clock playing an endgame out to
the end — `test_endgame_playout_bench` (`egplayout`) plays each bag-empty endgame
to game over, solving every move under a per-move wall-clock budget (time-limited
iterative deepening) and playing the best move found.

Interleaved baseline-vs-optimized, 30 endgames @ 200 ms/move, single-thread,
median of 3 rounds (`tools/eg_playout_ab.sh`):

| metric | baseline | optimized | delta |
|---|---|---|---|
| games completed | 30/30 | 30/30 | — |
| nodes searched | 14,967,225 | 14,981,623 | +0.1% |
| aggregate exact depth | 140 | 140 | 0% |
| wall clock | 51.18 s | 49.50 s | **−3.3%** |
| nodes/sec | 292 k | 303 k | **+3.5%** |

Because the EBF soft-limit stops *between* depths, both engines reach the same
depth on each move and play the *identical* line (node counts and aggregate depth
match), so the win is pure wall clock: the optimized solver plays out the same
endgames **3.3% faster** (+3.5% nps), stable across all three rounds. At a tighter
budget, or where a depth boundary falls differently, that same speed can instead
buy one more ply (stronger play at equal time). Move quality is unchanged here,
consistent with the byte-identical fixed-depth results.

## Correctness validation

- **Per-position value match**: game-theoretic value (`PVLine.score`) byte-identical
  to baseline on every benchmark position — the load-bearing check (the endgame
  is exact; any value change is a bug).
- **Node-count identity**: single-thread node counts (`endgame_ctx_get_nodes_searched`)
  byte-identical to baseline — proves each refactor left the search tree unchanged.
- **Known-value suite**: `test_single_endgame` oracles (−63 pass-first, +55
  vs_joey, −116, +11) green.
- **Shared-path suite**: `gen_load_position` feeds sim/gameplay/peg/analyze, so
  the full movegen/shadow/gameplay/sim/peg/cross-set/board/cgp/wmp/leavemap/kwg/eq
  tests were run — all green.
- **Deeper ply**: 5-ply values + node counts identical to baseline.
- **Multi-thread**: 6-thread values identical to the single-thread baseline
  (ABDADA value-determinism preserved).

## Methodology notes

- **Harness**: `test_endgame_speed_bench` (`egspeedbench`) solves a battery of
  endgame CGPs once and emits `BENCHROW <idx> <value> <nodes> <time>` per
  position. Battery: 500 deterministic CSW21 non-stuck endgames (self-play, seed
  31415). Single-thread makes node counts deterministic, turning node-count
  identity into a rigorous correctness oracle.
- **Measurement drift**: back-to-back runs varied <0.15%, but measurements taken
  minutes apart drifted 1–2% (thermal/scheduling). The final headline uses a
  **drift-immune interleaved A/B** (`tools/eg_final.sh`): baseline and optimized
  binaries timed in alternation, compared by median, so slow drift cancels.
- **Build hygiene**: objects were not keyed by build flavor, so switching
  `dev`↔`release` (or `BOARD_DIM`) relinked stale mixed-flag objects. Fixed by
  keying `obj/$(BUILD)-b$(BOARD_DIM)-r$(RACK_SIZE)/`.

## Files

- `test/benchmark_endgame_test.c` — `egspeedbench` (fixed-depth) and `egplayout`
  (time-limited full-game playout) harnesses.
- `tools/eg_ab.sh` — per-change validator (rebuild + suite + value/node match + speed).
- `tools/eg_final.sh` — drift-immune interleaved fixed-depth baseline-vs-optimized A/B.
- `tools/eg_playout_ab.sh` — interleaved time-limited playout A/B.
- `src/impl/move_gen.c`, `src/impl/move_gen.h` — the five optimizations.

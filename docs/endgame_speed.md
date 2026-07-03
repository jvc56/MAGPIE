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

The fixed-depth benchmark isolates the solver on single positions. To see the
speedup under a clock — fixed thinking time per move — `test_endgame_playout_bench`
(`egplayout`) plays each bag-empty endgame out to game over: each move gets a
**hard** per-move wall-clock budget (unlimited depth; iterative deepening runs
until the deadline fires *mid-search* via `external_deadline_ns`, checked every
1024 nodes), then the best move from the last completed depth is played.

**Identical opening positions (divergence-free), hard 1 s/move, 30 positions,
median of 3 interleaved rounds:**

| metric | baseline | optimized | delta |
|---|---|---|---|
| nodes searched | 10,596,643 | 10,863,907 | **+2.5%** |
| completed depth (sum over 30) | 70 | 70 | **0** |
| wall clock | 23.78 s | 23.72 s | ~0% |

Under a hard cutoff both engines burn the full budget, so the speedup shows up as
~2.5% more nodes searched in the same time — but it reaches the **same completed
depth and plays the same move**. The reason is quantization: completing one more
IDS depth costs ~5× more time (endgame effective branching factor ≈ 5), far more
than a few-percent edge buys, so a small speedup almost never crosses a
depth-completion boundary under a hard clock. (A full playout to game over shows a
larger ~6% aggregate node delta, but most of that is **line divergence** — once
either engine plays a different move the games diverge — not a genuine
same-position depth gain, which the identical-position table above isolates away.)

Contrast the **soft** (EBF, between-depth) limit: the solver only stops after a
completed depth, so both engines reach the same depth and play the identical line,
and the same speedup instead makes the solver finish ~3.3% faster (30 endgames @
200 ms/move soft: 51.2 s → 49.5 s, +3.5% nps). Either way the ~4% is real; whether
it buys wall-clock time (soft) or a sliver of extra search at equal time (hard)
depends on the stop rule, and a few-percent gain is too small to change which move
a hard-time-limited search plays.

## Playing strength under a time control (head-to-head match)

Does the ~4% buy any *points*? An 18-thread, 5 s/player/game, colour-balanced
head-to-head match between the baseline and optimized solvers (`tools/eg_match.py`,
cross-process via the `egmove1` transducer, per-turn soft allocation from a
per-game hard bank), run for 8 hours:

| metric | value |
|---|---|
| games / colour-paired positions | 10,989 / 5,494 (133.5 B nodes) |
| **OPT net strength** | **−0.009 ± 0.153 pts/game** (statistically 0) |
| positions where play diverged | 80 / 5,494 (**1.46%**) |
| among divergent: opt better / worse | **40 / 40** (net −49.5) |

Net strength is indistinguishable from zero. Play diverged on only 1.46% of
positions, and there **symmetrically** (40 better, 40 worse) — that divergence is
multi-threaded ABDADA scheduling noise, which helps both engines equally, not the
speed edge. Colour-balancing is what makes this readable: it cancels each
position's (large) inherent value, so a nonzero paired advantage would mean the
engines actually played different moves; almost always they don't.

The mechanism, now confirmed at scale: the move played is the last *completed*
depth, an extra ply costs ≥1.46× (median 4–16×), and a per-game bank over a short
endgame adds at most ~10–40% — so a ~4% edge can never fund an extra ply, even
banked. (Even at 5 s × 18 threads these bag-empty 7×7 endgames only reached depth
1–3 on 91% of moves — they branch hard — yet both engines reached the same
completed depth and hence the same move.)

**Conclusion:** the ~4% is real *speed* → **throughput** (more analysis / sims /
self-play per unit time, plus multi-threaded scaling) but **≈0 playing-strength
value** under any per-turn or banked time control at this scale. Converting speed
to strength needs a *multiplicative* speedup or a move-quality change, not
micro-refactors.

### Stress test: hard (stuck-tile, budget-binding) subset

The all-positions run finds 0 partly because most endgames fully solve (identical
play). To isolate the regime where the budget actually binds, the match was
re-run on **487 stuck-tile endgames filtered to those NOT fully solved to depth 25
in 2 s @ 18 threads**, at 5 s/player/game for 2 hours (1,122 games / 561 pairs):

| metric | value |
|---|---|
| OPT net strength | **+0.20 ± 0.76 pts/game** (still 0) |
| positions where play diverged | **136 / 561 (24.2%)** — 17× the easy battery |
| among divergent: opt better / worse | **69 / 67** (binomial z = 0.17, n.s.) |

Now play *does* diverge (24%, because the budget binds), but symmetrically — opt
wins 69, loses 67, a coin flip — i.e. multi-threaded ABDADA scheduling noise, not
a speed edge. Two controls confirm it:

- opt does **not** reach systematically deeper completed depth (total completed
  depth ratio opt/base = **0.995 ≈ 1**) — a 4% edge can't fund an extra ply even
  here.
- the **deeper-searching side won only 40.8%** of games (below 50%). Depth reached
  is driven by *position difficulty*, not skill: the side in the worse position
  has harder, less-forced choices and searches deeper, while the winning side's
  continuation is near-forced and converges fast. So "more search" isn't even an
  advantage signal in these head-to-head endgames — reinforcing that a speedup
  which only buys marginally more search buys no strength.

Even on the hardest, time-limited positions, the speedup nets zero playing
strength. (Data: `/tmp/egmatch_stuck/`.)

### Pre-endgame (PEG) strength

PEG is *breadth*-limited, not depth-limited — a halving beam-search over candidate
plays (stage 0 greedy-ranks all; then 32→16→8→4→2 at rising 2–6-ply fidelity, each
candidate's emptier leaves solved via the sped-up endgame). So a natural
hypothesis: 4% more candidate throughput could surface a better move. And unlike
the endgame's strict "last completed depth", a PARTIAL PEG stage that finishes ≥2
candidates *does* publish (peg.c:2896-2901) — so the mechanism is theoretically
viable here. Tested it (`pegstage` harness, 175 fixture positions, bag 1–4,
18 threads):

- **Move stability across the cascade** (`max_stage` k=1→5): the published move was
  **identical across all cascade depths for 29/32 positions**. Where it changed
  (3/32) it flipped at the stage 1→2 boundary then locked; deeper stages refine the
  win% value, not the move identity.
- **4% budget A/B** (3.0 s vs 3.12 s = ×1.04 — the deterministic model of a
  4%-faster engine): **0 of 60 positions changed their published move**; mean win%
  delta +0.000. (Thread nondeterminism itself was only 1/60 — PEG is near
  deterministic.)

So the 4% nets **0 PEG playing strength** too. The top move settles early
(best-first candidate scoring finds the winner among the first candidates), and
flipping it requires completing a whole additional stage — far more than a
few-percent throughput gain buys. It is the endgame branching-factor wall in a
different guise. The speedup is throughput, not strength, for the pre-endgame as
well.

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
- `egmove1` (transducer) + `tools/eg_match.py` — cross-process, time-banked,
  colour-balanced head-to-head strength match (baseline vs optimized).
- `pegstage` (test/benchmark_peg_test.c) — PEG move-stability across cascade
  stages + budget A/B, for the pre-endgame strength check.
- `src/impl/move_gen.c`, `src/impl/move_gen.h` — the five optimizations.

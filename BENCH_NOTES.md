# RIT sim benchmark exploration notes

Working notes from the session after PR review. **Delete before merge.**

## Summary of what shipped in this session

1. **Batched initial-phase BAI scheduling** (`src/impl/bai.h`). Each worker
   thread atomically reserves `num_arms` consecutive sample indices from
   the global counter and processes arms 0..num_arms−1 contiguously
   before going back for another batch. Replaces the previous block-mode
   scheduling where thread T processed `sample_minimum` consecutive
   samples of arm 0, then of arm 1, etc.
2. **Benchmark infrastructure** in `src/impl/autoplay.{c,h}` and
   `test/sim_benchmark_test.c`:
   - `autoplay_{get,reset}_total_sim_iterations()` — global atomic
     counter accumulating sim iterations across every turn of every
     game. `simbench` prints this + iters/sec.
   - `autoplay_set_bench_static_move(bool)` — when enabled, the sim
     still runs at every turn but autoplay plays the top-equity static
     move. Pins the game trajectory so different variants compare
     like-for-like positions. Without this, iters/sec varies by ±5%
     because sim quality influences which move is played, which
     influences game length.
   - `simbench` accepts `SIMBENCH_PLIES` env var to sweep ply depth.
   - `-DRIT_CACHE_INSTRUMENT` compile flag wires up hit/miss counters
     in `move_gen.c` (zero-cost when undefined).

## Headline result: batched BAI in all-initial-phase regime (MCTS target)

`simbench` config: `-lex CSW24 -wmp true -rit true -numplays 15 -plies P -threads 10 -tlim 2 -seed 42 -sr tt -minplayiterations 100000`, fixed game trajectory (static-move bench mode).

`minplayiterations=100000` forces the sim to stay in the initial (round-robin
equivalent) BAI phase throughout, which matches short MCTS inner sims with
no arm pruning.

| plies | block-mode iters/s | batched iters/s | speedup | block hit% | batched hit% |
|-------|-------------------:|----------------:|--------:|-----------:|-------------:|
| 1     | 33,361             | 41,833          | **+25.4%** | 5.83%  | 93.63% |
| 2     | 16,721             | 18,189          | **+8.8%**  | 10.30% | 65.38% |
| 3     | 10,322             | 11,469          | **+11.1%** | 6.61%  | 65.73% |
| 4     | 7,625              | 8,311           | **+9.0%**  | 5.41%  | 55.88% |
| 5     | 6,028              | 6,529           | **+8.3%**  | 4.76%  | 52.80% |
| 6     | 5,098              | 5,507           | **+8.0%**  | 4.42%  | 47.30% |
| 7     | 4,468              | 4,798           | **+7.4%**  | 4.24%  | 44.15% |

Interpretation: with batched scheduling a thread's 15 arms within one
iteration share a PRNG iter index and therefore the same pre-candidate
opp rack; the per-thread RIT cache hits on the ply-0 opp movegen. At
plies=1 the ply-0 call *is* the whole rollout, so hit rate approaches
the 14/15 = 93.3% theoretical ceiling.

## Parity effect in ply sweep

Odd plies (`1, 3, 5, 7`) *end* with an opponent movegen; even plies end
with a player movegen. Opponent racks are seed-derived (more
repeatable); player racks are candidate-derived (differ per arm). So:

| transition | delta  | new call |
|------------|-------:|----------|
| 1→2        | −28.3% | +player  |
| 2→3        | **+0.35%** | +opp     |
| 3→4        | −9.85% | +player  |
| 4→5        | −3.08% | +opp     |
| 5→6        | −5.50% | +player  |
| 6→7        | −3.15% | +opp     |

Adding an opp call pulls the average *up* when prior hit rate is still
above the equilibrium for deep plies (plies=2→3 nudges up ~0.3%);
otherwise it pulls down less than player calls do.

## Normal sim regime (`sample_minimum` default): batched = block-mode

Under the default BAI profile (short initial phase, TT phase dominates),
the batched change is neutral because only ~1% of samples live in the
initial phase. Measured with `tlim 2` and full game trajectory
(`autoplay_set_bench_static_move(true)`):

| config                    | iters/sec (median of 4) |
|---------------------------|------------------------:|
| No-RIT                    | 15,528                  |
| RIT + block-mode BAI      | 17,748                  |
| RIT + batched BAI         | 17,690                  |

So shipping the batched change costs nothing in the normal-sim case and
helps a lot in the MCTS case.

## Dead ends that cost time

### 1. Per-player cache split (direct-mapped 32+32 or 64+64)

Hypothesis: player A and player B evict each other in the shared 64-slot
cache. Splitting by player should recover those cross-player collisions.

Result (normal sim, fixed trajectory): **zero difference**. Hit rate
held at ~3.5% across all three variants.

Why: cross-player *hits* were 0.02% in the baseline, so split doesn't
sacrifice anything. Cross-player *evictions* were ~49%, but eliminating
them doesn't convert to hits because same-player eviction (~48%) would
evict that slot anyway. The workload generates enough unique racks per
second that capacity isn't the binding constraint.

A cache size of 1 tested at the same conditions gave the *same* hit
rate and iters/sec as size 64. So in the normal-sim workload, the cache
is essentially a repetition filter for the immediately-preceding lookup,
not a capacity-bound cache.

### 2. PRNG seed override (TLS-based race fix)

Hypothesis: each SimmedPlay has its own PRNG that advances once per
`simmed_play_get_seed` call. Different threads hit these PRNGs in a
non-deterministic order, so "iter N of arm 0" returns a racy value.
Forcing all arms in a batch to share a `splitmix64(iter_index)`-derived
seed would give byte-identical opp racks → maximum cache locality.

Implemented with `__thread` TLS (`simmed_play_seed_override`) set by the
BAI worker before each sample, cleared after. Gated by
`-DBAI_SEED_OVERRIDE`.

Result (all-initial-phase, plies sweep):

| plies | no override iters/s | with override iters/s | Δ |
|-------|--------------------:|----------------------:|--:|
| 1 | 41,965 | 40,939 | −2.4% |
| 2 | 18,170 | 17,941 | −1.3% |
| 3 | 11,486 | 11,425 | −0.5% |
| 4 | 8,320  | 8,272  | −0.6% |
| 5 | 6,562  | 6,531  | −0.5% |
| 6 | 5,495  | 5,483  | −0.2% |
| 7 | 4,808  | 4,784  | −0.5% |

Hit rate was *identical* across the two variants at every ply. The
override neither improves locality nor avoids a mutex acquire
measurably, and the TLS branch costs ~0.5% throughput. Dropped.

(Why no hit-rate change despite the race being real? The observed
hit rate under batched BAI appears to come from late-game turns where
the bag is small and random draws naturally repeat — not from the
in-batch same-iter mechanism I'd expected. The race doesn't *prevent*
same-iter hits often enough to matter for the average.)

### 3. Misleading early measurement

During exploration we saw "batched BAI +8%" and "per-player +X" numbers
that evaporated once `autoplay_set_bench_static_move` was added. The
earlier numbers were trajectory noise: sim quality influenced which
move was played, which influenced how many turns the game lasted, which
changed iters/sec because mid-game turns have different branching than
endgame turns. Any comparison at the sim-benchmark level *must* pin the
trajectory.

## Quick reference for future benchmarks

Build instrumented:
```
make clean && \
  make magpie_test BUILD=release -j8 \
    'cflags.release=-O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs -DRIT_CACHE_INSTRUMENT' \
    'cflags.test_release=-O3 -flto -march=native -Wall -Wno-trigraphs -DRIT_CACHE_INSTRUMENT'
```

Sweep plies:
```
for p in 1 2 3 4 5 6 7; do SIMBENCH_PLIES=$p ./bin/magpie_test simbench; done
```

Toggling off `autoplay_set_bench_static_move(true)` in
`test/sim_benchmark_test.c` exercises the full sim-picks-the-move path,
useful for end-to-end regressions but not for clean A/B comparisons.

Toggling `minplayiterations` in the `simbench` config string lets you
pick between MCTS-like (large value, all initial phase) and normal sim
(small / omitted, TT-phase-dominated) regimes.

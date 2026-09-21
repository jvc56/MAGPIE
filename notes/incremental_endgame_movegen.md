# Incremental endgame move generation: benchmark results

`EndgameArgs.incremental_movegen` (default off) makes the endgame solver derive
each node's `MOVE_RECORD_ALL_SMALL` move list from the same side's list two
moves up the search path, instead of generating it from scratch. Only the lanes
touched by the intervening moves are regenerated; untouched lanes carry over,
filtered by a packed rack sub-multiset test when the side's own move consumed
tiles. The implementation is in `src/impl/path_move_lists.{h,c}`, with
`generate_small_moves_in_lanes` in `src/impl/move_gen.c` as the lane-restricted
generator entry point.

This note records the measured effect of turning the flag on.

## Summary

- **1.26x** aggregate nodes per second at 4 plies, **1.12x** at 6 plies.
- Search results are identical with the flag on or off: same values, same node
  counts.
- With the flag off, speed is indistinguishable from the pre-change baseline,
  so the mechanism is free when disabled.

## Setup

- Harness: `egspeedbench` (`test_endgame_speed_bench` in
  `test/benchmark_endgame_test.c`).
- Lexicon: CSW21.
- Threads: 1, so node counts are deterministic.
- Positions: non-stuck endgame positions produced by `gennonstuck`
  (`/tmp/nonstuck_cgps.txt`, seed 31415). 100 positions at 4 plies, 40 at
  6 plies.
- Three configurations: baseline (`main` before the change), flag off, flag on.

### Machine

| | |
|---|---|
| Model  | Dell Precision 7680 (laptop) |
| CPU    | 13th Gen Intel Core i7-13850HX: 20 cores (8 performance + 12 efficiency), 28 threads, up to 5.3 GHz |
| Cache  | 28 MiB L2, 30 MiB L3 |
| Memory | 62 GiB |
| OS     | Ubuntu 24.04.5 LTS, Linux 6.8.0 |

The absolute nps figures are specific to this machine; the on/off ratios are
the portable result. On a hybrid CPU like this one, a single-threaded run can
land on either core type, which is another reason to compare ratios rather
than absolute numbers across runs.

## Speed

Aggregate nodes per second over all positions in the run:

| plies | positions | baseline nps | flag off nps | flag on nps | flag on vs. off |
|------:|----------:|-------------:|-------------:|------------:|----------------:|
| 4     | 100       | 116.1k       | 115.8k       | 146.5k      | 1.26x           |
| 6     | 40        | 217.9k       | 218.3k       | 243.7k      | 1.12x           |

Per-position ratios average about 1.2x and range from 0.87x to 2.3x. The few
positions that showed a slowdown came out as small wins when re-timed alone, so
those slowdowns were contention noise from benchmark runs executing in
parallel, not a real regression.

### Flag-off cost

Baseline and flag-off nps differ by -0.3% at 4 plies and +0.2% at 6 plies,
which is within noise. This meets the "feature flags must be free when off"
rule in `AGENTS.md`.

## Correctness

- All 140 benchmark solves produced identical values and node counts in all
  three configurations.
- A compile-time verifier cross-checked every derived list against a scratch
  generation at 4, 6 and 7 plies, with 4 threads, and through full time-limited
  playouts, with no mismatch.
- `test_incremental_movegen_identical` in `test/endgame_test.c` asserts equal
  score and node count with the flag on and off.
- The endgame test suite passes.

## Why the gain is not larger

A gprof profile at 5 plies shows where MAGPIE's endgame node time goes:

| component                                                      | share of node time |
|----------------------------------------------------------------|-------------------:|
| Full-list move generation                                      | about 25%          |
| Shadow-based best-move generation inside greedy leaf playouts  | about 23%          |
| Eager cross-set recompute after each move                      | about 18%          |
| Anchor maintenance                                             | about 6%           |

Incremental generation only addresses the first row, and only part of it:

- Only about one node in five generates a full list at all; the rest are
  leaves.
- Within the lists that are generated, lane regeneration still performs about
  63% of the original recursion, because the lanes a move touches are the busy
  ones.

Together these put roughly 1.25x close to the technique's ceiling in this
solver. The smaller gain at 6 plies than at 4 is consistent with that.

## Not covered

Per-node stuck-tile generation was left unchanged. It has its own cheap
cross-set fast path.

## Reproducing

```bash
make magpie_test BUILD=no_pgo_release
./bin/magpie_test gennonstuck          # writes /tmp/nonstuck_cgps.txt

# 4 plies, 100 positions
MAGPIE_BENCH_PLIES=4 MAGPIE_BENCH_MAX=100 MAGPIE_BENCH_INCREMENTAL=0 \
  MAGPIE_BENCH_TAG=off ./bin/magpie_test egspeedbench
MAGPIE_BENCH_PLIES=4 MAGPIE_BENCH_MAX=100 MAGPIE_BENCH_INCREMENTAL=1 \
  MAGPIE_BENCH_TAG=on ./bin/magpie_test egspeedbench

# 6 plies, 40 positions
MAGPIE_BENCH_PLIES=6 MAGPIE_BENCH_MAX=40 MAGPIE_BENCH_INCREMENTAL=0 \
  MAGPIE_BENCH_TAG=off ./bin/magpie_test egspeedbench
MAGPIE_BENCH_PLIES=6 MAGPIE_BENCH_MAX=40 MAGPIE_BENCH_INCREMENTAL=1 \
  MAGPIE_BENCH_TAG=on ./bin/magpie_test egspeedbench
```

Run the configurations one at a time on an otherwise idle machine; parallel
runs produced the contention noise noted above. `MAGPIE_PO_INCREMENTAL` is the
equivalent toggle for the `egplayout` harness.

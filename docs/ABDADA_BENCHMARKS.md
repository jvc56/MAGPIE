# ABDADA Parallel Endgame Solver Benchmarks

This document tracks benchmark results for the ABDADA parallel endgame solver.

## Test Configuration

- **Test positions**: Random 9-ply TWL98 endgames, seeds 45 and 49 (identified as hard endgames)
- **Hardware**: 16 logical cores available
- **TT Size**: 2GB (2^27 entries)

## Baseline Results (ABDADA without move rotation improvements)

| Configuration | Seed 45 | Seed 49 | Total | Speedup |
|---------------|---------|---------|-------|---------|
| 1 thread      | 237.62s | 253.09s | 490.70s | 1.00x |
| 8 threads     | 72.71s  | 52.65s  | 125.36s | 3.91x |
| 16 threads    | 62.24s  | 48.24s  | 110.48s | 4.44x |

**Observations:**
- 8→16 threads only improves by 13% (125s → 110s)
- Efficiency at 16 threads: 4.44/16 = 27.8%
- ABDADA deferred rate: ~0.3-0.6%

## Experiment 1: Physical Move Rotation at Root

**Hypothesis**: The rotation bonus of 10 was overwhelmed by HASH_MOVE_BF (1 << 28).
Physically rotating the move list (after sorting) should ensure threads actually
search different moves first.

**Implementation**: After sorting, rotate moves 1..N-1 by thread_idx positions,
keeping move 0 (TT/best move) fixed.

| Configuration | Seed 45 | Seed 49 | Total | Speedup |
|---------------|---------|---------|-------|---------|
| 1 thread      | 207.86s | 222.20s | 430.07s | 1.00x |
| 16 threads    | 67.92s  | 65.84s  | 133.76s | 3.22x |

**Result**: WORSE performance
- 16-thread speedup dropped from 4.44x to 3.22x
- Single-thread baseline also changed (possibly natural variance)
- The physical rotation seems to disrupt move ordering in a harmful way

**Conclusion**: Reverted. Physical rotation interferes with the carefully-sorted
move ordering too aggressively.

## Experiment 2: Aggressive Move Jitter with 4 Patterns

**Hypothesis**: More aggressive jitter at all depths could create better search diversity.

**Implementation**: 4 jitter patterns based on thread_idx % 4:
- Pattern 1: +5*tiles_played (favor exchanges)
- Pattern 2: -5*tiles_played (favor scoring)
- Pattern 3: Random ±16
- Pattern 4: Inverted score order

| Configuration | Seed 45 | Seed 49 | Total | Speedup |
|---------------|---------|---------|-------|---------|
| 16 threads    | 676.98s | 76.60s  | 753.57s | 0.57x (REGRESSION) |

**Result**: CATASTROPHIC regression. The inverted score pattern (pattern 4) causes
threads 4, 8, 12, 16 to search the worst moves first, completely destroying the
benefit of alpha-beta pruning.

**Conclusion**: Reverted. Move ordering is critical - even subtle disruptions can
cause massive performance regressions. Good move ordering is more important than
search diversity.

## Experiment 3: MIN_SPLIT_DEPTH = 3

**Hypothesis**: Lower MIN_SPLIT_DEPTH allows ABDADA deferral at more nodes,
potentially improving work sharing.

| Configuration | Seed 45 | Seed 49 | Total | vs Baseline |
|---------------|---------|---------|-------|-------------|
| 16 threads    | 66.02s  | 70.27s  | 136.29s | 23% slower |

**Result**: Worse performance. Increased deferral rate (0.67% vs 0.63%) adds overhead.

## Experiment 4: MIN_SPLIT_DEPTH = 5

**Hypothesis**: Higher MIN_SPLIT_DEPTH reduces ABDADA overhead.

| Configuration | Seed 45 | Seed 49 | Total | vs Baseline |
|---------------|---------|---------|-------|-------------|
| 1 thread      | 209.70s | 224.70s | 434.40s | ~same |
| 16 threads    | 76.99s  | 41.48s  | 118.47s | 7% slower |

**Result**: Mixed - Seed 49 faster (41s vs 48s), Seed 45 slower (77s vs 62s).
Lower deferral rate (0.55% vs 0.63%) but overall slightly worse.

**Conclusion**: MIN_SPLIT_DEPTH = 4 remains optimal. Values of 3 add too much
overhead, values of 5 reduce parallelism opportunities.

## Ideas Not Yet Tested

1. **Extend jitter to deeper plies**: Currently thread-based jitter only applies at depth > 2.
   Could extend to top 3-4 plies for more search diversity.

2. **Increase jitter amplitude**: Current ±3*tiles_played and 0-7 random is subtle.
   Larger values (without inverting order) might create more diversity.

4. **History heuristic**: Track moves that cause beta cutoffs for better move ordering.

5. **Killer moves**: Store moves that caused cutoffs at each ply level.

## Notes

- LMR (Late Move Reduction) was tried in a separate branch and did not improve results.
- The current implementation uses Lazy SMP + ABDADA hybrid approach.
- Cache-aligned nproc table (64-byte entries) prevents false sharing effectively.

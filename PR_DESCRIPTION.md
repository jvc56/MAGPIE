# Parallel Endgame Solver with Lazy SMP

## Summary

This PR implements a parallel endgame solver using Lazy SMP (Shared Memory Parallel) with staggered iterative deepening depths. Multiple threads search the same position with a shared transposition table, but start at different depths to reduce redundant work and improve TT population.

## Key Features

### Staggered Iterative Deepening
- Each thread starts iterative deepening at a different depth
- Normal mode: stagger +3 with smart wraparound (e.g., 8 threads on 13-ply start at depths 1,4,7,10,13,3,6,9)
- Avoids thread clustering when `gcd(stagger, plies) > 1` by adding offset for each wrap cycle

### Stuck Tile Detection
- At root position, detects if any tile on the player's rack cannot be played in any legal move
- When stuck tiles are detected, enables "slow-play" mode for search diversity

### Slow-Play Move Ordering
- In stuck tile positions, odd-numbered threads use alternative move scoring
- Normal scoring heavily prioritizes going out (playing all tiles)
- Slow-play scoring rewards keeping tiles on rack: `score + 2*our_tile_score + 2*opp_tile_score`
- Provides search diversity when the optimal strategy may be to avoid going out

### Shared Transposition Table
- All threads share a single TT for position caching
- ABDADA-style nproc tracking available (cache-line padded to prevent false sharing)
- TT hit rates of 20-45% observed in benchmarks

## Performance

**13-ply NWL20 endgame (vs_joey):**
- 1 thread: ~47s
- 8 threads: ~10s
- **Speedup: ~4.8x**

**10 random 6-ply TWL98 endgames:**
- Total wall clock: 29.85s
- CPU utilization: 614% (6.1 cores average)
- Range: 0.05s (trivial) to 12.0s (hard)

## Files Changed

- `src/impl/endgame.c` - Main parallel search implementation
- `src/ent/transposition_table.h` - TT with ABDADA nproc tracking
- `test/endgame_test.c` - Updated test configuration
- `test/benchmark_endgame_test.c` - Benchmark suite for performance testing

# ABDADA Parallel Endgame Solver

## Summary

This PR implements ABDADA (Alpha-Bêta Distribué avec Droit d'Aînesse = Distributed Alpha-Beta Search with Eldest Son Right) for multi-threaded endgame solving, achieving significant speedups on deep endgame searches.

ABDADA was developed by Jean-Christophe Weill and combines concepts from Young Brothers Wait Concept (YBWC) with a shared transposition table approach. The key insight is that the eldest (first) son at each node is always searched, while younger siblings are searched "exclusively" - meaning they're deferred if another processor is already searching them.

## Performance Results

Benchmarked on 10 games with 7-ply depth (CSW24, seed 42):

| Configuration | Total Time | Speedup |
|--------------|------------|---------|
| Main branch (1 thread) | 431.7s | baseline |
| ABDADA (1 thread) | 117.0s | 3.7x |
| ABDADA (6 threads) | 62.6s | 6.9x |

- **Algorithm improvements alone**: 3.7x speedup (even single-threaded) - from aspiration windows + incremental play/unplay
- **Parallel efficiency**: 53% with 6 threads (1.87x additional speedup)
- **Total speedup**: 6.9x with 6 threads over the baseline

The single-threaded speedups come from:
1. **Aspiration windows**: Narrow the alpha-beta search window based on the previous iteration's result, allowing more aggressive pruning
2. **Incremental play/unplay**: Use MoveUndo to efficiently backup and restore game state instead of full copy/restore

## Key Algorithmic Improvements

### 1. ABDADA Parallel Search
ABDADA coordinates parallel search using the "eldest son right" principle:

- **Eldest son always searched**: The first move at each node is always searched immediately (like YBWC)
- **Younger siblings searched exclusively**: Non-first moves check if another processor is already searching that node; if so, the move is deferred
- **Two-phase iteration**: First phase searches moves exclusively; second phase revisits any deferred moves
- **nproc tracking**: A counter in the transposition table tracks how many processors are currently searching each node

Key implementation details:
- Separate 256KB nproc table (vs multi-GB TT) for better cache locality
- Relaxed memory ordering for atomic operations to minimize contention
- Stack-allocated deferred array (up to 64 moves) to avoid malloc overhead
- Sentinel value `ON_EVALUATION` returned when a node is busy

### 2. Aspiration Windows
After depth 1, searches use a narrow window (±25) centered on the previous iteration's score:
- Faster searches when the value is stable
- Automatic re-search with wider windows on fail-high/fail-low
- Disabled for first-win optimization mode

### 3. Incremental Play/Unplay
Instead of copying and restoring the full game state at each node, the search uses:
- `play_move_incremental()` / `play_move_endgame_outplay()` to make moves with minimal state backup
- `unplay_move_incremental()` to restore state from a `MoveUndo` structure
- Lazy cross-set computation (only recompute when needed for move generation)
- Optimized outplay path that skips board updates when the player is going out

This reduces the per-node overhead significantly, especially in deep searches.

### 4. Lazy SMP Move Ordering Jitter
Different threads use different move ordering heuristics to explore different branches first:
- Thread 0: Pure score-based ordering (the "main" thread)
- Odd threads: Favor moves that play more tiles
- Even threads: Favor moves that play fewer tiles
- All non-zero threads get small random jitter (0-7) to break ties differently

This ensures threads don't all search the same moves in the same order.

### 5. Per-Thread PRNG
Each worker thread has its own Xoshiro PRNG seeded uniquely, enabling deterministic jitter without synchronization overhead.

### 6. Atomic Node Counting
Thread-safe node counting using `atomic_fetch_add` for accurate statistics across all threads.

### 7. Early Termination
Threads check `atomic search_complete` flag and can stop early when another thread has completed the full search.

### 8. Per-Ply Callback
New callback mechanism for iterative deepening progress:
```c
typedef void (*EndgamePerPlyCallback)(int depth, int32_t value,
                                       PVLine *pv, void *data);
```
Allows callers to receive updates after each depth is completed, useful for:
- Progress reporting during long searches
- PV display during iterative deepening
- Early termination based on external criteria

## Implementation Details

### New/Modified Files

- `src/ent/transposition_table.h`: Added nproc table and ABDADA helper functions
- `src/impl/endgame.c`: Core ABDADA implementation (~500 lines changed)
- `src/impl/endgame.h`: Added `num_threads` to EndgameArgs
- `test/benchmark_endgame_test.c`: New benchmark test suite
- `test/endgame_test.c`: Added multi-threaded correctness tests

### Transposition Table Changes

```c
// New 256K entry table for tracking concurrent node searches
atomic_uchar *nproc;

// Helper functions with relaxed memory ordering
transposition_table_is_busy(tt, zval)   // Check if node is being searched
transposition_table_enter_node(tt, zval) // Mark node as being searched
transposition_table_leave_node(tt, zval) // Unmark node when done
```

### Diagnostic Counters

New atomic counters for performance analysis:
- `deferred_count`: Moves deferred due to busy nodes
- `abdada_loops`: ABDADA loop iterations (>1 means deferrals occurred)
- `deferred_mallocs`: Heap allocations for large move lists

## API Changes

The `EndgameArgs` struct now includes:
```c
int num_threads;  // Number of worker threads for parallel search
```

Set to 1 for single-threaded behavior (default).

## Testing

- All existing endgame tests pass
- New multi-threaded correctness tests verify same results with 1 vs N threads
- Benchmark test suite for performance regression testing

## Future Work

- Tune aspiration window size for different endgame types
- Experiment with different nproc table sizes
- Consider lazy SMP depth-based jitter (different threads search to different depths)

# ABDADA Parallel Endgame Solver

## Summary

This PR implements ABDADA (Alpha-Beta Distributed Distributed Algorithm) for multi-threaded endgame solving, achieving significant speedups on deep endgame searches.

## Performance Results

Benchmarked on 10 games with 7-ply depth (CSW24, seed 42):

| Configuration | Total Time | Speedup |
|--------------|------------|---------|
| Main branch (1 thread) | 431.3s | baseline |
| ABDADA (1 thread) | 284s | 1.52x |
| ABDADA (6 threads) | 83s | 5.2x |

- **Algorithm improvements alone**: 1.52x speedup (even single-threaded) - from aspiration windows
- **Parallel efficiency**: 57% with 6 threads (3.4x additional speedup)
- **Total speedup**: 5.2x with 6 threads over the baseline

The single-threaded speedup comes primarily from aspiration windows, which narrow the alpha-beta search window based on the previous iteration's result, allowing more aggressive pruning.

## Key Algorithmic Improvements

### 1. ABDADA Parallel Search
ABDADA is a parallel alpha-beta algorithm that uses a shared transposition table with "nproc" tracking to coordinate work between threads:

- **Exclusive node searching**: Non-first moves are searched "exclusively" - if another thread is already searching the same node, the move is deferred
- **Two-phase iteration**: First phase searches moves exclusively; second phase revisits deferred moves
- **Reduced search overlap**: Threads explore different parts of the search tree rather than duplicating work

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

### 3. Lazy SMP Move Ordering Jitter
Different threads use different move ordering heuristics to explore different branches first:
- Thread 0: Pure score-based ordering (the "main" thread)
- Odd threads: Favor moves that play more tiles
- Even threads: Favor moves that play fewer tiles
- All non-zero threads get small random jitter (0-7) to break ties differently

This ensures threads don't all search the same moves in the same order.

### 4. Per-Thread PRNG
Each worker thread has its own Xoshiro PRNG seeded uniquely, enabling deterministic jitter without synchronization overhead.

### 5. Atomic Node Counting
Thread-safe node counting using `atomic_fetch_add` for accurate statistics across all threads.

### 6. Early Termination
Threads check `atomic search_complete` flag and can stop early when another thread has completed the full search.

### 7. Per-Ply Callback
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

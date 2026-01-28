# Optimize Zobrist Hashing Memory Layout

## Summary

This PR optimizes the Zobrist hashing implementation by converting heap-allocated 2D arrays to stack-allocated flat arrays, simplifying memory management.

## Changes

### Memory Layout Optimization (zobrist.h)

**Before:**
```c
typedef struct Zobrist {
  uint64_t their_turn;
  uint64_t **pos_table;        // heap-allocated 2D array
  uint64_t **our_rack_table;   // heap-allocated 2D array
  uint64_t **their_rack_table; // heap-allocated 2D array
  uint64_t scoreless_turns[3];
  XoshiroPRNG *prng;
  int board_dim;
  int ld_size;
} Zobrist;
```

**After:**
```c
typedef struct Zobrist {
  uint64_t their_turn;
  uint64_t pos_table[MAX_BOARD_DIM * MAX_BOARD_DIM * (MAX_ALPHABET_SIZE + 1)];
  uint64_t our_rack_table[(MAX_ALPHABET_SIZE + 1) * (RACK_SIZE + 1)];
  uint64_t their_rack_table[(MAX_ALPHABET_SIZE + 1) * (RACK_SIZE + 1)];
  uint64_t scoreless_turns[3];
  XoshiroPRNG *prng;
  int board_dim;
  int ld_size;
} Zobrist;
```

### Algorithm Simplification (zobrist_add_move)

Simplified the `zobrist_add_move` function by using `rack_add_letter`/`rack_take_letter` directly instead of manually tracking a placeholder rack array.

## Benchmark Results

Benchmarks run with `BUILD=release` (-O3 -flto -march=native), 8 threads:

### 10-ply Endgame (5 games, ~34% TT hit rate)

| Version | Total Time | Per Game |
|---------|------------|----------|
| With optimization | 372.9s | 74.58s |
| Without optimization | 371.9s | 74.38s |

### 6-ply Endgame (5 games, ~20% TT hit rate)

| Version | Total Time | Per Game |
|---------|------------|----------|
| With optimization | 55.3s | 11.07s |
| Without optimization | 55.2s | 11.04s |

### 3-ply Endgame (100 games, ~0.7% TT hit rate)

| Version | Total Time | Per Game |
|---------|------------|----------|
| With optimization | 107.1s | 1.071s |
| Without optimization | 106.9s | 1.069s |

**Conclusion:** Performance-neutral across all ply depths. The change is purely a code simplification that:
- Eliminates heap allocations for the hash tables
- Reduces memory fragmentation
- Simplifies cleanup (no need to free nested arrays)
- Makes the code easier to understand

## Test Plan

- [x] All existing tests pass (`make test`)
- [x] Endgame benchmark produces identical results (same PV, same node counts)
- [x] Code compiles cleanly with `-Wall -Wextra -Werror`

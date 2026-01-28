# Optimize Zobrist Hashing and Transposition Table Performance

## Summary

This PR optimizes the Zobrist hashing implementation with focus on memory layout and algorithmic improvements.

## Changes

### Memory Layout Optimization (zobrist.h)

**Before:**
```c
typedef struct Zobrist {
  uint64_t their_turn;
  uint64_t **pos_table;        // heap-allocated
  uint64_t **our_rack_table;   // heap-allocated
  uint64_t **their_rack_table; // heap-allocated
  uint64_t scoreless_turns[3];
  XoshiroPRNG *prng;
  int board_dim;
} Zobrist;
```

**After:**
```c
typedef struct Zobrist {
  // Hot: accessed on every move
  uint64_t their_turn;
  uint64_t scoreless_turns[3];
  uint64_t our_rack_table[ZOBRIST_MAX_LETTERS][RACK_SIZE];
  uint64_t their_rack_table[ZOBRIST_MAX_LETTERS][RACK_SIZE];
  // Cold: only accessed during initial hash calculation
  uint64_t pos_table[BOARD_DIM * BOARD_DIM][ZOBRIST_MAX_LETTERS * 2];
  // Very cold: only used during creation
  XoshiroPRNG *prng;
} Zobrist;
```

- **Inline arrays** instead of heap-allocated 2D arrays
- **Hot fields first** - rack tables (accessed every move) placed before pos_table (only used at game start)
- **Simpler memory management** - no per-row malloc/free calls in create/destroy
- **Removed unused `board_dim` field**

### Algorithm Optimization (zobrist_add_move)

- **Single pass through tiles** instead of two passes (one for board hash, one for building placeholder rack)
- **Direct rack table selection** via ternary instead of if/else
- **Cleaner incremental position tracking** with `row_inc`/`col_inc` instead of repeated conditionals

### Transposition Table Stats

- Added `TT_STATS` conditional compilation flag for collecting TT statistics (lookups, hits, collisions)

## Benchmark Results

100-game 3-ply endgame benchmark (CSW21, 8 threads, BUILD=release):

| Version | Run 1 | Run 2 | Run 3 | Average |
|---------|-------|-------|-------|---------|
| **With optimization** | 103.46s | 104.46s | 103.97s | **103.96s** |
| **Without optimization** | 103.90s | 102.67s | 103.89s | **103.48s** |

**Note:** The benchmark shows no statistically significant difference. This is expected because the 3-ply endgame benchmark has TT lookups=0 (transposition table not being exercised). The optimizations target:
1. Zobrist hash computation during TT probes
2. Memory allocation patterns during solver initialization

The structural improvements (inline arrays, simpler memory management, field ordering) provide:
- Reduced memory fragmentation
- Simpler code with no manual array allocation loops
- Better theoretical cache locality for rack tables

## Test Plan

- [x] All existing tests pass
- [x] Benchmark runs successfully
- [x] Memory sanitizer clean (no leaks from removed allocations)

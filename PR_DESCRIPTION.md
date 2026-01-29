## Summary

This PR implements incremental move play/unplay for the endgame solver, providing a **1.32x overall speedup** in endgame solving.

### Key Changes

1. **Incremental play/unplay**: Instead of copying the entire game state before each move and restoring it after, we now track only the changes made by a move in a `MoveUndo` structure and reverse them during unplay. This avoids expensive full-board copies during the alpha-beta search.

2. **Optimized cross-set updates**: Cross-sets are updated incrementally after unplay using the tracked tile positions, rather than recomputing them for the entire board.

3. **Deterministic Zobrist hashing**: Changed the transposition table's Zobrist hash to use a fixed seed (`12345`) instead of time-based seeding. This ensures deterministic behavior across runs, which is important for testing and debugging.

### New Functions

- `play_move_incremental()` - Plays a move while tracking changes in MoveUndo
- `unplay_move_incremental()` - Reverses a move using the tracked changes
- `play_move_endgame_outplay()` - Optimized path for moves that empty the rack (no unplay needed)
- `update_cross_sets_after_unplay_from_undo()` - Incrementally restores cross-sets

### Benchmark Results

Tested with 10 games per ply depth on an M4 Mac Mini:

| Ply | Main (s) | Optimized (s) | Speedup |
|-----|----------|---------------|---------|
| 1   | 0.170    | 0.183         | 0.93x   |
| 2   | 0.442    | 0.398         | 1.11x   |
| 3   | 1.374    | 0.955         | 1.44x   |
| 4   | 8.692    | 6.999         | 1.24x   |
| 5   | 15.884   | 11.006        | 1.44x   |
| 6   | 58.767   | 45.898        | 1.28x   |
| 7   | 134.974  | 95.688        | 1.41x   |
| 8   | 110.295  | 80.657        | 1.37x   |
| 9   | 216.449  | 158.696       | 1.36x   |
| 10  | 370.328  | 274.114       | 1.35x   |

**Overall: 917.4s → 694.6s (1.32x speedup)**

### Correctness

Both branches produce identical endgame solutions when using the fixed Zobrist seed, confirming the optimization does not affect correctness.

## Test Plan

- [x] All existing endgame tests pass
- [x] Benchmark shows consistent speedup at higher ply depths
- [x] Solutions match between main and optimized branches

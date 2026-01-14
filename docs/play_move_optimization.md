# play_move Optimization Ideas

## Profiling Results

From Instruments profiling of endgame solve (26.77 min total):

| Function | Time | % of Total |
|----------|------|------------|
| `play_move` | 17.74 min | 66.2% |
| `update_cross_set_for_move` | 12.75 min | 47.6% |
| `play_move_on_board` | 3.72 min | 13.9% |
| `game_backup` | 1.12 min | 4.2% |
| `standard_end_of_game_calculations` | 2.68 s | 0.2% |

**Key insight**: Cross-set updates alone are nearly half the total solve time.

## Optimization Ideas

### 1. Skip cross-set updates for outplays (Easy win)

When a move plays all tiles on the rack, the game ends. No subsequent move generation is needed, so cross-sets don't need updating.

```c
// In play_move or wherever update_cross_set_for_move is called
if (move->tiles_played < rack_get_total_letters(player_rack)) {
  update_cross_set_for_move(...);
}
```

**Expected impact**: Moderate - outplays are common and searched early in endgame.

### 2. Native play_small_move for endgame

Currently the search does:
1. `small_move_to_move()` - convert SmallMove to Move
2. `play_move()` - full game state update

A dedicated `play_small_move()` could:
- Skip the conversion step
- Skip unnecessary validation
- Skip game history recording
- Only do what endgame search needs:
  - Update board tiles
  - Update rack
  - Update score
  - Update cross-sets (when needed)
  - Store minimal undo info

**Expected impact**: Moderate - saves conversion and validation overhead.

### 3. Optimize cross-set update itself

Questions to investigate:
- Is it updating more squares than necessary?
- Is the KWG traversal the bottleneck?
- Could results be cached/memoized for common patterns?
- Are there memory access pattern issues (cache misses)?

Need to expand profiler on `update_cross_set_for_move` internals.

**Expected impact**: High if there's inefficiency, since it's 47.6% of time.

### 4. Lazy cross-set updates

Instead of updating cross-sets immediately after play_move, defer until move generation actually needs them. In many search branches, we hit TT cutoffs before generating moves.

**Complexity**: High - requires tracking dirty state.
**Expected impact**: Could be significant if many nodes cut off before movegen.

### 5. Reduce game_backup overhead

`game_backup` is 4.2% - worth checking what it's doing. For endgame search:
- We use `BACKUP_MODE_SIMULATION`
- Do we need full backup, or just enough for unplay?

### 6. Incremental board state

Instead of modifying the actual board and undoing, maintain a stack of deltas. Could improve cache locality and reduce memory writes.

**Complexity**: High - significant refactor.
**Expected impact**: Unknown.

## Priority Order

1. **Skip cross-sets for outplays** - Easy, clear win
2. **Profile cross-set internals** - Need data before optimizing
3. **Native play_small_move** - Medium effort, clear path
4. **game_backup investigation** - Quick check for easy wins
5. **Lazy cross-sets** - Only if data shows many TT cutoffs before movegen

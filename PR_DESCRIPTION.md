## Use word-pruned KWGs for cross-set computation in endgame solver

### Summary

The endgame solver already builds a pruned KWG (GADDAG) containing only words
that can physically be formed on the current board, used for main-axis move
generation. This PR additionally uses the pruned KWG for cross-set computation,
so perpendicular word checks traverse the smaller KWG.

**This is a safe optimization: the exact same set of legal moves is generated.**
A debug verification mode (`debug_verify_cross_set_equivalence`) confirms this
by generating moves with both pruned and full cross-sets at every node and
asserting the move lists are identical.

The speedup comes from two sources, both rooted in the pruned KWG having
fewer nodes:

1. **Faster cross-set computation.** Each incremental cross-set update
   (after play/unplay) traverses the pruned KWG instead of the full lexicon.
   Fewer nodes = less time per update.

2. **Earlier branch pruning during GADDAG move generation.** The pruned KWG
   excludes perpendicular words that can't physically be formed with the
   available tiles. This produces tighter cross-sets at some board positions
   (fewer allowed letters), which causes the GADDAG traversal in
   `generate_moves` to reject dead-end branches earlier — before checking
   rack availability or attempting further extension. The same legal moves
   are output, but fewer intermediate nodes are visited.

Also adds dual-lexicon support for the pruned KWGs (IGNORANT and INFORMED
modes), so cross-set pruning works correctly when players use different word
lists.

### Changes

- **`game.c` / `game.h`**: Add `override_kwgs[2]` and `dual_lexicon_mode` to
  `Game`. New `game_set_override_kwgs()` / `game_clear_override_kwgs()` API.
  Cross-set generation functions (`game_gen_classic_cross_set`,
  `game_gen_alpha_cross_set`) route through `get_kwg_for_cross_set()` which
  uses the override KWG when set.

- **`endgame.c` / `endgame.h`**: Build per-player pruned KWGs in
  `endgame_solver_reset()`. In `endgame_solver_create_worker()`, set override
  KWGs on each worker's game copy and regenerate cross-sets. Added
  `skip_pruned_cross_sets` flag in `EndgameArgs` for A/B benchmarking.
  Added `debug_verify_cross_set_equivalence` flag that asserts move-list
  equivalence between pruned and full cross-sets at every `generate_stm_plays`
  call (guarded by `#ifndef NDEBUG`, zero cost in release builds).

- **`game_defs.h`**: New `dual_lexicon_mode_t` enum (IGNORANT / INFORMED).

- **Dual-lexicon endgame tests**: New test positions using TWL98 vs CSW24
  to verify correct behavior in both lexicon modes.

- **Cross-set equivalence test**: `test_cross_set_pruning_equivalence` runs
  short endgame solves (2-ply, single-threaded) with verification enabled,
  covering both single-lexicon and dual-lexicon INFORMED modes.

### Benchmarking

**100-game interleaved A/B** (5-ply, 8 threads, seed 12345, randomized
first-mover per position):

```
Pruned (new):   857.57s  avg 8.576s/game
Unpruned (old): 1318.51s  avg 13.185s/game
Speedup: +35.0%
New faster: 73/100 positions
Order: new ran 1st in 48/100 positions
```

**Single-threaded deterministic** (Game 6 from seed 42, 6-ply, 4 trials each):

```
Pruned avg:   116.78s  (116.27-117.31s, identical PVs across trials)
Unpruned avg: 123.45s  (122.87-123.75s, identical PVs across trials)
Speedup: ~5.4%
```

The multi-threaded 35% figure includes ABDADA variance (hard positions
have high variance in parallel search). The single-threaded result
confirms a consistent, deterministic speedup.

### Profiling (Instruments Time Profiler)

Single-threaded, single position (Game 9 from seed 12345, 5-ply).

**Cross-set filtered view** — fewer KWG nodes means each cross-set
traversal completes faster:

| Metric | Old | New | Change |
|--------|-----|-----|--------|
| Total cross-set time | 13.60s | 9.20s | **-32%** |
| Cross-set samples | 14,193 | 9,195 | **-35%** |
| Heaviest stack `kwg_node` | 199ms | 120ms | **-40%** |

**`negamax_greedy_leaf_playout` filtered view** — both faster cross-set
updates and earlier branch pruning during move generation reduce overall
time:

| Function | Old | New | Change |
|----------|-----|-----|--------|
| Total | 1.51 min | 1.26 min | **-17%** |
| Samples | 90,639 | 75,398 | **-17%** |
| `generate_moves` | 4.70s | 3.09s | **-34%** |
| `update_cross_sets_after_unplay_from_undo` | 2.31s | 796ms | **-66%** |
| `play_move_incremental` | 2.07s | 1.62s | **-22%** |

The `generate_moves` speedup is not from generating fewer moves (the move
lists are identical) but from tighter cross-sets pruning GADDAG branches
earlier during traversal, reducing intermediate node visits.

### Test plan

- [x] Existing endgame tests pass (values unchanged)
- [x] New dual-lexicon endgame tests pass
- [x] Cross-set equivalence verified (pruned and full produce identical moves)
- [x] Profiling confirms speedup is in cross-set traversal and move generation

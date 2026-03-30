# PEG Solver Optimization Ideas

## Currently implemented

1. **Word prune once at PEG root** - `generate_possible_words` + `make_kwg_from_words_small` called once; all per-scenario endgame solves use `skip_word_pruning=true` with `game_get_effective_kwg` fallback. Saves ~10% on endgame passes by eliminating ~48 redundant KWG rebuilds per position.

2. **Cross-set sharing across scenarios** - After playing a candidate move, cross-sets are updated once via `update_cross_set_for_move_from_undo`. All ~8 bag-tile scenarios reuse the same cross-set state (only racks change between scenarios).

3. **Shared transposition table** - A single TT is created once and shared across all endgame passes and threads. Entries from shallower passes remain valid at deeper depths thanks to the depth guard in TT lookup.

4. **MOVE_RECORD_BEST_SMALL in greedy playout** - Uses shadow upper-bound pruning to find the best move without full enumeration, via the existing `negamax_greedy_leaf_playout` infrastructure.

## Near-term optimizations

### Forward-only greedy playout (no undo tracking)

The current greedy playout in `negamax_greedy_leaf_playout` saves a `MoveUndo` per move and unplays them all in reverse at the end. For PEG greedy evaluation, the board state is bulk-restored from a snapshot after each scenario, so the per-move undo tracking and unplay loop are unnecessary.

A PEG-specific greedy playout could:
- Call `play_move_on_board` + `update_cross_set_for_move` + score update (no MoveUndo)
- Skip the reverse-unplay loop entirely
- Restore state from a saved `Board` memcpy + rack/score restoration

This requires replicating the conservation heuristic (stuck-tile detection and bonus) from `negamax_greedy_leaf_playout` to preserve correct greedy rankings.

Estimated savings: ~15-25% on greedy pass (eliminates square-change tracking, bitmap updates, and the full reverse-unplay loop per playout).

### Board state save/restore via memcpy

Instead of `play_move_incremental` + `unplay_move_incremental` for candidate placement (which tracks ~100+ square changes per move), save the board as a single `memcpy(&saved, board, sizeof(Board))` before playing the candidate, and restore with one `memcpy` after all scenarios complete.

This eliminates:
- Per-square bitmap tracking in MoveUndo
- The reverse-restore loop in `move_undo_restore_squares`
- Cross-set reverse update via `update_cross_sets_after_unplay_from_undo`

Tradeoff: `sizeof(Board)` is ~28KB per save/restore, but a single memcpy is faster than 100+ tracked square changes.

### Skip cross-set updates during greedy playout

Cross-set updates between greedy moves account for a significant fraction of greedy playout time. Since the greedy playout is a heuristic, slightly stale cross-sets after the first move may be acceptable. The key question is whether the resulting move-generation errors materially affect candidate rankings.

If stale cross-sets cause unacceptable greedy ranking errors, a middle ground: update cross-sets only for the first 1-2 greedy moves (where accuracy matters most for scoring) and skip updates for later moves.

### Early cutoff pruning

Skip remaining bag-tile scenarios for a candidate once it cannot possibly reach the K-th best win% among completed candidates. For candidates evaluated in the endgame pass, if after 5 of 8 scenarios a candidate has 0 wins, it can't beat a candidate with 4+ wins.

This requires evaluating scenarios in a smart order (e.g., "killer draws" that tend to produce losses first) so that losing scenarios are discovered early for maximum pruning.

### Parallel scenario evaluation within a candidate

Currently, each candidate's ~8 scenarios are evaluated sequentially. With `num_threads > 1`, scenarios for a single candidate could be evaluated in parallel. This requires per-thread endgame solver instances (already supported) and synchronization for the shared TT.

## Medium-term optimizations

### First-win optimization for endgame stages

Use narrow-window search (alpha=-1, beta=+1) for fast win/loss detection instead of full-spread search. Much faster due to cutoffs. Compute spread only for candidates with tied win% on the final stage.

Modes:
- **PRUNE_ONLY**: First pass with first_win to prune losers, then re-eval survivors with full spread
- **WIN_PCT_THEN_SPREAD**: First_win on all stages; spread only for tied-win% candidates on final stage
- **WIN_PCT_ONLY**: Never compute spread (fastest, but no tiebreaking)

### Candidate partitioning by tiles played

Partition candidates into buckets by number of tiles played:
- **Bag-emptying** (play all rack tiles): opponent draws 1 tile, becomes endgame
- **Non-bag-emptying** (play fewer tiles): bag still has tiles, recursive evaluation needed

Evaluate bag-emptying candidates first (simpler, faster). Promote minimum candidates per tile-length bucket to ensure diversity.

### Aspiration windows for endgame stages

Use narrow aspiration windows centered on the previous stage's value for endgame stages. If the search fails high or low, widen progressively. Saves time when the value estimate from the previous stage is accurate.

### Move cap for interior endgame nodes

Limit the number of moves searched at interior (non-root) endgame nodes. At 1-ply, only the top N moves by score need evaluation since deeper positions resolve remaining uncertainty. This reduces branching factor at the cost of completeness.

## Longer-term ideas

### 2-bag PEG support

Extend to 2-tile-in-bag positions. For non-bag-emptying candidates, the opponent faces their own 1-bag PEG position after drawing. Requires recursive PEG calls and bingo threat detection.

### Incremental stage promotion

Reuse previous stage's endgame values for scenarios that were "decisive" (large spread margin). Only re-evaluate scenarios where the outcome was close. This exploits the observation that most scenarios don't change their winner between 1-ply and 2-ply.

### Pre-computed board state sharing

For the endgame pass, all scenarios for a candidate share the same board (different racks). Pre-compute the move generation's board-dependent state (anchors, GADDAG traversal starts) once and share across scenarios.

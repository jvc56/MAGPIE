# Handoff: Exchange Cutoff Optimization for Inference

## Context

The inference engine determines what racks could have led to an observed play. We're trying to add a **cutoff optimization** that allows early termination of move generation when we know a rack can't possibly be valid.

**Current State:**
- Scoring play cutoff optimization: WORKING (1.18x-1.52x speedup)
- Exchange cutoff optimization: NOT WORKING (causes sample count mismatches)

## What's Working: Scoring Play Cutoff

For scoring plays in `src/impl/inference.c`:
```c
const Equity cutoff_threshold = inference->target_score + current_leave_value +
                                inference->equity_margin;
```

This works because:
1. We iterate through all possible leaves for a played rack
2. For each leave, we know `current_leave_value` from the KLV
3. The observed play had equity = `target_score + actual_leave`
4. A rack is valid if best_play_equity <= target_score + current_leave_value + margin
5. We can pass `cutoff_threshold` to movegen and stop early if any play exceeds it

## The Exchange Cutoff Problem

For exchanges, the situation is fundamentally different:

1. When someone exchanges N tiles, we DON'T know which tiles were kept (the leave)
2. The exchange equity = `leave_value(kept_tiles)` (score is 0)
3. We're iterating through all possible 7-tile racks
4. For each rack, movegen computes `best_leaves[target_leave_size]` where `target_leave_size = 7 - N`
5. A rack is valid if:
   - The best play is an N-tile exchange (matches observed action), OR
   - best_play_equity <= `best_leaves[target_leave_size] + equity_margin`

The problem: we can't know `best_leaves[target_leave_size]` until AFTER movegen runs, so we can't set a meaningful cutoff threshold beforehand.

## What I Tried

### Approach 1: Deferred Threshold Update
In `gameplay.c:get_top_equity_move_with_exchange_cutoff()`:
- Pass `target_score + equity_margin` as `initial_best_equity`
- Set `target_leave_size_for_exchange_cutoff` to tell movegen to update threshold after computing `best_leaves`
- After exchange generation completes, update: `gen->initial_best_equity += best_leaves[target_leave_size]`

**Problem:** The `threshold_exceeded` flag was being set during exchange generation BEFORE the threshold was updated with `best_leaves`. This caused the anchor loop to terminate early, missing tile placements.

### Approach 2: Skip Threshold Check for Exchanges
Added in `move_gen.c`:
```c
const bool is_exchange_in_exchange_cutoff_mode =
    (move_type == GAME_EVENT_EXCHANGE) &&
    (gen->target_leave_size_for_exchange_cutoff >= 0);
if (gen->stop_on_exceeding_threshold &&
    move_equity_or_score > gen->initial_best_equity &&
    !is_exchange_in_exchange_cutoff_mode) {
  gen->threshold_exceeded = true;
}
```

**Problem:** After this fix, we got a SCORING PLAY mismatch (53245 vs 53244 samples). This suggests there's a pre-existing WMP cutoff bug that gets triggered under certain conditions.

### Approach 3: Simplified Exchange Path (Current State)
Removed exchange cutoff optimization entirely. For exchanges, just run full movegen:
```c
} else {
  // For exchanges: target_score is 0, current_leave_value is 0.
  const Equity cutoff_threshold =
      inference->target_score + current_leave_value + inference->equity_margin;
  top_move = get_top_equity_move(inference->game, inference->thread_index,
                                 inference->move_list);
  is_within_equity_margin = cutoff_threshold >= move_get_equity(top_move);
}
```

This is correct but provides no optimization for exchanges.

## Key Files

- `src/impl/inference.c` - Main inference logic, `evaluate_possible_leave()`
- `src/impl/gameplay.c` - `get_top_equity_move_with_exchange_cutoff()` (lines 499-560)
- `src/impl/move_gen.c` - Move generation with early cutoff support
- `test/infer_test.c` - Test comparing cutoff vs non-cutoff results

## Key Data Structures

- `MoveGen.initial_best_equity` - Threshold for early termination
- `MoveGen.stop_on_exceeding_threshold` - Enable early termination
- `MoveGen.target_leave_size_for_exchange_cutoff` - Signal to update threshold after best_leaves computed
- `MoveGen.threshold_exceeded` - Set when a move exceeds threshold
- `MoveGen.best_leaves[N]` - Best leave value for N-tile leave, computed during exchange generation

## Suggested Next Steps

### Option A: Two-Pass Approach
1. First pass: Generate just exchanges to compute `best_leaves[target_leave_size]`
2. If best exchange leave + margin < some heuristic, run second pass with cutoff
3. Pros: Conceptually simpler
4. Cons: Overhead of separate exchange-only pass

### Option B: Anchor-Level Cutoff
Instead of stopping entire movegen when threshold exceeded:
1. Complete all exchange generation normally
2. Update threshold with `best_leaves[target_leave_size]`
3. For each anchor, compute max possible score with remaining tiles
4. Skip anchor if max_possible_score < threshold
5. This is similar to WMP pruning but with dynamic threshold

### Option C: Post-Movegen Check Only
Don't try to optimize movegen for exchanges. Instead:
1. Run full movegen as usual
2. Check result: if top_move exceeds threshold AND is not matching exchange, skip this rack
3. Pros: Zero risk of bugs
4. Cons: No speedup for exchanges (but exchanges are rare)

### Option D: Fix the WMP Cutoff Bug First
The 53245 vs 53244 mismatch suggests there's a latent bug in the WMP cutoff path. Before adding exchange cutoff:
1. Reproduce the scoring play mismatch
2. Investigate why WMP cutoff produces different results
3. Fix that bug first
4. Then retry exchange cutoff

## Testing

Run the comparison test:
```bash
make magpie_test BUILD=release
./bin/magpie_test infercmp 2>&1 | tee /tmp/infercmp_output.txt
```

Look for:
- "Overall: X.XXx speedup" = success
- "mismatch" in output = failure (sample counts differ between cutoff and non-cutoff)

## Good Luck!

This is a tricky optimization because the exchange case has a circular dependency: you need `best_leaves` to set the threshold, but `best_leaves` is computed during movegen. The scoring play case doesn't have this problem because we know the leave value before movegen starts.

The current code is correct but doesn't optimize exchanges. If exchanges are rare enough in practice, Option C (no exchange optimization) may be acceptable. If exchange optimization is important, Option A or B would be the way to go.

You've got this!

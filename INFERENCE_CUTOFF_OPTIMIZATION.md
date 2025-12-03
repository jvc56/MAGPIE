# Inference Cutoff Optimization

This document describes the work done to optimize inference by using early termination (cutoff) during move generation.

## Background

Inference determines what rack a player likely had based on a move they played. For each possible rack that could have made the observed play:
1. Generate all legal moves for that rack
2. Check if the observed move is "recordable" (the best or near-best choice)
3. If recordable, count this rack as a valid possibility

The bottleneck is step 1: generating all moves for potentially millions of racks.

## The Optimization Idea

Instead of generating ALL moves to find the true best, we can use a **cutoff threshold**. If ANY move exceeds this threshold, we know the observed move isn't optimal for this rack, so we can stop early.

### For Scoring Plays (Working)

When the observed move was a scoring play with score S and leave value L:
- **Cutoff threshold**: `S + L + equity_margin`
- **Logic**: If any move has equity > threshold, this rack is not recordable
- **Implementation**: Stop move generation as soon as we find such a move

### For Exchanges (Work in Progress)

When the observed move was an exchange of N tiles:
- The exchange's "equity" is its leave value (score is 0)
- During exchange generation, `best_leaves[i]` tracks the best leave value for keeping `i` tiles
- **Cutoff threshold**: `best_leaves[RACK_SIZE - N] + equity_margin`
- **Logic**: If any scoring play exceeds this, no N-tile exchange can be optimal
- **Implementation**: After generating exchanges, set the cutoff and enable early termination for scoring plays

## Implementation Details

### Key Data Structures (move_gen.h / move_gen.c)

```c
// In MoveGen struct:
Equity initial_best_equity;        // The cutoff threshold
bool stop_on_exceeding_threshold;  // Enable early termination
bool threshold_exceeded;           // Set true when a move exceeds threshold
Equity best_leaves[RACK_SIZE + 1]; // Best leave value for each leave size
```

### Scoring Play Cutoff (Working)

**gameplay.c** - `get_top_equity_move_with_cutoff()`:
```c
bool get_top_equity_move_with_cutoff(Game *game, int thread_index,
                                     MoveList *move_list, Equity target_cutoff) {
  MoveGenArgs args = {
    // ... standard args ...
    .target_cutoff = target_cutoff,  // The threshold
  };
  generate_moves_for_game(&args);
  const MoveGen *gen = get_movegen(thread_index);
  return gen->threshold_exceeded;
}
```

**move_gen.c** - Threshold setup (in `gen_load_position`):
```c
if (args->target_cutoff != EQUITY_INITIAL_VALUE) {
  gen->initial_best_equity = args->target_cutoff;
  gen->stop_on_exceeding_threshold = true;
}
```

**move_gen.c** - Early termination check (in `update_best_move_or_insert_into_movelist`):
```c
// Check if this move exceeds the threshold for early termination
if (gen->stop_on_exceeding_threshold &&
    move_equity_or_score > gen->initial_best_equity) {
  gen->threshold_exceeded = true;
}
```

**move_gen.c** - Breaking out of loops (lines 748, 1963):
```c
if (gen->threshold_exceeded) {
  break;
}
```

**inference.c** - Using the cutoff:
```c
if (inference->use_cutoff_optimization &&
    inference->target_number_of_tiles_exchanged == 0) {
  cutoff_total_calls++;
  const bool cutoff_exceeded = get_top_equity_move_with_cutoff(
      inference->game, inference->thread_index, inference->move_list,
      cutoff_threshold);
  if (cutoff_exceeded) {
    cutoff_triggered_count++;
  }
  is_within_equity_margin = !cutoff_exceeded;
  top_move = move_list_get_move(inference->move_list, 0);
}
```

### Exchange Cutoff (Not Working Correctly)

**gameplay.h** - Function signature:
```c
// For exchange inference: returns true if any scoring move exceeds the best
// leave value for the target exchange size plus equity_margin. Uses early
// termination to stop as soon as such a move is found.
// If cutoff_used is not NULL, it will be set to the actual cutoff value used
// (best_leave_for_target + equity_margin).
bool get_top_equity_move_with_exchange_cutoff(Game *game, int thread_index,
                                              MoveList *move_list,
                                              int target_num_exch,
                                              Equity equity_margin,
                                              Equity *cutoff_used);
```

**gameplay.c** - Implementation:
```c
bool get_top_equity_move_with_exchange_cutoff(Game *game, int thread_index,
                                              MoveList *move_list,
                                              int target_num_exch,
                                              Equity equity_margin,
                                              Equity *cutoff_used) {
  MoveGenArgs args = {
    // ... standard args ...
    .target_num_exch_for_cutoff = target_num_exch,
    .exchange_cutoff_margin = equity_margin,
  };
  generate_moves_for_game(&args);
  const MoveGen *gen = get_movegen(thread_index);
  if (cutoff_used != NULL) {
    *cutoff_used = gen->initial_best_equity;
  }
  return gen->threshold_exceeded;
}
```

**move_gen.c** - Exchange cutoff setup (lines 2040-2055):
```c
void generate_moves(const MoveGenArgs *args) {
  MoveGen *gen = get_movegen(args->thread_index);
  gen_load_position(gen, args);
  gen_look_up_leaves_and_record_exchanges(gen);  // Populates best_leaves[]

  // For exchange inference: after generating exchanges, compute cutoff from
  // best_leaves and apply it for scoring play generation.
  // If ANY scoring play exceeds best_leave + margin, the exchange is suboptimal
  // and we can terminate early.
  if (args->target_num_exch_for_cutoff > 0) {
    const int target_leave_size = RACK_SIZE - args->target_num_exch_for_cutoff;
    const Equity best_leave_for_target = gen->best_leaves[target_leave_size];
    const Equity cutoff =
        best_leave_for_target + args->exchange_cutoff_margin;
    gen->initial_best_equity = cutoff;
    gen->stop_on_exceeding_threshold = true;
  }

  // ... continue with scoring play generation ...
  gen_shadow(gen);
  gen_record_scoring_plays(gen);
  gen_record_pass(gen);
}
```

**inference.c** - Exchange cutoff path (lines 178-201):
```c
} else if (inference->use_cutoff_optimization &&
           inference->target_number_of_tiles_exchanged > 0) {
  // For exchange inference: use the best leave for the target exchange size
  // as the cutoff for early termination.
  cutoff_total_calls++;
  const bool cutoff_exceeded = get_top_equity_move_with_exchange_cutoff(
      inference->game, inference->thread_index, inference->move_list,
      inference->target_number_of_tiles_exchanged, inference->equity_margin,
      NULL);
  if (cutoff_exceeded) {
    cutoff_triggered_count++;
    is_within_equity_margin = false;
  } else {
    top_move = move_list_get_move(inference->move_list, 0);
    is_within_equity_margin =
        inference->equity_margin >= move_get_equity(top_move);
  }
  top_move = move_list_get_move(inference->move_list, 0);
}
```

## The Problem with Exchange Cutoff

### Observed Behavior
- Over 200 games, cutoff version produces ~5500 fewer samples than non-cutoff
- This means the cutoff version incorrectly rejects some valid racks

### Why It's Tricky

1. **Exchange equity = leave value**: An exchange of N tiles has equity = leave value of the (RACK_SIZE - N) tiles kept. Score is 0.

2. **`best_leaves[]` vs specific exchange**: `best_leaves[target_leave_size]` is the BEST leave value across ALL subracks of that size. But for a specific rack, the actual best exchange might have a lower leave value.

3. **`compare_moves` and `top_move`**: When early termination happens:
   - A scoring play exceeded the cutoff
   - `compare_moves()` compares it to the current best (the exchange)
   - If the scoring play wins, it becomes `top_move`
   - This affects the `number_exchanged_matches` fallback check

4. **Recordability logic**: A rack is recordable if:
   ```c
   const bool recordable = is_within_equity_margin || number_exchanged_matches ||
                           rack_is_empty(inference->bag_as_rack);
   ```
   - `is_within_equity_margin`: Best move's equity <= threshold
   - `number_exchanged_matches`: Best move IS an exchange of the target size
   - When cutoff triggers, `top_move` becomes the scoring play, so `number_exchanged_matches = false`

### Scenarios Where Mismatch Could Occur

Consider a rack where:
- `best_leaves[2] = 10` (best 2-tile leave from some subrack)
- The actual best 5-tile exchange for THIS rack has leave value 6
- Best scoring play has equity 8
- `equity_margin = 3`

**Non-cutoff path**:
- Generates all moves, finds best = scoring play (equity 8)
- `is_within_equity_margin = (3 >= 8) = false`
- `number_exchanged_matches = false` (best is scoring, not exchange)
- `recordable = false`

**Cutoff path**:
- Cutoff = 10 + 3 = 13
- Scoring play equity 8 < 13, so `cutoff_exceeded = false`
- `top_move` = scoring play with equity 8
- `is_within_equity_margin = (3 >= 8) = false`
- `number_exchanged_matches = false`
- `recordable = false`

Same result in this case. But there may be edge cases where:
- The cutoff triggers when it shouldn't
- The `top_move` after early termination differs from what full generation would produce
- The thresholds don't align properly

## Potential Fixes to Explore

1. **Don't override `best_move_equity_or_score`**: When setting the cutoff, avoid changing which move is considered "best" - only use it for early termination.

2. **Preserve exchange as top_move**: When cutoff triggers, ensure the exchange (not the exceeding scoring play) remains accessible for the `number_exchanged_matches` check.

3. **Use consistent thresholds**: Make sure the `is_within_equity_margin` check in the else branch uses the same semantics as the non-cutoff path.

4. **Debug with specific CGP**: The test showed mismatches for specific positions like:
   ```
   10ALPHA/9MM4/8QUIST2/11P3/10XI3/10UG3/11O3/6FOB2T3/7HENDS3/15/15/15/15/15/15 CDNVVZ?/EEILRRR 93/174 0
   ```
   5 tiles exchanged (CDNVV). Tracing through this specific case could reveal the bug.

## Files Modified

- `src/impl/move_gen.h`: Added `initial_best_equity`, `stop_on_exceeding_threshold`, `threshold_exceeded` to MoveGen struct
- `src/impl/move_gen.c`: Added threshold checking logic, early termination breaks, exchange cutoff setup
- `src/impl/gameplay.h`: Added `get_top_equity_move_with_cutoff()` and `get_top_equity_move_with_exchange_cutoff()` declarations
- `src/impl/gameplay.c`: Implemented the cutoff functions
- `src/impl/inference.h`: Added `use_cutoff_optimization` field to Inference struct
- `src/impl/inference.c`: Added cutoff paths for both scoring and exchange inference
- `test/infer_test.c`: Added comparison test running both cutoff and non-cutoff, checking for sample count match

## Test Command

```bash
make clean && make magpie_test BUILD=release && ./bin/magpie_test infercmp
```

The `infercmp` test runs 200 games with both cutoff enabled and disabled, comparing sample counts to detect mismatches.

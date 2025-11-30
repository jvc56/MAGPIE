# Inference Cutoff Optimization Project History

This document captures the full history of the inference cutoff optimization work, including collaboration between Claude and Gemini.

## Project Goal

Optimize MAGPIE's inference engine by implementing early termination (cutoff) during move generation. Inference determines what rack a player likely had based on a move they played - this requires generating all legal moves for potentially millions of candidate racks.

**The optimization idea**: Instead of generating ALL moves to find the true best, use a cutoff threshold. If ANY move exceeds this threshold, we know the observed move isn't optimal for this rack, so we can stop early.

---

## Phase 1: Initial Implementation

### Scoring Play Cutoff (Working)

For scoring plays, the cutoff is straightforward:
- Observed play had score `S` and leave value `L`
- Cutoff threshold = `S + L + equity_margin`
- If any move has equity > threshold, this rack is not recordable
- Stop move generation immediately when such a move is found

**Result**: 1.18x-1.52x speedup on scoring plays

### Exchange Cutoff (The Hard Part)

For exchanges, the situation is fundamentally different:
- When someone exchanges N tiles, we DON'T know which tiles were kept (the leave)
- Exchange equity = `leave_value(kept_tiles)` (score is 0)
- We need `best_leaves[target_leave_size]` to set the threshold, but this is computed DURING movegen

**Circular dependency problem**: You need `best_leaves` to set the threshold, but `best_leaves` is computed during movegen.

---

## Phase 2: Claude's Exchange Cutoff Attempts

### Attempt 1: Deferred Threshold Update
- Pass initial threshold to movegen
- Set `target_leave_size_for_exchange_cutoff` to tell movegen to update threshold after computing `best_leaves`
- After exchange generation completes, update the threshold

**Problem**: The `threshold_exceeded` flag was being set during exchange generation BEFORE the threshold was updated. This caused the anchor loop to terminate early, missing tile placements.

### Attempt 2: Skip Threshold Check for Exchanges
Added logic to not set `threshold_exceeded` during exchange generation:
```c
const bool is_exchange_in_exchange_cutoff_mode =
    (move_type == GAME_EVENT_EXCHANGE) &&
    (gen->target_leave_size_for_exchange_cutoff >= 0);
```

**Problem**: Got a SCORING PLAY mismatch (53245 vs 53244 samples). This revealed a latent bug in WMP cutoff path.

### Attempt 3: Simplified Exchange Path
Removed exchange cutoff optimization entirely. For exchanges, just run full movegen.

**Result**: Correct but provides no optimization for exchanges.

---

## Phase 3: Gemini's Investigation

Gemini picked up the work and investigated a release-mode divergence:

### The Problem
- `bin/magpie_test infercmp` failed with `num_samples mismatch: 8004811 vs 7991815` (Optimized > Baseline)
- Only occurred in Release builds, not Debug
- Isolated to: Game 1, Seed 13345, Rack `CDNVVZ?` (has blank), Exchange 5 tiles

### Gemini's Hypotheses
1. **Sort Instability**: `generate_moves` might produce ties between Exchange and Scoring Play. In Release mode, sort order might differ.
2. **Uninitialized Memory**: `MoveGen` reuse or struct initialization might have uninitialized fields in Release mode.
3. **Optimization/UB**: Compiler might be optimizing away something due to Undefined Behavior.

### Gemini's Changes
- Added `printf` debugging in `has_blank` fallback block of `get_top_equity_move_with_exchange_cutoff`
- Changed `target_leave_size_for_exchange_cutoff` to `-1` in fallback block (didn't fix issue)
- Added `test_infer_cutoff_repro` test to reproduce failing case
- Reduced test to 5 games for faster iteration

---

## Phase 4: Bug Fixes and Working State

### Key Commits
- `29d8397c` - Fix WMP cutoff optimization bug and reduce test games to 5
- `80a2dddd` - Fix inference cutoff comparison test floating-point instability

### Current Working State
- **Scoring play cutoff**: WORKING with WMP
- **Exchange cutoff**: Disabled (runs full movegen for exchanges)
- **Tests pass**: Both cutoff and non-cutoff produce identical sample counts

---

## Phase 5: Benchmarking (Current Session - November 30, 2025)

### 1000-Game WMP Benchmark Results

| Metric | With Cutoff | Without Cutoff | Speedup |
|--------|-------------|----------------|---------|
| **Total Time** | 1487.66s | 1842.49s | **1.24x** |
| **Scoring Plays** | 515.83s | 792.12s | **1.54x** |
| **Exchanges** | 971.83s | 1050.37s | **1.08x** |

- **Cutoff triggered**: 90.56% of the time
- **Anchor skip rate**: 79.13% with cutoff vs 54.14% without

### 100-Game Non-WMP Benchmark Results

| Metric | With Cutoff | Without Cutoff | Speedup |
|--------|-------------|----------------|---------|
| **Total Time** | 405.54s | 537.42s | **1.33x** |
| **Scoring Plays** | 92.78s | 187.46s | **2.02x** |
| **Exchanges** | 312.76s | 349.97s | **1.12x** |

Key observation: Exchanges are ~4x slower without WMP (~22-24s each vs ~5-7s with WMP).

### 1000-Game Non-WMP Benchmark (In Progress)
Running in background, expected ~70-90 minutes. Results will be auto-committed and pushed when complete.

---

## Key Files Modified

- `src/impl/move_gen.h` - Added `initial_best_equity`, `stop_on_exceeding_threshold`, `threshold_exceeded`, `target_leave_size_for_exchange_cutoff`
- `src/impl/move_gen.c` - Threshold checking logic, early termination breaks, exchange cutoff setup
- `src/impl/gameplay.h` - Added `get_top_equity_move_with_cutoff()` and `get_top_equity_move_with_exchange_cutoff()`
- `src/impl/gameplay.c` - Implemented cutoff functions
- `src/impl/inference.h` - Added `use_cutoff_optimization` field
- `src/impl/inference.c` - Cutoff paths for scoring and exchange inference
- `test/infer_test.c` - Comparison tests, reproduction tests

---

## Suggested Future Work

### Option A: Two-Pass Approach for Exchanges
1. First pass: Generate just exchanges to compute `best_leaves[target_leave_size]`
2. If best exchange leave + margin < some heuristic, run second pass with cutoff

### Option B: Anchor-Level Cutoff
1. Complete all exchange generation normally
2. Update threshold with `best_leaves[target_leave_size]`
3. For each anchor, compute max possible score with remaining tiles
4. Skip anchor if max_possible_score < threshold

### Option C: Accept Current State
Exchange optimization adds complexity for modest gain. Exchanges are relatively rare in games. Current state is correct and provides good speedup for scoring plays.

---

## Handoff Documents

- `CLAUDE_ADVICE_AND_INSTRUCTIONS_FOR_GEMINI.md` - Claude's handoff notes to Gemini
- `GEMINI_ADVICE_AND_INSTRUCTIONS_FOR_CLAUDE.md` - Gemini's handoff notes back to Claude
- `INFERENCE_CUTOFF_OPTIMIZATION.md` - Technical documentation of the optimization
- `INFERENCE_BENCHMARK_RESULTS.md` - Benchmark results

---

## Timeline Summary

1. **Initial work**: Claude implemented scoring play cutoff (working) and attempted exchange cutoff (buggy)
2. **Handoff to Gemini**: Claude documented issues and handed off to Gemini
3. **Gemini investigation**: Found release-mode divergence, added debugging, isolated reproduction case
4. **Bug fixes**: Fixed WMP cutoff bug and floating-point instability
5. **Benchmarking**: Running comprehensive benchmarks to validate performance gains

---

*Document created: November 30, 2025*

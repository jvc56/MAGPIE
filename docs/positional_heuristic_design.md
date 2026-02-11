# Positional Heuristic Design for MAGPIE

## Motivation

MAGPIE currently evaluates moves as:

```
equity = score + leave_value + endgame_adjustments
```

This ignores the **board state after the move**: which premium squares are exposed, what cross-word scoring opportunities the opponent gains, how "open" or "closed" the board becomes. Maven (the strongest classic Scrabble AI) uses a cross-word position evaluation system that considers the opponent's scoring opportunities at each position adjacent to the placed tiles. This document proposes a similar system for MAGPIE, trainable from self-play with the same infrastructure used for leave values, and extremely cheap to evaluate at move generation time.

## What Maven Does (for reference)

Maven maintains a linked list of board positions adjacent to the played word. For each such position, it computes upper and lower bounds on the opponent's cross-word scoring opportunity. It tracks the **minimum** exposure across all positions (the position giving the opponent the least advantage) and uses this to differentiate between moves with the same raw score + leave value.

The key insight: two moves with identical score and leave can differ dramatically in what they give the opponent. A blank placed next to a TLS gives less than a high-value tile placed there.

## Design Goals

1. **Trainable from self-play** using the existing leavegen-style infrastructure
2. **O(1) per tile** evaluation during move generation (no DAWG traversal, no simulation)
3. **Additive** with existing equity formula: `equity = score + leave + positional_adj`
4. **Statistically rigorous**: values derived from millions of game samples, not hand-tuned
5. **Small memory footprint**: fits in L1/L2 cache

## Core Concept: Per-Square Placement Penalty Table

### The Idea

Precompute two parallel tables indexed by `(square, tile_value)`:

1. **Mean table**: the expected equity cost of placing a tile of a given point value on a given square
2. **Variance table**: how much uncertainty/risk that placement creates — the spread of possible opponent benefits

When you place a tile on a square, the opponent may later play through that square. The expected benefit the opponent derives depends on:
- **The tile's point value**: a 10-point tile on a TLS gives the opponent 30+ points; a blank gives 0
- **Which square it is**: squares adjacent to premium squares are more costly
- **How open the board is**: placements near premiums that are reachable from existing tiles are more costly than placements near premiums that are far from any hook. Board openness is largely independent of game stage — a game where early words reach the edges is more open than a mid-game with a tight central cluster

### Why Two Tables?

Two moves can have the same **expected** positional penalty but very different **risk profiles**:

- Placing a Z next to an unused TLS: the opponent almost certainly uses it (low variance, predictable penalty)
- Placing a Z one square away from a TWS with an open hook: the opponent scores huge IF they can reach it, but may not have the right tiles (high variance)

This matters strategically. When **ahead**, you prefer low-variance positions — don't give the opponent a chance to close the gap with a lucky rack. When **behind**, you prefer high-variance positions — create opportunities for swings in either direction. This is directly analogous to Maven's approach of tracking both upper and lower bounds on opponent scoring opportunity.

### Why This Works

The mean value of placing a specific tile on a specific square can be estimated as:

```
mean_penalty(square, tile_points) = E[opponent_benefit(square, tile_points)] - E[opponent_benefit(square, avg_tile)]
```

This is the **excess** opponent benefit from placing a high-value tile here versus an average tile. The "average tile" baseline is already captured by leave values (which implicitly account for average board interactions). The positional adjustment captures the **deviation from average** for this specific placement.

The variance value captures the spread:

```
var_penalty(square, tile_points) = Var[opponent_benefit(square, tile_points)]
```

High variance means the placement creates a volatile situation — the eventual impact depends heavily on what tiles the opponent draws.

## Proposed Table Structure

### Table 1: Square Placement Values (SPV) — Mean and Variance

Two parallel 2D tables with the same dimensions:

- `spv_mean[square_index][tile_score_bucket]` — expected equity cost
- `spv_var[square_index][tile_score_bucket]` — variance of equity cost

Dimensions:
- **square_index**: 0..224 for a 15x15 board (or 0..449 for both directions, since horizontal and vertical placements on the same square differ in which neighbors they expose)
- **tile_score_bucket**: 0..10 (tile point values 0, 1, 2, 3, 4, 5, 6, 8, 10; blanks = 0)

**Size**: 2 tables x 450 squares x 11 buckets x 4 bytes = **39,600 bytes** (fits in L1/L2 cache)

**Values**: Equity adjustments in the same units as leave values (hundredths of a point or equivalent). Trained from self-play. The mean table is zero-sum across squares for each bucket (prevents double-counting with leave values). The variance table is non-negative.

### Table 2: Board Openness Multiplier (optional)

A 1D table: `openness_mult[openness_bucket]`

Board openness is **not** the number of tiles on the board — it captures how far play has spread toward the edges and whether premium squares are reachable. Two cheap-to-maintain metrics:

#### Option A: Bounding Box Extent

Track the bounding box of all tiles on the board (min_row, max_row, min_col, max_col). These 4 integers are O(1) to maintain — just update the min/max after each move. The openness metric is the maximum extent from center:

```
extent = max(7 - min_row, max_row - 7, 7 - min_col, max_col - 7)
```

- **extent = 0**: only the center star played (completely closed)
- **extent = 3-4**: play is still in the inner region, corner premiums unreachable
- **extent = 6-7**: play reaches the edges, most premiums are exposed

Bucket into ~5 levels: `openness_bucket = min(extent / 2, 4)`.

#### Option B: Hooks at Extremities (more precise)

Track whether anchor squares exist in the outermost rows/columns of the bounding box. A bounding box reaching row 1 with no anchor squares along that edge is much less dangerous than one with open hooks there, because without hooks the opponent cannot play through that region.

```
openness = number of board edges (top/bottom/left/right) where:
    the bounding box is within 2 rows/cols of the edge
    AND anchor squares exist in the outermost 2 rows/cols on that side
```

This gives a value 0-4 (how many sides of the board are "open"). MAGPIE already maintains anchor state, so this is cheap to query. It can be computed once per position rather than per move generated.

**Size**: 5 x 4 bytes = **20 bytes**

#### Why not tile count?

Tile count is a poor proxy for openness. Consider:
- 50 tiles in a tight central cluster → premiums unreachable → closed board
- 20 tiles spread to the edges with hooks near TWS squares → wide open board

These have opposite risk profiles despite the first having 2.5x more tiles. The bounding box / hook-at-extremity metrics capture the actual strategic state.

### Evaluation at Move Time

For each tile placed (during shadow_record or equity calculation):

```c
Equity mean_adj = 0;
Equity var_adj = 0;
for each fresh tile placed:
    int sq = square_index(row, col, dir);
    int bucket = tile_score_to_bucket(tile_point_value);
    mean_adj += spv_mean[sq][bucket];
    var_adj += spv_var[sq][bucket];

// Combine mean and variance using spread-dependent weighting:
Equity positional_adj = mean_adj + spread_weight * var_adj;

// Optional openness scaling:
positional_adj *= openness_mult[board_openness_bucket];
```

The `spread_weight` is a function of the current score differential:
- **When ahead**: `spread_weight` is negative (penalize high variance — play safe)
- **When even**: `spread_weight` is zero or near-zero (variance-neutral)
- **When behind**: `spread_weight` is positive (reward high variance — seek swings)

A simple formulation: `spread_weight = clamp(spread / K, -1.0, 1.0)` where `spread` = opponent's lead in points and `K` is a scaling constant (~50-100 points) tuned from self-play. The clamp prevents extreme weighting.

The `board_openness_bucket` is computed once per position (not per move), so it adds zero cost to the per-move loop. The bounding box is maintained incrementally at O(1) per move played.

**Cost**: Two table lookups per tile placed (mean + variance), plus one multiply and one add at the end. During shadow playing, you already iterate over tiles to compute score. This adds two indexed memory accesses per tile — still negligible compared to the KWG traversal and leave map operations already happening. Both tables together (~20KB after symmetry) fit comfortably in L1 cache.

## Training Methodology

### Overview

Train SPV mean and variance values from self-play games, analogous to how leave values are trained. The key statistical quantities are:

```
For each (square, tile_score_bucket) pair:
  Collect: equity_outcome of moves that placed a tile of this bucket on this square
  Compare to: equity_outcome of moves that placed a tile of this bucket on an "average" square
  SPV_mean[square][bucket] = mean(outcome - baseline)
  SPV_var[square][bucket]  = variance(outcome - baseline)
```

Mean captures the expected positional cost. Variance captures how volatile the position is — how much the outcome depends on the opponent's subsequent draws and choices.

### Detailed Training Procedure

#### Phase 0: Establish Baseline

1. Train leave values to convergence using the existing leavegen infrastructure
2. Play N million self-play games with converged leave values, recording all moves
3. For each played move, record the **game outcome relative to equity prediction**:
   ```
   residual = actual_game_value - predicted_equity
   ```
   where `actual_game_value` = (final score difference from this player's perspective), and `predicted_equity` = (score + leave_value) at the time of the move

#### Phase 1: Accumulate Per-Square Statistics

For each move played in self-play:
1. For each fresh tile placed:
   - Look up `(square_index, tile_score_bucket)`
   - Add the move's residual to `sum_residuals[sq][bucket]`
   - Add the move's residual² to `sum_sq_residuals[sq][bucket]`
   - Increment `count[sq][bucket]`
2. After sufficient games, compute mean and variance:
   ```
   raw_mean[sq][bucket] = sum_residuals[sq][bucket] / count[sq][bucket]
   raw_var[sq][bucket]  = sum_sq_residuals[sq][bucket] / count[sq][bucket]
                          - raw_mean[sq][bucket]²
   ```

The variance computation uses the standard one-pass formula: `Var(X) = E[X²] - E[X]²`. For numerical stability with large sample counts, Welford's online algorithm can be used instead, but the one-pass formula is adequate here since residuals are bounded.

#### Phase 2: Subtract Baseline

The raw mean values contain noise from leave value imprecision and game variance. Subtract the per-bucket baseline (average across all squares for each bucket):

```
baseline_mean[bucket] = mean over all sq of raw_mean[sq][bucket]
spv_mean[sq][bucket] = raw_mean[sq][bucket] - baseline_mean[bucket]
```

This ensures the mean adjustment is zero-sum across squares for each tile value, which prevents it from conflicting with leave values.

For variance, subtract the per-bucket baseline variance to isolate the **excess variance** attributable to the specific square:

```
baseline_var[bucket] = mean over all sq of raw_var[sq][bucket]
spv_var[sq][bucket] = raw_var[sq][bucket] - baseline_var[bucket]
```

After baseline subtraction, `spv_var` can be negative (meaning a square has less variance than average — a "safe" placement) or positive (more variance than average — a volatile placement). This is the desired behavior: the spread_weight then penalizes or rewards deviation from average volatility.

#### Phase 3: Regularization

Squares with few observations (e.g., corner squares rarely played on) will have noisy estimates. Apply shrinkage to both tables:

```
spv_mean_reg[sq][bucket] = spv_mean[sq][bucket] * count[sq][bucket] / (count[sq][bucket] + prior_count)
spv_var_reg[sq][bucket]  = spv_var[sq][bucket]  * count[sq][bucket] / (count[sq][bucket] + prior_count)
```

where `prior_count` is a tunable hyperparameter (e.g., 1000). This shrinks low-sample-count estimates toward zero (the baseline). Variance estimates are inherently noisier than mean estimates (they depend on the fourth moment), so a larger `prior_count` for variance may be appropriate.

#### Phase 4: Iterative Refinement (Generational)

Like leave value training, iterate:
1. Play games using current `score + leave + spv` evaluation
2. Recompute residuals and update SPV table
3. Repeat for K generations

Each generation's SPV values should converge as the evaluation improves and games become more representative of optimal play.

### Alternative: Direct Regression

Instead of the residual-based approach, fit SPV values via linear regression:

```
residual_i = sum_over_tiles_placed(spv[sq_j][bucket_j]) + noise
```

Solve the normal equations for `spv` values that minimize squared residuals. This is statistically cleaner but requires storing all move data and solving a linear system with ~5000 unknowns. Feasible but more complex to implement.

### Sample Size Requirements

Each cell of the SPV table needs sufficient observations. Variance estimates require more samples than mean estimates (they depend on the fourth moment for their own estimation error). With:
- 225 unique positions (after diagonal symmetry) x 11 buckets = 2,475 cells
- Target: 10,000 observations per cell minimum (for mean); 20,000+ for reliable variance
- Average tiles placed per move: ~4
- Average cells hit per game: ~50 (both players, ~12 moves each, ~4 tiles)
- Games needed for mean: ~2,475 * 10,000 / 50 = **~500,000 games**
- Games needed for mean + variance: ~2,475 * 20,000 / 50 = **~1,000,000 games**

This is comparable to leave value training requirements and is entirely feasible with MAGPIE's multi-threaded autoplay.

## Integration Points in MAGPIE

### Data Structure

```c
// In a new header: src/ent/spv.h
typedef struct SPV {
    Equity mean[BOARD_DIM * BOARD_DIM][TILE_SCORE_BUCKETS];  // 225 canonical positions
    Equity var[BOARD_DIM * BOARD_DIM][TILE_SCORE_BUCKETS];   // same dimensions
    Equity openness_mult[OPENNESS_BUCKETS];                  // 5 buckets (0-4 open sides)
    // Board openness state (maintained incrementally):
    int bb_min_row, bb_max_row, bb_min_col, bb_max_col;     // tile bounding box
} SPV;
```

### Move Generation Hot Path

In `shadow_record()` (move_gen.c line 1258), after computing `score` and `equity`:

```c
// Current code:
Equity equity = score;
if (gen->move_sort_type == MOVE_SORT_EQUITY) {
    equity += static_eval_get_shadow_equity(...);
}

// Add positional adjustment (mean + spread-weighted variance):
if (gen->spv && gen->number_of_tiles_in_bag > 0) {
    equity += shadow_get_positional_adjustment(gen, spread_weight);
}
```

The `shadow_get_positional_adjustment` function iterates over tiles played in the current shadow state (already tracked by the generator), sums `spv_mean[sq][bucket]` and `spv_var[sq][bucket]` for each fresh tile, and combines them: `mean_adj + spread_weight * var_adj`. The `spread_weight` is computed once per position from the current score differential. This adds ~8 memory accesses per move (two per tile placed), which is negligible compared to the existing work.

### During Full Move Recording

In `static_eval_get_move_equity_with_leave_value()` (static_eval.h), add:

```c
Equity positional_adj = 0;
if (spv && number_of_tiles_in_bag > 0) {
    positional_adj = spv_evaluate(spv, move, board, spread_weight);
}
return move_get_score(move) + leave_adjustment + positional_adj + other_adjustments;
```

### Autoplay Integration

Add a new autoplay mode `AUTOPLAY_TYPE_SPV_GEN` that:
1. Plays games with current leave + SPV values
2. Records per-move residuals with tile placement details
3. Accumulates into SPV table
4. Writes SPV data per generation (like KLV files)

This parallels the existing `AUTOPLAY_TYPE_LEAVE_GEN` infrastructure.

## Refinements and Extensions

### Board Symmetry

The 15x15 board's premium layout has 8-fold geometric symmetry (4 rotations x 2 reflections), but **positional evaluation has only 2-fold symmetry** (reflection about the main diagonal). The other symmetries are broken by word directionality:

- **Left-right reflection is invalid**: back-hooks (appending a letter to extend a word rightward) are far more common than front-hooks (prepending leftward). A tile at column 13 exposes the TWS at column 14 cheaply — the opponent just appends one letter. A tile at column 1 exposes the TWS at column 0, but the opponent must prepend, which requires a valid front-hook. These mirror positions have genuinely different risk profiles.
- **Top-bottom reflection is invalid**: the same hook asymmetry applies vertically. Extending a word downward (appending) is easier than extending upward (prepending).
- **90°/270° rotations are invalid**: these combine a direction swap with an axis reversal, inheriting the above asymmetries.
- **Diagonal reflection IS valid**: square (r,c) played horizontally has the same SPV as square (c,r) played vertically, because the premium layout is diagonal-symmetric and swapping row/column just swaps the "across" vs "down" direction without changing the hook asymmetry.

Exploit the diagonal symmetry to reduce the table by 2x:

```
canonical_index = min(square_index_horizontal(r,c), square_index_vertical(c,r))
```

This reduces the table from 450 entries to 225 unique positions. Total table: 225 x 11 = 2,475 cells. With 1M games, that's ~20,000 samples per cell — adequate statistical power.

### Neighbor-Aware Variant

Instead of `spv[square][bucket]`, use `spv[square][bucket][neighbor_state]` where `neighbor_state` encodes whether adjacent premium squares are still open or already used:

- 0: all adjacent premiums available
- 1: some adjacent premiums used
- 2: all adjacent premiums used

This triples the table size but captures the most important board-state dependency. Alternatively, absorb this into the phase multiplier.

### Cross-Word Specific Table

A more Maven-like approach: instead of per-square values, compute per-cross-word-position values. For each position adjacent to the played tiles where a cross word could form, penalize based on what the opponent could score there.

MAGPIE already computes cross scores during move generation (`gen_cache_get_cross_score`). The cross score represents what the current player scores from cross words. The complementary question — what the opponent could score by playing through these positions later — is what the positional heuristic should capture.

This could be implemented as:

```c
for each fresh tile with a cross-word position:
    int exposed_value = tile_point_value * letter_multiplier;
    // If an adjacent premium (DWS/TWS/DLS/TLS) is still open:
    positional_adj -= exposed_value * premium_exposure_penalty;
```

This is more principled than a flat per-square table and captures the same information Maven uses, but requires inspecting adjacent squares (already cached in `row_cache`).

### Interaction with Leave Values

Leave values already capture average positional effects implicitly. For example, holding an S is valuable partly because S-hooks open fewer premium squares than other letters. To avoid double-counting:

1. **Train SPV after leave values converge** (not simultaneously)
2. **Use residuals** from leave-adjusted equity, not raw scores
3. **Enforce zero-sum** per bucket across squares (the baseline subtraction in Phase 2)
4. **Retrain leaves after SPV converges** with SPV active, then iterate

This alternating optimization converges because SPV and leaves capture orthogonal information (SPV = where you play, leaves = what you keep).

## Implementation Phases

### Phase 1: Infrastructure (1-2 days)
- Add SPV data structure with mean + variance tables and I/O (load/save binary)
- Add SPV evaluation function (mean + spread-weighted variance lookup per tile)
- Add spread_weight computation from score differential
- Wire into `static_eval` and `shadow_record`

### Phase 2: Training (2-3 days)
- Add residual tracking to autoplay (record actual outcome - predicted equity per move, with tile placement data)
- Add SPV accumulation for both mean and variance (sum, sum-of-squares, count per cell)
- Add symmetry folding (diagonal only)
- Add generational training loop (parallel to leavegen)

### Phase 3: Validation (1-2 days)
- Compare equity prediction accuracy with/without SPV (lower residual variance = better)
- Separately validate mean-only vs mean+variance to measure variance contribution
- Run game pairs: SPV-enabled vs SPV-disabled, and mean-only vs mean+variance
- Check convergence across generations for both tables
- Verify no regression in move generation speed (should be <1% overhead)
- Test spread_weight: games where the AI is behind should show more volatile play

### Phase 4: Refinement (ongoing)
- Experiment with neighbor-aware and cross-word-specific variants
- Retrain leave values with SPV active
- Tune regularization and openness scaling
- Test on tournament positions (like the PABOUCHE example)

## Compute Cost Summary

| Operation | Cost per move | Notes |
|-----------|--------------|-------|
| Score calculation | ~15 multiplies, adds | Existing |
| Leave value lookup | ~7 bit ops + 1 table read | Existing (LeaveMap) |
| **SPV lookup** | **~8 table reads + 8 adds + 1 mul** | **New: mean + var per tile placed** |
| Cross set validation | ~4 KWG node reads | Existing |

The SPV lookup is cheaper than any existing component of move evaluation. The ~20KB of tables (after diagonal symmetry, mean + variance) fit entirely in L1 cache and will remain hot across all moves generated from a single rack.

## File Format

Binary SPV file (`.spv`):

```
Header (20 bytes):
  magic: "SPV1" (4 bytes)
  num_squares: uint32 (225 after diagonal symmetry)
  num_buckets: uint32 (11)
  num_openness: uint32 (5, or 0 if no openness multiplier)
  flags: uint32 (bit 0 = has_variance_table)

Mean values (num_squares * num_buckets * sizeof(Equity)):
  mean[0][0], mean[0][1], ..., mean[0][10],
  mean[1][0], ...,
  ...

Variance values (num_squares * num_buckets * sizeof(Equity)):  [if flags & 1]
  var[0][0], var[0][1], ..., var[0][10],
  var[1][0], ...,
  ...

Openness multipliers (num_openness * sizeof(Equity)):
  openness_mult[0], ..., openness_mult[4]
```

Total file size (with variance): 20 + 2*(225*11*4) + 5*4 = **19,820 bytes**.
Total file size (mean only): 20 + 225*11*4 + 5*4 = **9,920 bytes**.

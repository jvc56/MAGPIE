# Dynamic Leave Values via Full-Rack Equity Table

## Motivation

MAGPIE's current leave values are **static**: they are precomputed assuming the full bag distribution and never change during a game. When you keep a leave of {A, E, R}, the KLV assigns a fixed value regardless of whether the bag still has plenty of S tiles (making draws like AERS likely) or whether all S tiles are already on the board.

This is a significant approximation. Late in a game, when 60+ tiles have been played, the bag composition can deviate dramatically from the full distribution. A leave holding {Q, U} is much better when the bag is depleted of U tiles (since you already have one) versus when U tiles remain plentiful. Static leaves can't capture this.

### What Static Leaves Get Wrong

The static leave value for a partial rack L is:

```
KLV(L) = [Σ_D C(full_bag, D) × E(L ∪ D)] / [Σ_D C(full_bag, D)] - avg_equity
```

where D ranges over all possible draws to complete the rack, C(full_bag, D) is the number of ways to draw D from a full bag, and E(L ∪ D) is the equity of the completed rack.

At game time with a depleted bag B, the correct leave value is:

```
DLV(L, B) = [Σ_D C(B, D) × E(L ∪ D)] / [Σ_D C(B, D)] - avg_equity(B)
```

The difference is the draw weights C(B, D) vs C(full_bag, D). With a depleted bag, some draws become impossible (tiles exhausted) and others become more likely (tiles concentrated). The static leave misweights all of these.

### The Fix

Store the full-rack equity table E(R) for all valid 7-tile racks (~888K entries). When needed, recompute the **entire KLV** using the actual bag composition. This is the same computation as `rack_list_write_to_klv()` — the same code path, the same `generate_leaves` recursion — just with the depleted bag instead of the full bag. The result is a complete KLV that drops into the existing infrastructure: `generate_exchange_moves` populates the LeaveMap from it, and every move in every subsequent game gets exact bag-aware leave values via the normal O(1) lookup.

### Primary Use Case: Simulation

In Monte Carlo simulation, many games are played forward from a common root position. All these games share the same initial bag state. This makes full KLV recomputation the right approach:

1. At the root position, note the bag composition (tiles not on board, not on known racks)
2. Run `recompute_klv_for_bag(req, bag, &dynamic_klv)` — same algorithm as `rack_list_write_to_klv()`
3. Use the dynamic KLV for all simulation games from this root
4. Every move in every simulation uses exact bag-aware leave values at O(1) cost — zero per-move overhead beyond the normal LeaveMap lookup

The recomputation cost is paid once per root position and amortized across all simulation games. With a depleted bag, the computation is much cheaper than the full-bag version used during training (most of the 888K racks are non-drawable and are skipped immediately).

## Background: Current Leave Training Pipeline

Understanding the existing system is essential since the new system extends it minimally.

### Training (rack_list.c)

1. **Enumerate all valid 7-tile racks**: `rack_list_create()` generates all multisets of 7 tiles drawable from the letter distribution. For English Scrabble: **888,030 racks**.

2. **Play self-play games**: For each turn, the player's rack is looked up in the rack list and the best move's equity is recorded:
   ```c
   rack_list_add_rack(rack_list, player_rack,
                      equity_to_double(move_get_equity(best_move)));
   ```
   This accumulates a running mean: `rli->mean += (1/count) * (equity - mean)`.

3. **Force rare racks**: Racks that haven't been seen enough times are forced into games to ensure coverage.

4. **Derive leave values**: `rack_list_write_to_klv()` iterates over all 888K racks. For each rack R with equity E(R):
   - Enumerate all proper subsets L of R (the "leaves")
   - For each leave, compute draw probability weight: `C = Π choose(dist[ml], drawn[ml])`
   - Accumulate: `leave_list[L].equity_sum += E(R) × C`
   - Accumulate: `leave_list[L].count_sum += C`
   - Final: `klv[L] = equity_sum / count_sum - average_equity`

5. **Save KLV file**: The derived leave values are written. **The rack equities are discarded.**

### Key Observation

The rack equities E(R) are the fundamental quantity. Leave values are a derived summary that bakes in the full-bag assumption. By saving rack equities alongside leave values, we can re-derive leave values for any bag composition using the exact same algorithm.

## Proposed Design

### Data: Rack Equity File (.req)

A new file format storing the equity of every valid 7-tile rack:

```
Header (16 bytes):
  magic: "REQ1" (4 bytes)
  num_racks: uint32_t (888030 for English)
  rack_size: uint32_t (7)
  reserved: uint32_t (0)

KWG (rack index structure):
  kwg_size: uint32_t
  kwg_nodes: uint32_t[kwg_size]

Rack equities:
  equities: float[num_racks]   (same format as KLV leave values)
```

**File size**: ~3.6 MB (888K × 4 bytes for equities, plus KWG index). The KWG is the same one already built during leavegen — it indexes all valid 7-tile racks as "words" in a DAWG.

**Memory at runtime**: ~3.6 MB. Fits comfortably in L2 cache. The equities are a flat array indexed by the KWG word index, exactly like KLV leave values.

### Training Changes

Minimal. After `rack_list_write_to_klv()` writes the KLV, also write the rack equity file:

```c
void rack_list_write_req(const RackList *rack_list) {
    for (int i = 0; i < rack_list->number_of_racks; i++) {
        req->equities[i] = rack_list->racks_ordered_by_index[i]->mean;
    }
    // Write to .req file using the same KWG already built for the KLV
}
```

This is a one-line addition to the training pipeline. The rack equities are already computed — we just stop throwing them away.

### Dynamic Leave Computation

#### The Formula

For a leave L of k tiles and a bag B, the dynamic leave value is:

```
DLV(L, B) = weighted_mean(L, B) - baseline(B)
```

where:

```
weighted_mean(L, B) = [Σ_D W(D, B) × E(L ∪ D)] / [Σ_D W(D, B)]

W(D, B) = Π_ml choose(B[ml], D[ml])     (ways to draw multiset D from bag B)

baseline(B) = [Σ_R W(R, B) × E(R)] / [Σ_R W(R, B)]
              (average rack equity over all racks drawable from B)
```

D ranges over all (7-k)-tile multisets drawable from B. The baseline ensures leave values are relative adjustments (positive = better than average rack, negative = worse).

### Integration: Full KLV Recomputation (Recommended)

#### The Core Algorithm

Recompute the entire KLV once per root position. This is structurally identical to `rack_list_write_to_klv()` — the same iteration over racks, the same `generate_leaves` recursion — just with the depleted bag distribution:

```c
void recompute_klv_for_bag(const REQ *req, const int *bag_distribution,
                           KLV *dynamic_klv) {
    // This is rack_list_write_to_klv() with bag_distribution
    // replacing ld's full distribution.

    // 1. Compute baseline: weighted average equity across all drawable racks
    double weighted_sum = 0;
    uint64_t total_weight = 0;
    for (int i = 0; i < req->num_racks; i++) {
        Rack rack;
        rack_decode(&req->encoded_racks[i], &rack);
        uint64_t w = get_total_combos(&rack, bag_distribution);
        if (w == 0) continue;  // Not drawable from this bag
        weighted_sum += req->equities[i] * (double)w;
        total_weight += w;
    }
    double baseline = weighted_sum / (double)total_weight;

    // 2. Distribute rack equities to sub-leaves (same as generate_leaves)
    RackListLetterDistribution rl_ld;
    init_rl_ld_from_distribution(bag_distribution, &rl_ld);
    RackListLeave *leave_list = calloc(klv_num_leaves, sizeof(RackListLeave));

    for (int i = 0; i < req->num_racks; i++) {
        Rack rack;
        rack_decode(&req->encoded_racks[i], &rack);
        if (!rack_is_drawable(&rack, &rl_ld)) continue;

        // generate_leaves distributes this rack's equity to all its sub-leaves,
        // weighted by draw probability. Reused verbatim from rack_list.c.
        Rack leave;
        rack_reset(&leave);
        generate_leaves(leave_list, dynamic_klv, req->equities[i],
                       &rack, &rl_ld, &leave,
                       kwg_get_dawg_root_node_index(dynamic_klv->kwg), 0, 0);
    }

    // 3. Normalize and subtract baseline (same as rack_list_write_to_klv)
    for (uint32_t i = 0; i < klv_get_number_of_leaves(dynamic_klv); i++) {
        if (leave_list[i].count_sum > 0) {
            dynamic_klv->leave_values[i] = double_to_equity(
                (leave_list[i].equity_sum / (double)leave_list[i].count_sum)
                - baseline);
        } else {
            dynamic_klv->leave_values[i] = 0;
        }
    }
    free(leave_list);
}
```

The `generate_leaves` function is reused **verbatim** from rack_list.c. It already handles distribution-weighted leave derivation and KWG index tracking. The only difference is that `rl_ld` is initialized from the current bag instead of the full distribution.

#### Cost and Amortization

The outer loop iterates over 888K rack equities. `get_total_combos` returns 0 immediately for any rack that requires a tile type with more copies than the bag holds (a single `choose(n, k)` returning 0 short-circuits the product). The actual work scales with the number of drawable racks:

| Tiles in bag | Drawable racks (approx) | Relative cost vs full bag |
|-------------|------------------------|--------------------------|
| 93 (near full) | ~888,000 (all)      | 100% |
| 70          | ~400,000               | ~50% |
| 50          | ~100,000               | ~15% |
| 30          | ~15,000                | ~2% |
| 15          | ~1,000                 | negligible |

The full-bag `rack_list_write_to_klv()` runs in well under a second on modern hardware (it executes once per leavegen generation, single-threaded, and is not a bottleneck). With a depleted bag, the dynamic version is dramatically faster.

For simulation with N games from a root position:
- **One-time cost**: `recompute_klv_for_bag()` — milliseconds to sub-second
- **Per-game cost**: zero additional — normal KLV/LeaveMap lookups at O(1)
- **Amortized cost per game**: (recomputation time) / N — negligible for N > 10

#### Simulation Flow

```
1. Set up root position (board state, bag, player rack)
2. Compute visible bag: B = full_bag - tiles_on_board - player_rack
3. Call recompute_klv_for_bag(req, B, &dynamic_klv)          [ONE TIME]
4. For each simulation game (i = 1..N):
   a. Clone root position
   b. Shuffle remaining tiles, deal opponent rack
   c. Play game to completion using dynamic_klv for leave values
   d. Record outcome
5. Aggregate simulation results
```

Step 3 produces a KLV that is a **drop-in replacement** for the static KLV. The `generate_exchange_moves` function that populates the LeaveMap reads from the KLV — it doesn't care whether the KLV was loaded from a file or recomputed at runtime. No changes to move generation or the LeaveMap are needed.

#### Mid-Simulation Recomputation

In a simulation, the bag changes as moves are played. The initial dynamic KLV computed at the root becomes stale as tiles are drawn. Two approaches:

1. **Recompute at key points**: Recompute the dynamic KLV after every N moves, or when the bag composition changes significantly from the root (e.g., a tile type is exhausted). Each recomputation is fast (especially late-game) and benefits all subsequent moves in all active simulations.

2. **Use root KLV throughout**: Accept that the KLV becomes slightly stale during each simulation game. This is still much better than the static KLV (which assumes the full bag), since the root bag is closer to the in-game bag than the full bag is. For short simulation horizons (~5-10 moves), the root KLV is a good approximation.

### Alternative: Top-N Reranking (For Single-Game Play)

For real-time single-game play (not simulation), full KLV recomputation may not be justified per turn. A lighter approach reranks only the top N candidate moves:

```
1. Generate moves normally using static KLV (existing pipeline, unchanged)
2. Collect top N moves (N = 20-100)
3. For each candidate move:
   a. Compute the leave (tiles kept after playing)
   b. Compute DLV(leave, current_bag) by enumerating draws
   c. Re-score: equity = score + DLV(leave, bag) + other_adjustments
4. Re-sort by updated equity
```

**Cost per turn**: N × (draw enumeration for one leave). For the common case (play 4, keep 3, draw 4): ~4,000 draws × 50 moves = 200K lookups ≈ 10-20ms.

This is a fallback for cases where full KLV recomputation isn't amortized. For simulation, the full recomputation is strictly better.

### Baseline Computation

The baseline `avg_equity(B)` is needed to make dynamic leave values relative. It represents the expected equity of a random rack drawn from bag B:

```
baseline(B) = [Σ_R W(R, B) × E(R)] / [Σ_R W(R, B)]
```

This is computed as part of the `recompute_klv_for_bag()` function (the first loop in the code above). For top-N reranking, it's computed once per turn since all candidate moves face the same bag.

### Per-Player vs Shared Baseline

The "bag" from the current player's perspective excludes the opponent's tiles. In a real game, the opponent's rack is unknown. Two approaches:

1. **Visible bag**: `B = full_bag - tiles_on_board - player_rack`. This is what the player knows. The opponent's tiles are part of B. This is correct for the player's decision-making perspective.

2. **True bag**: `B = full_bag - tiles_on_board - player_rack - opponent_rack`. This would require knowing the opponent's rack (only valid in analysis or simulation). In simulation where both racks are known, this is more precise.

## Training Methodology

### Phase 1: Train Rack Equities (No Change)

The existing leavegen pipeline already computes rack equities as an intermediate step. The only change is saving them:

1. Run leavegen as usual: generational self-play with forced rare racks
2. At each generation boundary, in addition to `rack_list_write_to_klv()`, also call `rack_list_write_req()` to save the rack equity table

The rack equities converge alongside leave values. After K generations, the final REQ file contains the converged equities.

### Phase 2: Validate Dynamic Leaves

Compare static vs dynamic leave values across game positions:

1. Play N games, recording each turn's rack and bag state
2. For each turn, compute both KLV(leave) and DLV(leave, bag)
3. Measure: how often does the dynamic leave change the top move selection?
4. Measure: does using dynamic leaves improve the correlation between predicted equity and actual game outcome (lower residual)?

### Phase 3: Simulation Comparison

Run simulation-based evaluation: from a set of test positions, simulate N games with static KLV and N games with dynamic KLV. Compare the move selection and outcome prediction accuracy.

### Phase 4: Retrain with Dynamic Leaves Active (Optional)

If dynamic leaves are used during autoplay games, the training games become more representative of games played with dynamic evaluation. This could improve both the rack equities and the static leave values derived from them. This is an outer loop around the existing leavegen iteration.

## Data Structures

```c
// Rack Equity Table — loaded from .req file, used to recompute KLV
typedef struct REQ {
    uint32_t num_racks;       // 888,030 for English
    KWG *kwg;                 // Same KWG structure as KLV, indexes all 7-tile racks
    uint32_t *word_counts;    // For KWG traversal (generate_leaves)
    Equity *equities;         // Rack equity for each indexed rack
    EncodedRack *encoded_racks; // Packed rack representations for fast decoding
} REQ;
```

The REQ shares its KWG with the KLV (same set of "words" — all valid 7-tile racks). At load time, the EncodedRacks can be reconstructed by walking the KWG, or stored in the file for faster startup.

## File Format

### .req File

```
REQ file (Rack EQuity):

  kwg_size: uint32_t (LE)        — number of KWG nodes
  kwg_nodes: uint32_t[kwg_size]  — KWG node data (LE)
  num_racks: uint32_t (LE)       — number of rack equities (888030)
  equities: float[num_racks]     — rack equities in KWG word index order (LE)
```

This is intentionally identical in structure to the KLV file format: a KWG followed by a count and an array of floats. The same load/save code can be reused. The difference is semantic: KLV values are leave equities (for partial racks 1-6 tiles), REQ values are rack equities (for full 7-tile racks only).

**File size**: KWG for 888K racks is roughly the same size as the KLV KWG (~2-4 MB for nodes). Equities: 888K × 4 = 3.5 MB. **Total: ~6-8 MB**.

## Implementation Phases

### Phase 1: Save Rack Equities (Minimal Change)

- After `rack_list_write_to_klv()`, also save rack equities to .req file
- Reuse KLV file format code (same KWG, same float array serialization)
- Add `req_load()` and `req_destroy()` functions (trivial: copy of `klv_load`/`klv_destroy`)
- **Effort**: ~1 day, ~100 lines of code

### Phase 2: Full KLV Recomputation

- Implement `recompute_klv_for_bag()` — refactor `rack_list_write_to_klv()` to accept an arbitrary bag distribution instead of only the full distribution
- This is primarily a refactor: extract the bag distribution parameter from the existing function, add a `req` parameter for rack equities (replacing the `rli->mean` lookups)
- The `generate_leaves` function is reused verbatim
- **Effort**: ~2 days, ~200 lines of code

### Phase 3: Simulation Integration

- Wire `recompute_klv_for_bag()` into the simulation pipeline
- Add REQ to game configuration (loaded alongside KLV at startup)
- At simulation root: compute visible bag, recompute KLV, pass dynamic KLV to game runners
- **Effort**: ~1-2 days, ~100 lines of code

### Phase 4: Validation

- Run simulation-based evaluation from test positions
- Compare move selection and equity prediction with static vs dynamic KLV
- Profile recomputation cost across different bag sizes
- **Effort**: ~2 days

## Expected Impact

### Where Dynamic Leaves Help Most

1. **Pre-endgame** (~7-14 tiles in bag): Static leaves are worst here. You can nearly predict your next draw. Dynamic leaves approach certainty about what you'll draw, making leave values nearly exact.

2. **Depleted tile types**: When a common tile (E, S, R) is exhausted, racks containing that tile become impossible to draw. Static leaves still value leaves as if those draws are possible. Dynamic leaves correctly assign zero weight to impossible completions.

3. **Simulation**: Every simulation game from a root position benefits from the recomputed KLV. The cumulative impact across hundreds of simulated games is significant even if the per-move difference is small.

4. **Opponent tracking**: In simulation where both racks are known, the true bag is precisely determined. Dynamic leaves exploit this complete information.

### Where Dynamic Leaves Don't Help

1. **Opening/early game**: The bag is barely depleted. DLV ≈ KLV. The recomputation produces essentially the same values.

2. **Endgame**: The bag is empty. Leave values are irrelevant — exact endgame search takes over.

## Relationship to SPV (Positional Heuristic)

Dynamic leaves and SPV are orthogonal improvements to move evaluation:

- **Static KLV**: What tiles you keep → expected value (assuming average bag)
- **Dynamic leaves**: What tiles you keep → expected value (given actual bag)
- **SPV**: Where you place tiles → expected positional cost/variance

They compose naturally in the equity formula:

```
equity = score + dynamic_leave_value + spv_adjustment
```

Training order:
1. Train rack equities and static KLV (existing pipeline)
2. Train SPV from residuals of (score + static KLV)
3. Save rack equities as .req file
4. At simulation time: recompute KLV from REQ + bag, use dynamic KLV + SPV for all games

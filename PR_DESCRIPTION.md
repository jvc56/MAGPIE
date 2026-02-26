## Summary

Three targeted optimizations to the bag-empty endgame search, each
identified or confirmed via `sample` profiling of the timed round-robin
benchmark (30 stuck-tile positions, `BUILD=profile`, 600s at 1ms
intervals). Taken together they produce a **~15.5% wall-clock speedup**
on stuck-tile endgame solves.

---

### 1. SmallMove-native `compute_conservation_bonus` and `compute_played_tiles_face_value`

Both functions previously required the caller to expand a `SmallMove` into
a full `Move` via `small_move_to_move`, which walks the board to reconstruct
the tile array. Both functions only care about rack-played tiles and
immediately skip `PLAYED_THROUGH_MARKER` entries anyway.

The rack-played tiles are already encoded directly in `tiny_move` bits
20–61 (7 tiles × 6 bits each) with blank flags at bits 12–18 and the
count in `metadata.tiles_played`. The new implementations read those
fields directly with no board scan and no `Move` construction.

Call-site impact:
- `assign_estimates_and_sort`: two `small_move_to_move` calls removed.
- `negamax_greedy_leaf_playout`: one `small_move_to_move` removed from the
  per-candidate conservation-bonus loop.

### 2. Lazy `mv_a` expansion in `compute_build_chain_values`

The inner loop checks whether any already-processed move B geometrically
contains move A. When containment passes, a tile-by-tile match requires
expanding both SmallMoves to full Moves. Previously,
`small_move_to_move(&mv_a, ...)` was called inside the inner loop on every
containment hit — re-expanding the same `sm_a` repeatedly.

`mv_a` is now expanded lazily: declared before the inner loop, computed at
most once on the first containment hit, and skipped entirely when no
candidate ever passes containment (the common case).

The lazy `mv_a` hoist reduced `assign_estimates_and_sort` self-time from
~1.2% to ~0.45% of total endgame worker samples (~44% reduction in that
function).

### 3. Single-tile movegen fast path + analytical leaf evaluation

Profiling showed that 58.2% of all movegen calls were for 1-tile racks,
consuming 45.6% of total movegen time. These calls were running the full
GADDAG machinery unnecessarily — in a bag-empty endgame, a player with one
tile either places it (the only non-pass move) or is stuck. Two
complementary shortcuts eliminate this overhead entirely.

**`generate_single_tile_plays` (in `generate_stm_plays`):**
When the stm rack has exactly one tile, bypass the GADDAG and scan all 225
squares directly. For each non-empty candidate square, check the combined
horizontal/vertical cross-set, score via bonus-square multipliers and
neighbor cross-scores, and track the best. Returns the single best
SmallMove in O(225) time with no KWG traversal.

**Analytical leaf evaluation (in `abdada_negamax`):**
When `nplays == 1`, the move is a non-pass, and the rack has 1 tile, the
placement empties the rack and ends the game immediately. Instead of the
full board-mutation cycle (small_move_to_move → play_move_endgame_outplay
→ child abdada_negamax → unplay), compute directly:

```
leaf_value = on_turn_spread + move_score
           + equity_to_int(calculate_end_rack_points(opp_rack, ld))
```

Then update PV/TT and return — no board state touched.

Both shortcuts include `#ifdef ENDGAME_SINGLE_TILE_VERIFY` blocks that
run the full legacy paths in parallel and assert identical results.

---

## Performance

### `sample` profiling — roundrobin benchmark

`BUILD=profile`, macOS `sample` (600s, 1ms intervals), 30 stuck-tile
positions, P1=20s P2=12s budgets, precheck+baseline 80% time management.
Metric: self-time samples / total endgame worker samples.

#### Optimizations 1+2: assign_estimates_and_sort reduction

| Version | `assign_estimates_and_sort` | minus build-chain callsite |
|---|---|---|
| Baseline | 1.200% | 0.809% |
| + SmallMove-native bonus/face-value | 1.517%* | 0.933%* |
| + lazy mv_a hoist | **0.451%** | **0.451%** |

\* Run 2 hit more stuck-tile positions; conservation bonus itself is within
noise. SmallMove-native change is a correctness/clarity win rather than a
measurable speed win on its own.

#### Optimization 3: single-tile fast path

Before and after adding `generate_single_tile_plays` + analytical leaf
evaluation (both commits applied together):

| Function | Baseline % | Optimized % | Δ (pp) |
|---|---:|---:|---:|
| `abdada_negamax` | 16.2% | 3.4% | **−12.8** |
| `generate_moves` | 6.8% | 0.4% | **−6.4** |
| `assign_estimates_and_sort` | 5.7% | 4.5% | −1.3 |
| `game_gen_cross_set` | 5.2% | 6.1% | +0.9 |
| `generate_stm_plays` | 0.1% | 3.3% | +3.1 |
| `recursive_gen_small` | 29.3% | 33.2% | +3.8 |

`generate_single_tile_plays` is inlined into `generate_stm_plays` by the
compiler. The `recursive_gen_small` share rises not because it got slower
but because the ~19 pp of overhead it used to share the budget with has
been eliminated. `abdada_negamax` self-time drops sharply because the
per-leaf play/unplay cycle (previously visible as `abdada_negamax`
overhead) is gone for single-tile outplay leaves.

### Wall-clock benchmark — eldar_v 5-ply stuck-tile endgame

CSW21, P1 rack AEEIRUW / P2 rack V (100% stuck). `BUILD=release`
(`-O3 -flto -march=native`), 9 threads, 8 GB TT.

| Depth | Baseline | Optimized | Speedup |
|-------|----------|-----------|---------|
| 1 | 0.706s | 0.505s | 1.40× |
| 2 | 1.290s | 1.027s | 1.26× |
| 3 | 9.540s | 8.084s | 1.18× |
| 4 | 58.931s | 49.878s | 1.18× |
| **5 (total)** | **251.2s** | **217.5s** | **1.155×** |

**Net speedup: ~15.5% faster** end-to-end on this stuck-tile position. The
effect is strongest at shallow depths (1.40× at depth 1) where single-tile
leaf nodes dominate, and dilutes at depth 5 as the broader search tree
overshadows them. The profile data is consistent with these numbers: the
~19 pp of self-time eliminated from `abdada_negamax` and `generate_moves`
maps to roughly a 19/(100−19) ≈ 23% reduction in active CPU cycles, with
the remaining gap accounted for by parallelism overhead and TT effects.

---

## Files changed

- **`src/impl/endgame.c`**:
  - `compute_played_tiles_face_value`: takes `const SmallMove *`; reads
    tile values and blank flags directly from `tiny_move`.
  - `compute_conservation_bonus`: same signature change.
  - `assign_estimates_and_sort`: removed `est_board`, two
    `small_move_to_move` calls, and the `opp_stuck_frac <= 0` guard.
  - `negamax_greedy_leaf_playout`: removed one `small_move_to_move` from
    per-candidate conservation-bonus loop.
  - `compute_build_chain_values`: lazy `mv_a` expansion.
  - `generate_single_tile_plays`: new function; cross-set scan for 1-tile
    racks, bypasses GADDAG entirely.
  - `generate_stm_plays`: fast path to `generate_single_tile_plays` when
    `stm_rack->number_of_letters == 1`.
  - `abdada_negamax`: analytical leaf shortcut for single-tile outplay
    nodes.

## Test plan

- [x] `make clean && make BUILD=dev EXTRA_CFLAGS="-DENDGAME_SINGLE_TILE_VERIFY" magpie_test && ./bin/magpie_test endgame`
- [x] `./bin/magpie_test eldar_v`

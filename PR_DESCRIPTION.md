## Summary

Speed up `compute_opp_stuck_fraction` by using a cross-set pre-check before falling back to full move generation. A single board scan intersects horizontal and vertical cross-sets to build a bitvector of all machine letters that have a valid single-tile play. If every opponent rack tile appears in that bitvector, the stuck fraction is 0 and movegen is skipped entirely. For 1-tile racks the check is authoritative in both directions (no multi-tile words possible), so movegen is skipped regardless of the result.

### How the board scan works

`board_get_playable_tiles_bv` iterates over empty squares adjacent to existing tiles. At each square it computes `h & v` (the intersection of horizontal and vertical cross-sets) and accumulates into a `playable` bitvector. It exits early once `(playable & rack_tiles_bv) == rack_tiles_bv` — all rack tiles found playable.

Blank is excluded from the rack mask passed to the board scan. The caller checks blank playability separately: if any non-blank bit is set in the returned bitvector (`playable_bv >> 1`), the blank is playable.

### Cross-set validity guard

The endgame solver uses lazy cross-set updates — after a move is played, cross-sets may be stale until the next movegen call triggers an update. The pre-check is gated on `board_get_cross_sets_valid(board)`. When cross-sets are stale, the pre-check is skipped and the function falls through to full movegen, which handles stale cross-sets via its own lazy update path.

## Performance

Profiled with `BUILD=profile` (`-O3 -g`), `sample` tool (30s, 1ms intervals). Metric: fraction of total thread samples in `compute_opp_stuck_fraction`.

| Benchmark | Baseline (main) | With pre-check |
|---|---|---|
| benchfp (100% stuck positions) | 7.7% | 0.8% |
| benchns (non-stuck positions) | 6.3% | 0.4% |

## Zobrist bounds-checking fix

`zobrist_calculate_hash` and `zobrist_add_move` in `src/ent/zobrist.h` indexed
`z->scoreless_turns[scoreless_turns]` without bounds-checking. The array has
size 3 (indices 0–2), but `scoreless_turns` can exceed 2 in some endgame paths,
causing an out-of-bounds read. Both call sites now clamp the index:
`scoreless_turns > 2 ? 2 : scoreless_turns`.

## GCG multiple-description pragma support

`GCG_DESCRIPTION_TOKEN` in `src/impl/gcg.c` was previously treated as a
unique, must-precede-moves pragma (same group as `GCG_TITLE_TOKEN`,
`GCG_ID_TOKEN`). It is now moved to the `GCG_PLAYER_TOKEN` group — still
required to precede move events, but allowed to appear multiple times. When
multiple `#description` lines are present, the parse handler concatenates them
with `\n` rather than overwriting.

## Files changed

- **`src/ent/board.h`**: New `board_get_playable_tiles_bv` — single-pass board scan returning a bitvector of playable machine letters with early exit.
- **`src/ent/zobrist.h`**: Bounds-check on `scoreless_turns` index in `zobrist_calculate_hash` and `zobrist_add_move`.
- **`src/impl/endgame.c`**: Cross-set pre-check in `compute_opp_stuck_fraction` with validity guard. Falls through to movegen when cross-sets are stale or when multi-tile racks have tiles without single-tile plays.
- **`src/impl/gcg.c`**: Allow multiple `#description` pragmas; concatenate with newline. Move `GCG_DESCRIPTION_TOKEN` to the non-unique pre-move pragma group.
- **`test/board_test.c`**: Test for `board_get_playable_tiles_bv` — validates against movegen for every machine letter on two board positions.

## EBF time management calibration

The cross-set precheck produces irregular search trees: shallow depths are
heavily pruned, making them anomalously fast relative to deeper depths. This
caused the 2-ply EBF estimate (`sqrt(t[d]/t[d-2])`) to spike at early depths
and prematurely terminate search.

Three fixes applied to `iterative_deepening` in `endgame.c`:

1. **`min_depth_for_time_mgmt` raised from 3 → 4**: the d1/d3 EBF ratio
   (precheck prunes d1 extremely heavily) was the most volatile estimate.
   Starting decisions at d4 uses the d2/d4 ratio, which is far more stable.

2. **EMA smoothing on the per-ply branching factor** (`alpha=0.4`): blends the
   current 2-ply sample with the historical EMA, damping transient spikes from
   depth-to-depth tree irregularity.

3. **Mid-depth bail via per-depth deadline**: after each completed depth, a
   deadline of `1.5 × estimated_next_depth_time` is set. Worker threads check
   it every 4096 nodes (via a `noinline` helper to avoid growing the recursive
   stack frame). If the deadline fires mid-depth, `search_complete` is set and
   the depth is discarded — the last fully completed depth's result is used.
   This prevents a depth from overshooting its budget and starving subsequent
   turns in a multi-turn endgame. The 1.5× multiplier tolerates normal tree
   variance while still catching 2–3× overruns. The deadline is only set when
   `elapsed >= 75% of hard_time_limit`. Below that threshold the EBF estimate
   can be noisy and the solver has many depths ahead; bailing early would drop
   back to a shallower result unnecessarily. Above 75% a runaway depth
   directly starves subsequent turns, making bail appropriate.

## 200-game 3-way round robin (positions 0–199, 20s/12s budget, O3)

Three configs compared across 200 endgame positions (8 threads, CSW21,
P1=20s P2=12s):

- **O** — Old: no cross-set precheck, 80% hard time limit
- **B** — Precheck + baseline 80% hard limit (isolates precheck effect)
- **F** — Precheck + EBF + 75% mid-depth bail guard (this PR, full stack)

| Pairing | Net spread | Wins | Losses | Ties | Meaning |
|---------|-----------|------|--------|------|---------|
| **O-B** | -39 (avg -0.20) | 13 | 21 | 167 | precheck effect |
| **O-F** | **-81** (avg **-0.41**) | 13 | 30 | 158 | **combined effect** |
| **B-F** | -7 (avg -0.04) | 19 | 25 | 157 | EBF effect |

Total time over 200 games (P1 + P2 combined):

| Config | Total time | Overtime |
|--------|-----------|----------|
| **O** | 9332.7s | **2.85s** |
| **B** | 9284.2s | **2.61s** |
| **F** | **7656.4s** | **0.32s** |

Key findings:
- **Combined gain**: F beats old by 81 points (avg +0.41/game) over 200 games,
  a clear win for the full stack.
- **Precheck contributes**: B beats O by 39 points (avg +0.20/game) from the
  cross-set stuck check alone.
- **EBF contributes**: F beats B by 7 points (avg +0.04/game) from smarter time
  management; smaller but consistent.
- **Time efficiency**: F uses ~18% less total search time than O or B while
  achieving better decisions — the precheck prunes stuck checks and the EBF
  banks remaining budget for subsequent turns.
- **Overtime**: F incurs only 0.32s total overtime vs 2.85s (O) and 2.61s (B),
  demonstrating tighter budget adherence.

## 226-game 3-way round robin — "bullet" time control (non-stuck positions, 1s/0.4s budget, O3)

Same three configs on 226 non-stuck endgame positions (8 threads, CSW21,
P1=1.0s P2=0.4s). Tight "bullet" budgets stress time management on positions
where neither player has stuck tiles.

| Pairing | Net spread | Wins | Losses | Ties | Meaning |
|---------|-----------|------|--------|------|---------|
| **O-B** | -25 (avg -0.11) | 10 | 25 | 191 | precheck effect |
| **O-F** | **-105** (avg **-0.46**) | 10 | 32 | 184 | **combined effect** |
| **B-F** | -104 (avg -0.46) | 12 | 25 | 189 | EBF effect |

Cross-table ranked by W-L vs all opponents:

| Rank | Config | W | L | T | W-L | Spread | Avg/game | Overtime |
|------|--------|---|---|---|-----|--------|----------|----------|
| 1 | **F** | 57 | 22 | 373 | **+35** | **+209** | **+0.92** | 25.68s |
| 2 | B | 37 | 35 | 380 | +2 | -79 | -0.35 | 25.95s |
| 3 | O | 20 | 57 | 375 | -37 | -130 | -0.58 | 27.84s |

Key findings:
- **F dominates at bullet**: W-L of +35 vs B's +2 and O's -37, avg +0.92
  spread/game — a clear signal even at this time control.
- **EBF is the driver**: O-F and B-F are nearly identical (-105 vs -104),
  confirming the precheck has minimal effect on non-stuck positions. F's
  entire edge over B comes from EBF time management, not the precheck.
- **Precheck neutral here**: O-B shows only -25 (avg -0.11), consistent with
  the precheck having nothing to do when no tiles are stuck.
- **Overtime roughly equal**: all configs incur ~26-28s total overtime across
  226 games; F is actually the most budget-efficient (25.68s).

## Test plan

- [ ] `make clean && make BUILD=dev magpie_test && ./bin/magpie_test board` — board tests pass
- [ ] `./bin/magpie_test endgame` — all endgame tests pass (including 25-ply deterministic solve)
- [ ] `./bin/magpie_test eldar_v` — stuck-tile endgame expected score unchanged

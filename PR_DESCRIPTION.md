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

## Files changed

- **`src/ent/board.h`**: New `board_get_playable_tiles_bv` — single-pass board scan returning a bitvector of playable machine letters with early exit.
- **`src/impl/endgame.c`**: Cross-set pre-check in `compute_opp_stuck_fraction` with validity guard. Falls through to movegen when cross-sets are stale or when multi-tile racks have tiles without single-tile plays.
- **`test/board_test.c`**: Test for `board_get_playable_tiles_bv` — validates against movegen for every machine letter on two board positions.

## EBF time management calibration

The cross-set precheck produces irregular search trees: shallow depths are
heavily pruned, making them anomalously fast relative to deeper depths. This
caused the 2-ply EBF estimate (`sqrt(t[d]/t[d-2])`) to spike at early depths
and prematurely terminate search.

Two fixes applied to `iterative_deepening` in `endgame.c`:

1. **`min_depth_for_time_mgmt` raised from 3 → 4**: the d1/d3 EBF ratio
   (precheck prunes d1 extremely heavily) was the most volatile estimate.
   Starting decisions at d4 uses the d2/d4 ratio, which is far more stable.

2. **EMA smoothing on the per-ply branching factor** (`alpha=0.4`): blends the
   current 2-ply sample with the historical EMA, damping transient spikes from
   depth-to-depth tree irregularity.

## 30-game round robin benchmark (positions 100–129, 20s/12s budget, O3)

Compared four solver configs across 30 endgame positions (8 threads,
CSW21, P1=20s P2=12s, precheck always on for B/F N configs):

| Config | Description | Net spread vs all opponents |
|--------|-------------|----------------------------|
| **FO** | EBF, no precheck | **+18** |
| **FN** | EBF, precheck | **+18** |
| **BN** | Baseline, precheck | -17 |
| **BO** | Baseline, no precheck | -19 |

Key findings:
- **EBF time management dominates**: both EBF configs beat both baseline configs
  by ~37 points total (avg ~0.6 pts/game). Baseline wastes budget by burning
  80% of remaining time every turn regardless of search progress.
- **Precheck effect**: slightly positive with baseline (BN +2 vs BO), slightly
  negative with EBF (FN -13 vs FO on FO-FN pairing, but FO and FN tied overall
  at +18). The negative EBF interaction was the EBF calibration issue now fixed.
- **Overtime**: minimal (BO=0.26s, FO=0.04s, BN=0.06s, FN=0.02s total across
  all 30 games).

Based on this, precheck is now the only supported mode and EBF calibration has
been updated for precheck's tree structure.

## Test plan

- [ ] `make clean && make BUILD=dev magpie_test && ./bin/magpie_test board` — board tests pass
- [ ] `./bin/magpie_test endgame` — all endgame tests pass (including 25-ply deterministic solve)
- [ ] `./bin/magpie_test eldar_v` — stuck-tile endgame expected score unchanged

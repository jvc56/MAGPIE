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

## Test plan

- [ ] `make clean && make BUILD=dev magpie_test && ./bin/magpie_test board` — board tests pass
- [ ] `./bin/magpie_test endgame` — all endgame tests pass (including 25-ply deterministic solve)
- [ ] `./bin/magpie_test eldar_v` — stuck-tile endgame expected score unchanged

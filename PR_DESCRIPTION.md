## Summary
- Use the wordpruned KWG for cross set computation in endgame (previously only used for main word finding)
- Add `override_kwg` mechanism to Game struct to allow cross set generation to use pruned KWG

## Changes
- Add `override_kwg` field to Game struct
- Add `game_set_override_kwg()` setter function
- Add `get_kwg_for_cross_set()` helper that uses `override_kwg` when set
- Set `override_kwg` in `endgame_solver_create_worker()`

## Benchmark Results

| Depth | Games | Main Branch | Feature Branch | Speedup |
|-------|-------|-------------|----------------|---------|
| 3-ply | 100   | 67.46s (0.675s/game) | 65.93s (0.659s/game) | **2.3%** |
| 5-ply | 20    | 142.98s (7.15s/game) | 133.55s (6.68s/game) | **6.6%** |
| 10-ply | 3    | 229.73s (76.58s/game) | 210.82s (70.28s/game) | **8.2%** |

The improvement scales with search depth because deeper searches spend proportionally more time on cross set computation.

## Test plan
- [x] Benchmark shows performance improvement at multiple depths
- [x] Same PVs and values computed (correctness verified)
- [x] Code compiles without warnings

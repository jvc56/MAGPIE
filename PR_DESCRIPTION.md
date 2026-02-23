## Summary

When the opponent is 100% stuck (all rack tiles are stuck), their only legal move is pass. Previously, this forced pass consumed a full depth ply in the alpha-beta search, halving the effective search horizon. This PR plays forced passes without decrementing depth, effectively doubling search depth in stuck-tile positions at near-zero cost.

The search remains fully adversarial — after each real move, the opponent's stuckness is re-verified since a tile placement can un-stick them.

Also includes a voluntary pass penalty in the greedy playout conservation heuristic: `(own_rack + opp_rack) * opp_stuck_frac`.

## Changes

- **`src/impl/endgame.c`**: Forced-pass fast path in `abdada_negamax` — when movegen returns exactly one move and it's a pass, recurse at same depth. Gated by `forced_pass_bypass` flag.
- **`src/impl/endgame.h`**: Added `forced_pass_bypass` field to `EndgameArgs`.
- **`test/benchmark_endgame_test.c`**: A/B benchmark infrastructure for stuck and non-stuck positions, timed selfplay with per-turn tracking, CGP generators.
- **`test/endgame_test.c`**: Enable bypass in existing endgame tests.

## Benchmarks

### Fixed-ply (stuck positions, 100% opponent stuck)

| Benchmark | Positions | Result | Spread | Speed |
|---|---|---|---|---|
| 3-ply old vs 3-ply new | 50 | 12-0-38 (new wins) | +46 | 0.05x (deeper search) |
| 3-ply old vs 2-ply new | 500 | 13-3-484 | +66 | 1.81x faster |

At same nominal depth, new never regresses. At matched effective depth (~2 real moves each), 1.8x faster with +66 spread.

### Fixed-ply (non-stuck positions)

| Benchmark | Positions | Result | Spread | Speed |
|---|---|---|---|---|
| 3-ply old vs 3-ply new | 500 | 4-0-496 | +30 | 1.00x |

**Zero overhead on non-stuck positions.** Identical times, identical values in 496/500 cases.

### Timed selfplay (IDS with interrupt, 100% stuck)

| Benchmark | Games | Result | Spread |
|---|---|---|---|
| 30s/turn, all stuck | 1500 | 21-17-1462 | +57 |
| 150s/turn, hard stuck | 188 | 22-14-152 | +50 |

At 30s/turn, most stuck positions solve easily either way (wash). At 150s/turn on the hardest positions (those that couldn't solve within 30s), the deeper effective search produces measurably better play: new wins 22-14 with +50 spread.

## Test plan

- [ ] `make clean && make BUILD=dev` — compiles with sanitizers, no warnings
- [ ] `./bin/magpie_test endgame` — all existing endgame tests pass
- [ ] `./bin/magpie_test eldar_v` — eldar_v expected score unchanged

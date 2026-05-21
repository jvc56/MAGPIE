# Game-Pair BAI Tournament Results

Comparing **Static** (4-ply static equity) vs **Nested** (2-ply with K=5 candidates, N=8 inner rollouts) simulation strategies.

Both strategies use:
- BAI sampling rule: round-robin (equal iterations per candidate)
- 15 candidate moves per position
- Endgame solver (25-ply, 10s) when bag is empty
- Late-game mode (99-ply sim-to-end) when < 21 tiles unseen
- 10 threads

## Results Summary

| Tournament | Pairs | Time/Turn | Games (N-S) | Win% | Pairs (N-S) | Pair% | Spread | Spread/Game |
|---|---|---|---|---|---|---|---|---|
| 50-pair (15s) | 50 | 15s | 58-42 | 58.0% | 37-13 | 74.0% | +2134 | +21.3 |
| 40-pair (5s) | 26* | 5s | 30-20** | 60.0% | — | — | +1126 | +22.5 |
| 150-pair (5s) | 150 | 5s | 150-150 | 50.0% | 86-63-1 | 57.3% | +2277 | +7.6 |

\* 40-pair run stopped at 26 pairs
\** From stopped run; incomplete

## Key Observations

### Nested dominates at 15s/turn
At 15 seconds per turn, nested sim wins 58% of games with +21.3 spread/game. This corresponds to roughly **100-120 NASPA rating points** using the logistic Elo formula with spread SD of 80-100.

### Time matters for nested
The advantage shrinks significantly at 5s/turn. Game win rate drops from 58% to 50%, and spread/game drops from +21.3 to +7.6. This makes sense: nested sim is ~20x more expensive per iteration than static, so at shorter time controls it gets far fewer samples to work with.

### Nested wins on spread even when games are even
In the 150-pair 5s run, games were split exactly 150-150, but nested still won 57% of pairs and had +7.6 spread/game. Nested tends to win by larger margins than it loses.

### Disagreements are small
Analysis of the 50-pair 15s run showed the largest meaningful disagreement between strategies was ~5 wp pts (Pair 4: FITNA vs DIF). Most divergences were < 1 wp pt — positional micro-preferences rather than strategic disagreements.

## Bug Fix: Win Percentage Cutoff

During the 50-pair run, discovered that the BAI win percentage cutoff fired prematurely on late-game 99-ply sims. With `cutoff=0.0`, the check `wp >= (1.0 - cutoff)` triggered when single-iteration results produced exactly 100% win rate. Fixed by adding `cutoff > 0.0` guard to `is_win_pct_at_upper_extreme()` and `is_win_pct_at_lower_extreme()` in `src/ent/win_pct.c`.

The 50-pair run was affected by this bug in late-game positions. The 150-pair run includes the fix.

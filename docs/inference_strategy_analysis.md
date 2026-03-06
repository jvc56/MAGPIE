# Inference Strategy Analysis

## What We Tested

Does inferring the opponent's rack (via Bayesian leave analysis of their
previous move) improve Scrabble play when used to bias Monte Carlo
simulation tile draws?

## equity_margin Parameter

The `equity_margin` controls when inference runs. Inference is skipped
when the best move's equity exceeds the second-best by more than the
margin. Two settings were tested:

- **eq_margin=1 (INFER1):** Infer on almost every move. Only skip when
  one move is clearly dominant (>1 equity point gap).
- **eq_margin=20 (INFR20):** Infer only when moves are close (<20 equity
  point gap). Skip inference on clear-cut decisions.

## Results at 108 Seeds

### Inference vs No Inference

Both inference players outperform pure simulation in overall win rate:
- INFR20: 52.7% overall, +43 Elo vs STATIC
- INFER1: 52.4% overall, +41 Elo vs STATIC
- SIM2: 50.4% overall, +31 Elo vs STATIC

The inference edge over SIM2 is ~10 Elo — present but small.

### INFER1 vs INFR20

Head-to-head, INFR20 leads INFER1 112-104 (51.9%) but INFER1 leads in
spread (+0.3/g). The difference is not significant.

**INFER1 strengths:**
- Best spread overall (+1.4/g)
- Dominates STATIC the most (58.6%, +5.0/g)
- Sweeps STATIC most often (29/54 pairs = 54%)

**INFR20 strengths:**
- Best overall win rate (52.7%)
- More consistent (fewer extreme losses)
- Preserves more simulation budget for clear decisions

### The Budget Tradeoff

With a 2s turn budget and 1s inference limit:
- INFER1 runs inference on nearly every move, consuming 0.5-1.0s of its
  simulation budget. Sometimes falls back to equity-best when budget is
  exhausted.
- INFR20 skips inference on clear decisions, preserving full 2s for
  simulation ~60% of the time.
- SIM2 always has the full 2s for simulation.

Despite less simulation time, inference players match or beat SIM2. This
suggests the information from inference (even approximate) compensates for
reduced simulation depth.

## The STATIC Paradox

STATIC (equity-best, no simulation) has these paradoxical properties:

1. **Worst win rate** (44.5%) but **highest average score** (454.2 pts)
2. **Positive spread vs SIM2** (+3.3/g) despite losing the matchup 46.5%
3. **Smallest spread deficit** (-1.1/g) despite worst win rate

Explanation: STATIC always plays the highest-equity move regardless of
game state. This produces high raw scores but no strategic adaptation.
When tiles favor STATIC, it wins by huge margins (biggest blowout: +376).
When tiles are neutral, the strategic players' modest edges in move
selection are enough to win close games.

SIM2 beats STATIC in *games* but not in *points* because SIM2's strategic
advantage manifests as narrow wins, while STATIC's wins are blowouts.

## Non-Transitivity

The tournament results don't fit a clean linear rating scale:

| Matchup | Actual | BT Model | Diff |
|---|---|---|---|
| INFER1 vs STATIC | 58.6% | 55.9% | +2.7% |
| INFR20 vs STATIC | 54.4% | 56.1% | -1.7% |
| SIM2 vs STATIC | 53.5% | 54.4% | -0.9% |

INFER1 beats STATIC *more* than its overall rating predicts, while
INFR20 beats STATIC *less* than expected. This suggests matchup-specific
advantages:

- Inference with tight margin (INFER1) is specifically good against
  non-adaptive play (STATIC), where knowing the opponent's rack is
  most valuable since STATIC won't adjust.
- Inference with loose margin (INFR20) wastes inference against STATIC
  since the moves are often clear-cut anyway.

## Open Questions

1. **Would a longer budget help inference more?** With 4s or 8s per turn,
   inference's cost is proportionally smaller. The budget tradeoff might
   favor inference at longer time controls.

2. **Is eq_margin=10 better than 1 or 20?** We only tested two points.
   The optimal margin likely depends on time control.

3. **Does inference help more in close games?** The spread data hints
   that inference is more valuable when the game is competitive (vs STATIC)
   than when both players are strategic (vs SIM2).

4. **Why does INFR20 have better win rate but worse spread than INFER1?**
   One hypothesis: INFR20 makes safer moves on average (less inference
   overhead = more simulation = better move evaluation), winning narrow
   games, while INFER1's inference occasionally identifies spectacular
   plays that create large margins but sometimes wastes budget on moves
   that don't help.

5. **Is the stale sim_results issue biasing results?** When inference
   eats the budget and the fallback to equity-best triggers, that turn
   plays a STATIC-quality move. This affects INFER1 more than INFR20
   (since INFER1 infers more often), potentially masking inference's
   true strength.

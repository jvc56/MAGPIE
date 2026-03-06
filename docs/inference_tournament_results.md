# Inference Tournament Results

## Summary

We ran a series of round-robin tournaments to evaluate whether opponent rack
inference improves MAGPIE's Scrabble play compared to pure simulation and
static (equity-best) strategies.

**Bottom line:** Inference provides a small but consistent edge over pure
simulation (~10 Elo), and both significantly outperform static play (~30-40
Elo). However, the differences among the top three players are not statistically
significant at 108 seeds. All three non-STATIC players win ~50-53% against
each other.

## Tournament 4 Configuration (Final)

| Parameter | Value |
|---|---|
| Seeds completed | 108 of 350 |
| Games played | 1296 |
| Budget per turn | 2.0 seconds |
| Inference time limit | 1.0 seconds |
| Min play iterations | 50 |
| Simulation plies | 2 |
| Threads | 10 |
| Platform | M4 Mac Mini |

## Players

| Player | Strategy |
|---|---|
| STATIC | Equity-best move (no simulation, no inference) |
| SIM2 | 2-ply simulation, full 2s budget |
| INFER1 | 2-ply simulation + inference, equity_margin=1 (infer on nearly all moves) |
| INFR20 | 2-ply simulation + inference, equity_margin=20 (infer only when moves are close in equity) |

## Final Standings

### Elo Ratings (STATIC = 2000)

| Player | Elo | vs STATIC |
|---|---|---|
| INFR20 | 2043 | +43 |
| INFER1 | 2041 | +41 |
| SIM2 | 2031 | +31 |
| STATIC | 2000 | — |

### W-L Crosstable (sorted by win%)

```
              INFR20      INFER1        SIM2      STATIC  |         TOTAL
INFR20               112.0-104.0 112.0-104.0 117.5-98.5  | 341.5-306.5 52.7%
INFER1  104.0-112.0              109.0-107.0 126.5-89.5   | 339.5-308.5 52.4%
SIM2    104.0-112.0  107.0-109.0             115.5-100.5  | 326.5-321.5 50.4%
STATIC   98.5-117.5   89.5-126.5 100.5-115.5              | 288.5-359.5 44.5%
```

### Spread per Game (sorted by total spread)

```
              INFER1        SIM2      INFR20      STATIC  |        TOTAL
INFER1                       -1.1        +0.3        +5.0 |   +894 +1.4/g
SIM2           +1.1                      +2.4        -3.3 |    +52 +0.1/g
INFR20         -0.3          -2.4                    +1.5 |   -246 -0.4/g
STATIC         -5.0          +3.3        -1.5              |   -700 -1.1/g
```

## Key Findings

### 1. Inference Provides a Small Edge

Both inference players (INFER1, INFR20) outperform pure simulation (SIM2)
in win rate, though the margins are small (~2%). INFER1 leads in spread
(+1.4/g overall) while INFR20 leads in raw win count.

### 2. Win Rate and Spread Rankings Diverge

- **By wins:** INFR20 52.7% > INFER1 52.4% > SIM2 50.4% > STATIC 44.5%
- **By spread:** INFER1 +1.4/g >> SIM2 +0.1/g > INFR20 -0.4/g > STATIC -1.1/g

INFR20 wins more games but has negative spread. INFER1 wins fewer games
but wins by larger margins when it does win.

### 3. INFER1 Dominates STATIC

INFER1 beats STATIC 58.6% with +5.0 spread/game — the most lopsided matchup
in the tournament. It sweeps STATIC in 29 of 54 game pairs (54%). This
suggests inference is most valuable against non-adaptive opponents.

### 4. STATIC Scores the Most Points

Paradoxically, STATIC has the highest average score (454.2) despite the worst
win rate. Without strategic adaptation, STATIC plays maximum-equity moves
which score well in absolute terms but fail to exploit or defend against
opponents.

### 5. Non-Transitivity

The results don't fit a clean linear Elo scale. INFER1 beats STATIC 2.7%
more than the Bradley-Terry model predicts, while INFR20 beats STATIC 1.7%
less than expected. There are matchup-specific advantages that a single
rating number can't capture.

### 6. Statistical Significance

At 108 seeds, the differences among the top three players are NOT
statistically significant. The INFR20 vs SIM2 head-to-head is 112-104
(z ≈ 0.5, p > 0.30). Achieving p < 0.05 for a 2% win rate edge would
require ~3,500 games (~1,700 seeds).

### 7. Most Game Pairs Split

When two non-STATIC players face each other, ~85% of game pairs split
(each player wins one game). Sweeps are rare (5-10% of pairs), indicating
the players make the same move choices most of the time.

## Notable Games

- **Highest score:** 648 by STATIC (vs SIM2, seed 9083)
- **Biggest blowout:** STATIC 609 - INFR20 233 (+376, seed 9064)
- **Tied games:** 3 out of 1296 (0.23%)

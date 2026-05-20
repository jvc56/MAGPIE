# BAI Custom Utility — A/B Match Results

Two autoplay-game-pair head-to-head matches between

- **P1 = dollar-blend**: `-uwin1 1 -uspread1 2 -uspreadscale1 100`
  (the linear-regime optimum for cash stakes of `$1 base + $0.01/pt`;
  derivation in `PLAN_BAI_CUSTOM_UTILITY.md`)
- **P2 = pure win%**: `-uwin2 1 -uspread2 0` (current default behavior)

Shared settings, both runs:
`CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 15 -plies 2
-threads 10 -threshold none -scondition 99.9 -mi1 50 -mi2 50`. Both run on
this M4 mini.

## Run 1 — 75 pairs (150 games), `-tlim 1`, seed 9000

| | **P1 (dollar-blend)** | **P2 (pure win%)** |
|---|---:|---:|
| Wins | **87** (58.00%) | 62 (41.33%) |
| Losses | 62 (41.33%) | 87 (58.00%) |
| Ties | 1 (0.67%) | 1 (0.67%) |
| Total (W+½T) | **87.5 (58.33%)** | 62.5 (41.67%) |
| Avg score / game | **460.87** ± 59.81 | 451.82 ± 59.76 |
| Avg per-game spread | **+9.05** | −9.05 |
| MAGPIE "P1 better" conf | **97.5%** | — |

Statistical tests (one-sided, P1 > 50% on each metric):

| metric | mean P1 advantage | one-sided p | confidence |
|---|---:|---:|---:|
| Win-rate (W + ½T) | +8.33 pp | 0.021 | **97.9%** |
| Per-game spread | +9.05 pts | 0.08 – 0.13¹ | 87 – 92% |
| Per-game $ EV (your stakes) | **+$0.257** | ~0.035 | **~96.5%** |

¹ Range reflects assumed per-game spread SD ∈ {80, 85, 100}; autoplay output
gives only marginal player score SDs, not joint covariance.

**Dollar EV decomposition** (per game):
`E[$] = (P_win − P_loss)·$1 + $0.01·E[spread] = 0.167 + 0.091 = $0.257`.
Win-rate contributes ~2× the spread component at these stakes (1 win is
worth 200 spread points → win-rate dominates).

## Run 2 — 250 pairs (500 games), `-tlim 0.1`, seed 5116196624972831882

(Required upgrading `-tlim` to accept fractional seconds; that change lives
on the `bench-100ms-tlim` branch off this one, not in PR #537.)

| | **P1 (dollar-blend)** | **P2 (pure win%)** |
|---|---:|---:|
| Wins | 251 (50.20%) | 246 (49.20%) |
| Losses | 246 (49.20%) | 251 (50.20%) |
| Ties | 3 (0.60%) | 3 (0.60%) |
| Total (W+½T) | 252.5 (50.50%) | 247.5 (49.50%) |
| Avg score / game | 449.35 ± 64.05 | 450.00 ± 64.27 |
| Avg per-game spread | **−0.65** | +0.65 |
| MAGPIE "P1 better" conf | 57.1% | — |

Dollar EV per game ≈ `(0.502 − 0.492)·$1 + $0.01·(−0.65) = $0.010 − $0.0065
≈ +$0.003 / game`. Essentially zero.

## Interpretation — budget sensitivity

The dollar-blend's edge **collapses by ~10× when the per-turn budget goes
from 1 s to 100 ms**:

| budget | n pairs | P1 win-rate | spread/game | conf |
|---|---:|---:|---:|---:|
| 1 s | 75 | 58.3% | +9.05 | 97.5% |
| 100 ms | 250 | 50.5% | −0.65 | 57.1% |

Likely cause: at 100 ms with 10 threads, BAI has roughly one-tenth the
iterations per arm. Per-arm win% standard error scales as `√(1/N)`, so the
per-arm wpct uncertainty rises by ~√10 ≈ 3.2×. The spread term's marginal
value in BAI ranking is proportional to `1 / (4·spread_scale)` and is
swamped by per-arm wpct noise when N is small. At 1 s/turn BAI has resolved
arms cleanly enough that the spread term breaks ties; at 100 ms/turn it
doesn't.

This suggests the dollar-blend's value is **budget-dependent** — useful
at TV-game / annotation budgets (≥ 1 s), neutral at blitz budgets
(~100 ms). A future sweep across both `uspread` weight and per-turn budget
would map the boundary.

## Reproducing

Run 1 (1 s/turn, 75 pairs):
```
./bin/magpie autoplay games 75 -gp true \
  -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all \
  -numplays 15 -plies 2 -threads 10 -tlim 1 \
  -threshold none -scondition 99.9 -mi1 50 -mi2 50 -seed 9000 \
  -uwin1 1 -uspread1 2 -uspreadscale1 100 \
  -uwin2 1 -uspread2 0
```

Run 2 (100 ms/turn, 250 pairs — requires `bench-100ms-tlim` branch for
fractional `-tlim`):
```
./bin/magpie autoplay games 250 -gp true \
  -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all \
  -numplays 15 -plies 2 -threads 10 -tlim 0.1 \
  -threshold none -scondition 99.9 -mi1 50 -mi2 50 \
  -seed 5116196624972831882 \
  -uwin1 1 -uspread1 2 -uspreadscale1 100 \
  -uwin2 1 -uspread2 0
```

## Limitations

- **Single seed per run** — no within-run variance estimate for win-rate
  (the p-values come from the binomial null, treating each game as i.i.d.).
- **Per-game spread SD assumed** for the spread/dollar p-values; autoplay
  doesn't dump per-game CSVs by default. A `-dumpscores` knob would let us
  compute exact dollar SDs.
- **Only one `(uwin, uspread, uspreadscale)` point tested** — `(1, 2, 100)`.
  A sweep across `uspread ∈ {0.5, 1, 2, 4, 8}` at fixed scale would map
  the curve.
- **No within-bracket significance** for the budget comparison — Run 1 and
  Run 2 differ in pair count *and* seed *and* budget; we can't isolate
  budget as the sole variable from these two runs alone.

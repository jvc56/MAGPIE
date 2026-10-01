# Blocking/setup pilots — September 30, 2026

These are bounded pilots run to check that the pipeline and the studies work and to measure cost. **None of them supports a default.** Every effect below is either within noise or not significant at 5%. The raw data (≈30 MB) is kept outside the repository under `~/sources/bs-data/`; this directory holds the manifests, splits, summaries and per-game/per-pool rows.

Machine: Apple M4 (4P + 6E cores), 16 GB, 8 worker processes. Build: `make magpie_test BUILD=no_pgo_release`. Binary hashes are in each `MANIFEST.txt`. Code: branches `claude/blocking-setup-teacher` → `claude/sim-nomination` → `claude/blocking-setup-studies`.

## P1 — training data (CSW24, NWL23)

`generate.sh <lex> <dir> 360 <seed> 8 10000 <lex>`. Seeds: CSW24 20261001, NWL23 20261101.

Each run is 360 independent no-PAT static games with one position per game, 120 per phase (early: bag ≥ 60; middle: 30–59; late: 7–29). The universe is the top 60 static moves, up to 5 exchanges within 35 points, and the top 25 PAT moves under `<lex>.pat` (about 61 moves in all). The teacher ran at 16, 64 and 256 racks (nested). There are two independent 10 s, 4-ply round-robin references. The split is frozen by game hash; the test split is untouched.

| | CSW24 | NWL23 |
|---|---|---|
| train / validation / test | 210 / 78 / 72 | 211 / 68 / 81 |
| teacher time per position (16 / 64 / 256 racks) | 125 / 525 / 2,110 ms | 125 / 525 / 2,109 ms |
| blocking median Spearman vs 256 racks (16 / 64) | 0.835 / 0.949 | 0.827 / 0.945 |
| setup median Spearman vs 256 racks (16 / 64) | 0.629 / 0.855 | 0.624 / 0.861 |
| top-25 admissions replaced vs 256 racks, blocking (16 / 64) | 3.3 / 1.8 | 3.4 / 1.8 |
| `static_choice` fit: best train grid point (wb, ws) | (0.5, 0.25) | (0.75, 0.5) |
| …train mean (in-sample, optimistic) | +0.140 pp | +0.328 pp |
| …validation | +0.001 ± 0.056 pp, 13 changed | +0.005 ± 0.076 pp, 17 changed |
| `sim_admission` fit: validation | 0.000 (degenerate) | +0.024 ± 0.024 pp |

Under the admission objective, the play the reference ranks best is almost always already in the equal-count static pool. At this size the objective is therefore nearly flat; fitting it would need far more positions, or an objective defined through selection sims.

Volatility diagnostics are in `volatility-research_v0.json` (train + validation, research weights). They cover lead, bag, the pass-reply mean, PAT's pre-board term, and the spread and extremes of the candidates' deltas and PAT terms. No feature has a reliable relation to the gain. The late-bag quartile has the largest mean gain, consistent with the archived setup study, but nothing is significant. These remain hypotheses.

## P2 — pool comparison A/B/Bp/C (CSW24)

Command: `pools_study.sh CSW24 CSW24 research_v0.bsp positions.csv <dir> 20261202 8 5000 20000`.

- Positions: 96 fresh positions (games seed 20261201), never used for fitting.
- Weights: research 1.4 / 0.75.
- Arms, each 25 per source plus up to 5 exchanges:
  - A = static + blocking + setup
  - B = PAT + blocking + setup (checks ranked by static equity)
  - Bp = as B, but the checks rank by PAT equity
  - C = static + PAT + blocking + setup
- Every arm is compared against a static pool of exactly the same size.
- Sims: 5 s top-two selection, then an independent 20 s round-robin reference over the union of all pools. Rollouts are no-PAT static throughout.
- Positions where both choices agree are included.

| comparison | mean (pp) | SE | p | positions where the choice differs |
|---|---:|---:|---:|---:|
| B − A | +0.006 | 0.006 | 0.32 | 1 / 96 |
| C − A | 0.000 | 0 | — | 0 / 96 |
| Bp − B | −0.006 | 0.006 | 0.32 | 1 / 96 |
| A − static(\|A\|) | +0.003 | 0.006 | 0.55 | 3 / 96 |
| B − static(\|B\|) | +0.010 | 0.008 | 0.23 | 5 / 96 |

The B and A pools differ in 50 of 96 positions, but the chosen play differs in only one. This matches the untimed audit, which found that PAT adds about 0.8 nominees per position, and those nominees are rarely chosen. Pool sizes average 36–38. Nomination costs about 0.53 s per arm, nearly all of it the 64-rack checks.

## P3 — paired games, adjusted static vs static (CSW24)

Command: `games_study.sh`. Each pair is two games on one seed with seats swapped, so each seat draws the same tiles in both games. The "adjusted" player plays argmax(static + wb·blocking + ws·setup) over the top 60 static moves and 5 exchanges at 64 racks. It falls back to static play when the bag is empty.

| players | pairs | adjusted (or PAT) score | z / p | decisions changed | adjusted decision time p50 / p90 / p99 |
|---|---:|---:|---:|---:|---:|
| research 1.4 / 0.75 vs static | 200 | 53.6% ± 2.1% | 1.77 / 0.077 | 40% | 462 / 761 / 1,499 ms |
| P1 fit 0.5 / 0.25 vs static | 200 | 51.5% ± 2.1% | 0.73 / 0.47 | 21% | 456 / 721 / 1,297 ms |
| PAT vs static | 500 | 50.75% ± 1.3% | 0.59 / 0.56 | — | 2.5 / 15.6 / 73 ms |

The static player takes 0.18 ms (p50) per decision, so the adjusted evaluator costs about 1,000× static at 64 racks. These are equal-work comparisons against static play. An equal-time comparison would set the adjusted player against a sim of the same budget, which is not done here. The research-weight result was seen first in a pilot, so any confirmation must use fresh seeds and a prespecified test. The changed-decision counts by bag and lead stratum are in each `analysis.json`.

## Proposed next runs (not started; need a go-ahead)

1. **Static-ish confirmation (~50 min).** 2,000 fresh pairs (new seed) of research 1.4 / 0.75 vs static, plus 16-rack and late-only (bin) variants to price speed. The primary test is the prespecified two-sided pair-mean z.
2. **Track 1 data per lexicon (~40 min each).** 3,000 positions with labels at 64 racks only and choice-set references (`bs_fit.py choices`, then `refs cands=`) at 10 s, for CSW24 and NWL23, then fit `static_choice`.
3. **Decisive A/B/C pool comparison (~6–7 h).** This is the archived 15 s / 60 s design on about 2,000 fresh positions. The cost can be cut a lot: screen pools without sims (`sims=0`), run selection sims only for distinct pools, and run the reference only on the plays that some pool chooses. Even so, B and A pick the same play about 99% of the time, so the B − A estimate rests on roughly 20 positions per 2,000 unless pools are enriched by an outcome-blind rule.

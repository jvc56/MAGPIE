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

## Speed work on the teacher (after the pilots)

The 2,000-pair confirmation (P4, seed 20261401) was stopped at the user's request after 273 of 4,000 games so that the evaluator could be sped up first. Its partial output is marked incomplete and has not been analyzed.

All timings below are single-process on a quiet M4. The benchmark set is 48 fresh CSW24 positions (P2's), each with about 62 candidates (the top 60 placements plus up to 5 exchanges) and 64 racks. Commands are `bsbench` and `bsrace`.

| step | ms / position | results |
|---|---:|---|
| baseline (pilot code) | 311 | — |
| skip exchange generation in the teacher's searches | 251 | identical |
| cache pass-branch follow-ups per leave | 210 | identical |
| lane-restricted reply / follow-up reconstruction | 191 | identical |
| …seeded with the known play (`initial_best_move`) | 166 | identical |
| best 16 pass replies within 20 points | 161 | identical |
| exchanges reuse the pass branch; candidate played once per batch | 152 | identical |

"Identical" means byte-identical benchmark output. The archived replay (200 positions, 24,308 rows) still has 0 mismatches. The three new movegen arguments cost nothing when unused: over 5 interleaved 10,000-game static autoplay runs the mean time was 6.881 s both before and after, with identical results.

`MOVE_RECORD_BEST_SMALL` was tried as a faster score-only search and rejected. It was 4× slower here because it has no word-map path, and its follow-up scores differed by more than ties alone would explain.

**Best-move race** (`blocking_setup_checker_choose`; weights 1.4/0.75; 360 P1 CSW24 positions). The reference is the argmax of full measurement; with z = 0 the race asserts exactly that argmax.

| z | agrees | mean / max regret (pts) | ms / position | speedup vs exact | work |
|---|---:|---:|---:|---:|---:|
| 1.5 | 92.5% | 0.116 / 6.13 | 30.6 | 5.1× | 0.17 |
| 2.0 | 97.8% | 0.027 / 2.10 | 35.4 | 4.4× | 0.20 |
| 2.5 | 99.2% | 0.009 / 2.09 | 41.3 | 3.8× | 0.24 |
| 3.0 | 99.4% | 0.008 / 2.09 | 48.1 | 3.3× | 0.29 |

**In games** (the adjusted player in `bsstudy:games`, pilot seed, worker 0, 109 checked decisions, single process). The check took 274 ms at the median (mean 279 ms) with the pilot code. The exact version takes 126 ms (mean 132 ms) and its games are byte-identical to the pilot's. Racing gives 37 ms at z = 3 (mean 46 ms) and 28 ms at z = 2 (mean 33 ms).

Remaining options, none tried yet:
- 16 racks combined with the race, which would be about 4× less work but adds teacher noise.
- Worker threads for evaluate-all, since candidates are independent once the checker is loaded.
- Reconstructing our follow-ups when the reply changed, against a base-board list; I estimate this at about −10%.

## P5 — prespecified confirmation, race-based evaluator z = 3 (CSW24)

- **Design.** The protocol was frozen before launch (`p5-confirm-z3/PROTOCOL.json`): 2,000 fresh pairs (seed 20261501) of the adjusted player against no-PAT static, using the research weights 1.4 / 0.75 and 64 racks.
- **Adjusted player.** Its candidates are raced with `blocking_setup_checker_choose` at z = 3 in batches of 8 racks. This is not identical to the pilot's exact policy: on 360 positions it chose the full-evaluation move 99.4% of the time.
- **Primary test.** A two-sided z-test of the mean pair score against 0.5, at α = 0.05, with all pairs included.
- **Run.** 8 workers on the M4 took 7 min 21 s; at launch, background OS processes were using about one core.

| outcome | value |
|---|---|
| adjusted player's score (pair mean) | **52.24% ± 0.65%** (SE), 95% CI [50.97%, 53.51%] |
| z, two-sided p | 3.46, 0.0005 |
| wins / ties / losses | 2,079 / 21 / 1,900 |
| spread per game | +8.0 ± 1.15 points |
| decisions changed vs static | 39.3% of 40,612 checked decisions |
| check time per decision (8 workers busy) | median 71 ms, p90 143 ms, p99 263 ms, max 1.05 s |

**Result.** The primary test rejects at 5%. The pilot estimate (53.6% ± 2.1%) is consistent with this one.

**Limits.**
- This is an equal-work comparison against static play: the adjusted player spends about 70 ms per move and the static player under 1 ms.
- It does not show the adjusted player beats a sim of equal time.
- It covers CSW24 only.
- The weights are research choices, not fitted.
- The exact (z = 0) policy was not confirmed.

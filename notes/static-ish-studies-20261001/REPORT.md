# Static-ish play studies — night of 2026-09-30 / 10-01

All runs use CSW24 unless a different lexicon is named. Each is a prespecified paired-game test: 2,000 fresh pairs, the same tiles for each seat within a pair, and seats swapped between the two games. The primary test is a two-sided z test of player a's mean pair score against 50% (α = 0.05), and every pair is included. Protocols are frozen in each directory's `PROTOCOL.json`, which also records the binary hash and commit.

"Static-ish" means the `adjusted` player in `bsstudy:games`. It takes the top 60 no-PAT static placements plus up to 5 exchanges within 35 points, and plays the one with the highest static equity plus the weighted pass-relative blocking and setup deltas. The checks use 64 sampled racks and a race at z = 3, with the research weights 1.4 / 0.75 unless stated otherwise. The `.bsp` files used are in this directory.

These are equal-work comparisons unless noted. Timings are medians per checked decision with 8 worker processes busy on an M4.

## Teacher value and weights

Teacher value: P8 (in #746's notes).
- Equity mode beats score mode head to head: 51.76% ± 0.66%, p = 0.007.
- equity_score shows no difference from score mode: 49.61% ± 0.65%.

P9, equity-mode weight grid (800 pairs per point, all against equity 1.4 / 0.75, common seed 20262101). Scores are player a's mean pair score; the SE is about 1.0 pp at every point.

| blocking \ setup | 0.4 | 0.75 | 1.1 |
|---|---|---|---|
| 0.7 | 49.25 | 49.47 | 46.78 |
| 1.4 | 49.94 | (reference) | 47.97 |
| 2.1 | 46.84 | 47.09 | 46.88 |

- No point beats 1.4 / 0.75. Larger blocking (2.1) or larger setup (1.1) weights are 2–3 SE worse.
- **P10 was skipped.** The prespecified confirmation of the selected point (1.4, 0.4) did not run, because that point did not beat the reference even in tuning. This deviation is recorded in `p10-confirm-tuned/SKIPPED`.

## Equity mode against baselines

| run | player a | player b | a's score | 95% CI | p | spread | check (a / b) |
|---|---|---|---:|---|---:|---:|---|
| P13 | equity static-ish | no-PAT static | 51.42% ± 0.65% | [50.15, 52.70] | 0.028 | +5.4 | 91 / 0 ms |
| P11 (NWL23) | equity static-ish, CSW24 weights unrefit | no-PAT static | **52.25% ± 0.62%** | [51.03, 53.47] | 0.0003 | +6.3 | 92 / 0 ms |
| P14 | equity static-ish | PAT static (CSW24.pat) | 51.12% ± 0.65% | [49.86, 52.39] | 0.082 | +4.2 | 90 / 2 ms |
| P15 | equity static-ish | 2-ply sim, 15 candidates, 100 ms/move | **52.25% ± 0.66%** | [50.96, 53.54] | 0.0006 | +10.6 | mean 96 / 83 ms |

- **NWL23:** the CSW24-derived weights transfer without refitting.
- **Sim at about equal time (P15):** static-ish beats a 100 ms 2-ply sim. Static-ish averaged 96 ms per decision against the sim's 83 ms, which includes fast static endgame decisions. The sim's own strength against static is measured in P21 below.

## Cost reductions

| run | player a | player b | a's score | 95% CI | p | time per decision (mean, a / b) |
|---|---|---|---:|---|---:|---|
| P12 | equity, 96 racks | equity, 64 racks | 50.04% ± 0.64% | [48.78, 51.29] | 0.95 | median 105 / 94 ms |
| P16 | equity, checks only while bag < 60 | equity, ungated | 50.54% ± 0.64% | [49.28, 51.79] | 0.40 | 70 / 95 ms |

- **96 racks** gains nothing over 64.
- **Gating to bag < 60** shows no detectable loss and is 26% cheaper per decision.

## Sim iteration rates and rollout cost

These were measured on a quiet machine, single thread: 4-ply sims of 15 candidates on 6 positions. Raw data is in `bench-sim/`.

| rollout policy | iterations / s (geometric mean) | vs no-PAT |
|---|---:|---:|
| no-PAT static | 10,457 | 1 |
| PAT static | 6,911 | 0.66× |
| static-ish, score mode, 16 racks | 10.1 | 1/1,000 |
| static-ish, score mode, 64 racks | 6.1 | 1/1,700 |
| static-ish, equity mode, 64 racks | 4.2 | 1/2,500 |

The rollout wiring costs nothing when unused: 120,000 fixed iterations took 11.97–12.19 s on the base binary and 11.97–12.30 s on the new one.

## Head-to-head cost and mode tests (P17–P20)

| run | player a | player b | a's score | 95% CI | p | mean time per decision (a / b) |
|---|---|---|---:|---|---:|---|
| P17 | equity, race z = 3 | equity, exact (z = 0) | 49.10% ± 0.64% | [47.84, 50.36] | 0.16 | 94 / 317 ms |
| P18 | equity, universe 30 | equity, universe 60 | 49.66% ± 0.64% | [48.40, 50.92] | 0.60 | 64 / 95 ms |
| P19 (NWL23) | equity teacher | score teacher | 49.26% ± 0.64% | [48.01, 50.52] | 0.25 | median 88 / 68 ms |
| P20 | score, 96 racks | score, 64 racks | 50.82% ± 0.65% | [49.56, 52.09] | 0.20 | median 81 / 70 ms |

- **Racing (P17)** is not detectably worse than exact evaluation and takes 3.4× less time. The interval still allows a loss of up to 1.6 pp.
- **Universe 30 (P18)** is not detectably worse than 60 and is 33% cheaper.
- **Equity vs score teacher does not replicate on NWL23 (P19).** CSW24 (P8) gave +1.76 ± 0.66 pp for equity; NWL23 gives −0.74 ± 0.64 pp. The two estimates differ by about 2.7 SE. Whether the gain is lexicon-dependent, or the CSW24 result was partly luck, is unresolved. The inverse-variance pooled estimate is +0.49 ± 0.46 pp, not significant.
- **Racks (P20).** 96 vs 64 racks in score mode is not significant. Together with P12 (equity mode, 50.04%), extra racks beyond 64 show no reliable gain.

## Sim baseline, late-only gating, fewer racks (P21–P23)

| run | player a | player b | a's score | 95% CI | p | mean time per decision (a / b) |
|---|---|---|---:|---|---:|---|
| P21 | 2-ply sim, 100 ms | no-PAT static | 50.70% ± 0.66% | [49.40, 52.00] | 0.29 | 100 ms median / 0 ms |
| P22 | equity, checks only while bag < 30 | equity, ungated | 49.73% ± 0.65% | [48.45, 51.00] | 0.67 | **37 / 93 ms** |
| P23 | equity, 32 racks | equity, 64 racks | **47.81% ± 0.66%** | [46.51, 49.11] | **0.001** | 74 / 92 ms |

- **P21.** The 100 ms sim is not detectably better than static (spread −5.3). Static-ish's win over it in P15 therefore fits static-ish beating static.
- **P22.** Checking only late in the game (bag < 30) shows no detectable loss and cuts mean decision time by 60%; the interval allows a loss of up to 1.5 pp.
- **P23.** 32 racks is significantly worse than 64 and saves only 20%. Combined with P12 and P20, 64 racks is the floor and more racks show no gain.

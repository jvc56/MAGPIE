# Static-ish play studies — night of 2026-09-30 / 10-01

## Summary

Each result is a prespecified paired-game test with 2,000 pairs (≈ ±0.65 pp SE) unless noted. "Static-ish" means the top static placements and exchanges, re-ranked by static equity plus 1.4 × blocking + 0.75 × setup, with 64 racks and a race at z = 3.

- **Against static** it wins in every lexicon tried, with CSW24-derived weights and no refitting:

  | lexicon | mode | score vs static | p |
  |---|---|---:|---:|
  | CSW24 | score | 52.24% (P5) | 0.0005 |
  | CSW24 | equity | 51.42% | 0.028 |
  | NWL23 | score | 53.93% | <0.0001 |
  | NWL23 | equity | 52.25% | 0.0003 |
  | FRA20 | equity | 52.69% | <0.0001 |

- **Against short sims** it wins at about equal time and with less time:
  - vs a 100 ms 2-ply sim: 52.25%. That sim is no better than static: 50.70%.
  - vs a 300 ms 2-ply sim, about 2.6× static-ish's time: 52.38%.
- **Cheap configuration.** Checks only while the bag is below 30 and a universe of 30 cut the mean decision time from about 83 ms to 18 ms. Against static it scores 52.41% (P30), and 52.38% on NWL23 (P35). Head-to-head, the full configuration may be about 1 pp better (P33, 48.91%, p = 0.09).
- **Against PAT static.** The full configuration is about +1 pp better (P14 51.12%, P36 51.04%, pooled p ≈ 0.02); the cheap one only ties it (P34).
- **Rack count.** 64 racks is the floor: 32 is significantly worse (47.81% head to head), and 96 or 128 show no gain.
- **Racing.** z = 3 is not detectably worse than exact evaluation (49.10%, p = 0.16) and is 3.4× cheaper.
- **Teacher value.** Equity mode is a small, probably real gain over score mode: +0.87 ± 0.29 pp pooled, but heterogeneous across lexica and about 1.3× the time. The default remains score mode.
- **Weights.** No point on a 3×3 grid beat 1.4 / 0.75 in equity mode.
- **Nomination.** Nominated root candidates do not help a 300 ms sim at equal time (49.58%, 1,000 pairs).
- **Rollouts.** Static-ish rollouts run about 1,700× fewer sim iterations per second than static rollouts. They were benchmarked but not tested in played games.

All of these compare equal work against static play unless stated. The sim comparisons are short-budget 2-ply sims only.

## Setup

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

## Nominated sim candidates in played games (P24)

Player a was a PlayChooser 2-ply sim, 300 ms per move. Its root candidates came from the nominator: static 10, blocking 5, setup 5, exchanges 3, a universe of 30, with the equity teacher and exact checks. Player b was the same sim over the default top 15 static candidates, given 460 ms per move to cover a's roughly 160 ms of nomination. The run was 1,000 pairs, seed 20263601.

| a's score | 95% CI | p | decision time, median (a / b) |
|---:|---|---:|---|
| 49.58% ± 0.94% | [47.73, 51.42] | 0.65 | 482 / 460 ms |

At this short budget, nominating candidates by blocking and setup does not help the sim at equal time. The test can rule out gains above about 1.5 pp. Larger budgets, where the earlier position-level studies saw small gains, were not tested.

## French transfer and a longer sim (P25–P26)

| run | player a | player b | a's score | 95% CI | p | spread | mean time per decision (a / b) |
|---|---|---|---:|---|---:|---:|---|
| P25 (FRA20) | equity static-ish, CSW24 weights unrefit | no-PAT static | **52.69% ± 0.66%** | [51.39, 53.98] | <0.0001 | +7.2 | median 89 / 0 ms |
| P26 | equity static-ish | 2-ply sim, 15 candidates, 300 ms/move | **52.38% ± 0.65%** | [51.10, 53.65] | 0.0003 | +11.5 | 95 / 247 ms |

- **French (P25).** The weights transfer to French, which has a different letter distribution, without refitting.
- **Longer sim (P26).** Static-ish beats a 2-ply sim that uses 2.6× its time per decision.

## Replication and cheap configuration (P27–P29)

| run | player a | player b | a's score | 95% CI | p | time per decision (a / b) |
|---|---|---|---:|---|---:|---|
| P27 | CSW24 equity teacher (replication of P8) | score teacher | 50.91% ± 0.64% | [49.66, 52.16] | 0.15 | median 91 / 71 ms |
| P28 | score, gated bag < 60, universe 30 | score, ungated, universe 60 | 50.31% ± 0.65% | [49.03, 51.59] | 0.63 | **mean 36 / 72 ms** |
| P29 (FRA20) | equity teacher | score teacher | 51.28% ± 0.64% | [50.02, 52.53] | 0.047 | median 90 / 65 ms |

**Stacked cost cuts (P28).** Gating to bag < 60 and shrinking the universe to 30 together halve the decision time with no detectable loss.

**Equity vs score, all head-to-heads.** Inverse-variance pooled over P8, P27, P19 and P29, equity mode is **+0.79 ± 0.32 pp** better (p = 0.015). The results are heterogeneous: Q = 8.5 on 3 df, p = 0.04, with NWL23 the outlier. Excluding P8, the result that prompted the follow-ups, the pooled gain is **+0.48 ± 0.37 pp (p = 0.19)**.

At most, equity mode is a small improvement, and it is not established. It costs about 1.3× the time. The default stays score mode. A fresh-seed NWL23 replication (P32) is below.

## Cheap configuration and NWL23 (P30–P32)

| run | player a | player b | a's score | 95% CI | p | time per decision (a) |
|---|---|---|---:|---|---:|---|
| P30 | score, checks only while bag < 30, universe 30 | no-PAT static | **52.41% ± 0.46%** | [51.52, 53.31] | <0.0001 | **mean 18 ms** |
| P31 (NWL23) | score static-ish, CSW24 weights | no-PAT static | **53.93% ± 0.66%** | [52.63, 55.22] | <0.0001 | median 66 ms |
| P32 (NWL23) | equity teacher (replication of P19) | score teacher | 51.21% ± 0.64% | [49.96, 52.46] | 0.057 | median 87 / 67 ms |

- **Cheap configuration (P30).** It does as well against static as the full configuration (P5, 52.24%) at about a fifth of the time. Its SE is smaller because the two games of a pair diverge less when the checks start late; the pair-level SE accounts for this.
- **Equity vs score, all five head-to-heads (P8, P27, P19, P29, P32).** The inverse-variance pooled gain for equity is **+0.87 ± 0.29 pp** (p = 0.002), or +0.67 ± 0.32 pp (p = 0.04) excluding P8. By lexicon: CSW24 +1.32 ± 0.46, NWL23 +0.24 ± 0.45, FRA20 +1.28 ± 0.64. Equity mode is a small, probably real gain at about 1.3× the time.

## Cheap configuration head-to-heads, PAT baseline (P33–P36)

| run | player a | player b | a's score | 95% CI | p | time per decision (a / b) |
|---|---|---|---:|---|---:|---|
| P33 | cheap (score, bag < 30, universe 30) | full (score, ungated, universe 60) | 48.91% ± 0.65% | [47.64, 50.18] | 0.093 | mean 19 / 71 ms |
| P34 | cheap | PAT static | 50.06% ± 0.62% | [48.84, 51.28] | 0.92 | median 46 / 2 ms |
| P35 (NWL23) | cheap | no-PAT static | **52.38% ± 0.46%** | [51.47, 53.28] | <0.0001 | median 45 ms |
| P36 | full, score mode | PAT static | 51.04% ± 0.65% | [49.76, 52.32] | 0.11 | median 67 / 2 ms |

- **Cheap vs full (P33).** The full configuration may be about 1 pp better than the cheap one (not significant) and uses about 4× the time.
- **Against PAT static.** The cheap configuration ties it (P34). The full configuration beats it by about 1 pp in each of P14 (equity) and P36 (score); neither is significant alone, and together they pool to +1.08 ± 0.46 pp (p ≈ 0.02).
- **NWL23 (P35).** The cheap configuration also holds up there.

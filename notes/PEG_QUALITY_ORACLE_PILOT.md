# Fresh PEG direct-4 oracle pilot

Accepted common direct-3/direct-4 judgments: 24/24.

| judge | mean s | median s | p75 s | p90 s | max s |
|---|---:|---:|---:|---:|---:|
| direct 3 | 35.87 | 3.06 | 6.97 | 28.36 | 652.23 |
| direct 4 | 117.93 | 11.08 | 25.21 | 87.83 | 2202.14 |

Best-nominee flips: 4/24 (16.7%). Pair-rank discordance excluding ties: 17.1%.

| comparison | n | mean direct-3 | mean direct-4 | mean 4−3 correction | sign flips | 4-on-3 slope |
|---|---:|---:|---:|---:|---:|---:|
| full32_minus_adaptive | 24 | -0.009924 | -0.006810 | +0.003114 | 0 | 0.695 |
| deep16_minus_adaptive | 24 | +0.008390 | +0.001756 | -0.006635 | 3 | 0.247 |
| deep24_minus_deep16 | 24 | +0.000000 | +0.000000 | +0.000000 | 0 | nan |

A 9-hour block projects to about 274 direct-4 positions before sentinel overhead, using `max(mean, p75)` pilot wall time. This is a scheduling estimate, not a strength result.

## Full-oracle decision

The frozen 200-position panel contains 73 policy-disagreement positions:
22/27/15/9 in bags 1/2/3/4. At the pilot mean, judging all 73 at direct
4 ply projects to 2.39 hours before sentinel overhead. The full disagreement
set is therefore the largest defensible sample and fits the overnight-scale
budget even with a substantial allowance for the observed 2202.14-second
maximum.

`quality_oracle_full_20260730.json` freezes all 73 disagreements. It was
written after the pilot, but its selection is independent of pilot values and
runtime because it includes every disagreement; no outcome-based subsampling
is possible. The runner will resume the 24 already accepted common judgments
and complete the remaining 49.

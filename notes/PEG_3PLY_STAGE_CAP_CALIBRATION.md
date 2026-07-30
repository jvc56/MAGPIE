# PEG 3-ply and stage-cap calibration

The follow-up 4-ply depth-versus-width experiment is in
`notes/PEG_4PLY_CALIBRATION.md`.

## Decision summary

Completing a 3-ply stage has measurable value, but widening the candidate
field has sharply diminishing returns.

On 40 held-out positions balanced across bag sizes 1–4:

- `{16,8}` changed the completed 2-ply move on 7/40 positions and gained
  +0.0159 utility and +1.576 win percentage points;
- `{24,12}` changed 8/40 and gained +0.0200 utility and +1.993 win points;
- `{48,24}` changed 8/39 completed pairs and gained +0.0220 utility and
  +2.187 win points;
- `{64,32}` produced exactly the same accepted 3-ply moves and values as
  `{48,24}`, while doing more work and censoring the same hard position.

Here `{K,K/2}` means that PEG evaluates the top K greedy candidates at 2 ply,
then re-ranks the top K/2 survivors at 3 ply. No deeper stages were swept.

If a 3-ply stage is added later, `{16,8}` is the best efficiency baseline.
`{24,12}` is a possible strength-biased alternative, but its incremental value
over `{16,8}` came from one initial-panel position and did not replicate in the
expansion panel. The data do not support `{48,24}` or `{64,32}` for production.

No live TimeManager or PEG stopping policy is changed on this branch.

## Protocol

The prospectively locked initial and expansion panels supplied 40 positions,
ten at each bag size 1–4. Every position received:

- a greedy seed arm;
- paired 2-ply/3-ply arms with schedules `{16,8}`, `{24,12}`, `{48,24}`,
  and `{64,32}`;
- a common 900-second arm cap;
- ten PEG workers, `no_pgo_release`, CSW24 RIT, full arm enumeration;
- forward/reverse arm order by position.

Only one process and position ran at a time. Candidate events retained
completion timestamps, ranks and totals, cumulative scenarios, solve-wide
nested nodes, wall time, process CPU, and partial/completed-stage state.

Every distinct accepted stage endpoint was judged together by a direct
3-ply pair-conditioned judge on one deterministic weight-stratified stride-4
root sample. Exact agreements were assigned zero pair regret without a judge.
The representative sensitivity subset was repeated at stride 2. A judgment
was accepted only when every nominee completed at 3 ply.

This judge measures alignment with a common direct-3-ply objective. It is not
a deeper 4-ply oracle; exhaustive direct 4-ply judging remained impractical.

## Completion

| Schedule | Completed through 3 ply | Partial/censored | Median total arm wall seconds |
| --- | ---: | ---: | ---: |
| `{16,8}` | 40/40 | 0 | 8.98 |
| `{24,12}` | 40/40 | 0 | 11.52 |
| `{48,24}` | 39/40 | 1 | 30.33 |
| `{64,32}` | 39/40 | 1 | 39.91 |

The single censored position was `b4-p01`. Both large schedules completed
their full 2-ply fields, then completed only 14 candidates at 3 ply:
14/24 for `{48,24}` and 14/32 for `{64,32}`. Those partial stages were retained
as work observations but were not credited as quality.

All 23/23 required stride-4 judges and 13/13 stride-2 judges completed.
Seventeen positions had exact agreement among every accepted endpoint and
needed no judge. No shallow value was substituted for a censored judge.

## Value of 3 ply

Positive values below are the direct-3-ply value of the completed 3-ply move
minus the paired completed 2-ply move.

| Schedule | Move changes | Utility gain (95% CI) | Win gain, pp (95% CI) | Spread gain (95% CI) |
| --- | ---: | ---: | ---: | ---: |
| `{16,8}` | 7/40 | +0.0159 [+0.0039, +0.0307] | +1.576 [+0.389, +3.049] | +0.936 [-0.227, +2.341] |
| `{24,12}` | 8/40 | +0.0200 [+0.0066, +0.0357] | +1.993 [+0.667, +3.549] | +1.064 [-0.129, +2.499] |
| `{48,24}` | 8/39 | +0.0220 [+0.0075, +0.0387] | +2.187 [+0.755, +3.860] | +1.078 [-0.153, +2.501] |
| `{64,32}` | 8/39 | +0.0220 [+0.0075, +0.0387] | +2.187 [+0.755, +3.860] | +1.078 [-0.153, +2.501] |

At `{16,8}`, 3 ply increased the total gain over greedy from +0.0903 to
+0.1061 utility. When 3 ply actually changed the move, the conditional gain
was +0.0906 utility and +9.01 win points. Thus the stage acts as a relatively
rare correction with a large payoff, rather than a small improvement on every
position.

The `{16,8}` 3-ply gain by bag was:

| Bag | Mean utility gain, 3 ply minus 2 ply |
| ---: | ---: |
| 1 | +0.0000 |
| 2 | +0.0418 |
| 3 | +0.0217 |
| 4 | 0.0000 |

The observed benefit was concentrated in bags 2 and 3. That supports keeping
production generally shallow and considering 3 ply only when the position and
remaining work budget justify it; it does not support unconditional 3-ply
deepening at bags 1 or 4.

## Value of wider caps

| Completed endpoint comparison | Move changes | Marginal utility | Marginal win, pp |
| --- | ---: | ---: | ---: |
| `{16,8}` → `{24,12}` at 3 ply | 1/40 | +0.0042 [0.0000, +0.0125] | +0.417 [0.000, +1.250] |
| `{24,12}` → `{48,24}` at 3 ply | 2/39 | +0.0015 [0.0000, +0.0044] | +0.143 [0.000, +0.427] |
| `{48,24}` → `{64,32}` at 3 ply | 0/39 | 0.0000 | 0.000 |

The initial panel showed the small cap-width gains. In the expansion panel,
all four schedules returned exactly the same completed 3-ply move on every
position. The widening effect therefore did not replicate, while the basic
3-ply benefit did:

| Cohort | `{16,8}` 3-ply increment | `{24,12}` | `{48,24}` | `{64,32}` |
| --- | ---: | ---: | ---: | ---: |
| Initial 20 | +0.0077 | +0.0161 | +0.0199 | +0.0199 |
| Expansion 20 | +0.0240 | +0.0240 | +0.0240 | +0.0240 |

The combined cap-width result is driven by three cap-transition disagreements
across two initial-panel positions. It is evidence for retaining a little
optional headroom, not for making a large field the default.

## Work cost

| Schedule | Median 3-ply-stage scenarios | Median 3-ply-stage nested nodes | Median local stage seconds |
| --- | ---: | ---: | ---: |
| `{16,8}` | 565 | 6,283,240 | 2.93 |
| `{24,12}` | 866 | 10,746,591 | 4.59 |
| `{48,24}` | 1,692 | 20,059,165 | 10.31 |
| `{64,32}` | 2,268 | 25,196,662 | 14.62 |

Relative to `{16,8}`, `{24,12}` used about 71% more median 3-ply nested nodes
for +0.0042 mean utility. `{48,24}` used about 87% more median nodes than
`{24,12}` for another +0.0015. `{64,32}` used another 26% over `{48,24}` for
exactly zero observed value.

Wall conversions had long tails. The accepted `{16,8}` 3-ply stage ranged up
to 306.6 local seconds, and `{24,12}` up to 465.6 seconds. Candidate
completion, scenarios, and nested nodes remain the portable strength/work
coordinates.

## Two-candidate 3-ply admission boundary

A 3-ply stage still should not begin unless at least two candidates are
expected to finish. Across 40 positions, incremental work from the completed
2-ply boundary through two completed 3-ply candidates was:

| Schedule | Scenarios, median / p90 | Nested nodes, median / p90 | Local wall seconds, median / p90 |
| --- | ---: | ---: | ---: |
| `{16,8}` | 60 / 1,975 | 383,087 / 6,233,588 | 0.29 / 3.52 |
| `{24,12}` | 62 / 1,908 | 23,618 / 4,519,402 | 0.04 / 3.14 |
| `{48,24}` | 62 / 1,908 | 11,790 / 4,470,172 | 0.02 / 3.18 |
| `{64,32}` | 53 / 1,044 | 25,100 / 5,228,970 | 0.04 / 3.19 |

For `{16,8}`, local p90/max time for the first two 3-ply candidates was
4.45/9.35 seconds at bag 1, 3.57/11.94 at bag 2, 3.73/5.96 at bag 3, and
0.58/0.58 at bag 4. As in the 2-ply admission study, a p90-only seconds
threshold would miss an observed tail. A future guard should reserve a
conservative per-bag work envelope plus safety, using scenarios where nested
nodes are zero.

## Sensitivity and machine load

Stride-2 and stride-4 best nominees agreed on 12/13 sensitivity positions.
On that enriched subset, the `{16,8}` 3-ply increment was +0.0369 at stride 4
and +0.0293 at stride 2; stride-2 minus stride-4 was -0.0076 with a CI that
included zero. The one best-nominee mismatch was `x-b3-p02`: stride 4 preferred
the 2-ply `6j GEO`, while stride 2 preferred the 3-ply `3a OGEE`.

Initial-panel sentinel normalized-throughput CV was 0.016 with -1.2% drift;
expansion-panel CV was 0.028 with -2.6% drift. No early or late segment was
flagged. Median process-CPU/wall scheduled-core occupancy across the arms was
about 1.04 on the ten-worker denominator; small values above one reflect
auxiliary/main-thread work. Timing was stable enough for local conversion,
while quality conclusions remain based on structural completion.

## Recommendation

1. Keep 2 ply as the main production refinement.
2. If remaining budget supports a 3-ply stage, start with `{16,8}` and only
   admit it when at least two 3-ply candidates are conservatively expected to
   complete.
3. Consider `{24,12}` only as a strength-biased alternative after fresh policy
   validation; its rare cap-width gain did not replicate.
4. Do not use `{48,24}` or `{64,32}` as the production default. Their marginal
   value was tiny to zero, their work cost was much higher, and the large
   schedules censored on the hard bag-4 tail.
5. A future policy experiment should focus 3-ply opportunity on bags 2–3 and
   retain shallow/long-budget behavior elsewhere. This branch deliberately
   does not implement that policy.

## Artifacts

Raw and generated artifacts remain ignored under:

- `obj/peg_time_calibration/run-20260729-stagecaps/`
- `obj/peg_time_calibration/run-20260729-stagecaps-expansion/`

Each directory contains `records.jsonl`, `raw.log`, `manifest.json`,
`analysis.json`, and `analysis.md`. The initial directory also contains
`analysis.combined.json` and `analysis.combined.md`. Together the raw
directories are approximately 157 MB and are not committed.

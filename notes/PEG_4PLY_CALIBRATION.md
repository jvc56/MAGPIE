# PEG 4-ply depth-versus-width calibration

## Decision summary

At a ten-minute arm cap, 4 ply was feasible on this panel but had very little
incremental decision value.

Across 40 held-out positions balanced across bag sizes 1–4:

- `{16,8,4}` and `{24,12,6}` both completed through 4 ply on 40/40
  positions;
- 3 ply changed the narrow 2-ply move on 7/40 positions and gained +0.0105
  direct-4 utility;
- 4 ply changed the narrow 3-ply move on 3/40 positions and gained only
  +0.0002 utility;
- widening `{16,8}` to `{24,12}` changed one 3-ply/4-ply move and gained
  +0.0014 utility, but that result came from one initial-panel bag-2 position
  and did not replicate in the expansion panel.

Thus the current search priority remains: complete a narrow 2-ply stage,
complete its 3-ply refinement, and consider 4 ply only with surplus budget
that can conservatively finish at least two candidates. The data do not
support replacing the narrow default with a wide or unconditional 4-ply
production schedule.

No live TimeManager or PEG stopping policy is changed on this branch.

## Protocol

The prospectively locked initial and expansion panels supplied 40 positions,
ten at each bag size 1–4. Every position received:

- a greedy seed arm;
- a narrow `{16,8,4}` arm;
- a wider `{24,12,6}` arm;
- a 600-second arm cap;
- ten PEG workers, `no_pgo_release`, CSW24 RIT, and full arm enumeration;
- forward/reverse arm order by position.

Only one process and position ran at a time. Candidate events retained ranks,
completion timestamps, cumulative scenarios, solve-wide nested nodes, process
CPU, wall time, and partial/completed-stage state.

Every distinct accepted stage endpoint was judged together by a pair-
conditioned direct-4-ply judge on one deterministic weight-stratified stride-4
sample. Exact endpoint agreement received zero regret without a judge. The
preselected sensitivity subset and every depth/width disagreement received a
stride-2 judge. A full-enumeration judge adjudicated stride-4/stride-2 best-
nominee disagreements and the coarse bag-1 4-ply change.

The direct-4 judge is a common-sample objective at the same search depth, not a
5-ply ground truth.

## Completion and judging

| Item | Completed | Censored |
| --- | ---: | ---: |
| `{16,8,4}` arms | 40/40 | 0 |
| `{24,12,6}` arms | 40/40 | 0 |
| Direct-4 stride-4 judges | 24/24 | 0 |
| Direct-4 stride-2 judges | 16/16 | 0 |
| Direct-4 full judges | 3/3 | 0 |

Sixteen positions had exact endpoint agreement and needed no judge. No partial
stage or shallower value was substituted for a direct-4 judgment.

The narrow and wide total-arm median local wall times were 14.52 and 20.89
seconds. Their maxima were 573.69 and 579.15 seconds. Completion within the
600-second cap is strong local feasibility evidence, but the maxima leave
little portable safety margin.

## Value by stage

Positive values below mean the later endpoint was preferred by the
adjudicated direct-4 objective.

| Comparison | Move changes | Utility gain (95% CI) | Win gain, pp (95% CI) | Spread gain (95% CI) |
| --- | ---: | ---: | ---: | ---: |
| Narrow 2→3 ply | 7/40 | +0.0105 [+0.0008, +0.0244] | +1.042 [+0.076, +2.424] | +0.660 [-0.645, +2.109] |
| Narrow 3→4 ply | 3/40 | +0.0002 [0.0000, +0.0007] | +0.021 [0.000, +0.063] | +0.310 [-0.072, +0.833] |
| Wide 2→3 ply | 8/40 | +0.0119 [+0.0016, +0.0257] | +1.181 [+0.160, +2.556] | +0.755 [-0.566, +2.214] |
| Wide 3→4 ply | 3/40 | +0.0002 [0.0000, +0.0007] | +0.021 [0.000, +0.063] | +0.310 [-0.072, +0.833] |

The observed 4-ply increment was about 2% of the 3-ply utility increment.
Conditional on one of the three 4-ply move changes, the mean gain was +0.00319
utility, +0.278 win percentage points, and +4.13 spread. These were rare,
small corrections rather than a broad strength improvement.

The three 4-ply changes were:

- bag 1 `b1-p04`: ZAIRE → AZIDE. The stride-4 and stride-2 samples rejected
  AZIDE, but full enumeration slightly preferred it by +0.000675 utility,
  entirely +6.75 spread at equal win rate.
- bag 3 `b3-p04`: TEARGAS → AGRASTE. Stride 4 valued the change at +0.00819
  utility and stride 2 at +0.00402.
- bag 2 `x-b2-p01`: EUGE → MOUE. Stride 4 rejected the change, while stride 2
  and full enumeration agreed on +0.000708 utility, entirely +7.08 spread at
  equal win rate.

Mean narrow 3→4 utility by bag was +0.000068, +0.000071, +0.000819, and zero
for bags 1–4 respectively. There is no observed bag-4 case for 4 ply.

## Width versus depth

| Comparison | Move changes | Mean utility gain (95% CI) |
| --- | ---: | ---: |
| Narrow → wide at 2 ply | 0/40 | 0.0000 |
| Narrow → wide at 3 ply | 1/40 | +0.0014 [0.0000, +0.0042] |
| Narrow → wide at 4 ply | 1/40 | +0.0014 [0.0000, +0.0042] |
| Narrow 4 ply → wide 3 ply | 4/40 | +0.0012 [-0.0005, +0.0042] |

The sole width gain was the already-known initial-panel bag-2 position
`b2-p01`: the wide schedule retained DUKA instead of narrow DAUB, worth about
+0.0559 conditional direct-4 utility. Every expansion-panel position returned
the same narrow and wide move.

Numerically, wide 3 ply beat narrow 4 ply, but its confidence interval includes
zero and the advantage is driven by that single non-replicating width rescue.
This is not sufficient evidence for a general wide-first policy.

## Work and the two-candidate admission boundary

| 4-ply schedule | Stage scenarios, median | Stage nested nodes, median | Local stage seconds, median |
| --- | ---: | ---: | ---: |
| `{16,8,4}` | 320 | 6,236,742 | 1.85 |
| `{24,12,6}` | 479 | 9,560,587 | 3.22 |

The wider 4-ply stage used about 53% more median nested nodes without changing
any additional 4-ply move beyond the candidate already rescued by width at
3 ply.

Incremental work from the completed 3-ply boundary through the first two
completed 4-ply candidates was:

| Schedule | Scenarios median / p90 / max | Nodes median / p90 / max | Local seconds median / p90 / max |
| --- | ---: | ---: | ---: |
| `{16,8,4}` | 142 / 1,965 / 3,816 | 436,722 / 23,846,995 / 78,768,856 | 0.23 / 9.98 / 47.86 |
| `{24,12,6}` | 142 / 1,965 / 3,816 | 36,775 / 26,854,447 / 78,238,915 | 0.06 / 10.63 / 46.87 |

All 80 arms completed at least two 4-ply candidates, so this run observed no
below-two start. Nevertheless, a p90-only admission reserve would have missed
the roughly 47-second local tail. A future guard should use a conservative
per-bag scenarios/nodes envelope plus safety and should retain scenario count
where nested exact-endgame nodes are zero.

## Sensitivity and machine load

Stride-2 and stride-4 best nominees agreed on 14/16 sensitivity positions.
Both disagreements received a completed full-enumeration judge. The coarse
bag-1 sample was also fully enumerated. All adjudicated comparisons above use
the full result when available; the unadjudicated all-stride-4 3→4 estimate
was -0.0130, demonstrating that sparse small-bag samples can be misleading.

Initial-panel sentinel normalized-throughput CV was 0.019 with -4.1% drift;
occupancies were 0.845/0.813/0.815. Expansion CV was 0.019 with +1.0% drift;
occupancies were 0.878/0.902/0.875. No early or late segment was flagged.
Median scheduled-core occupancy for the narrow and wide arms was about 1.045
on the ten-worker denominator.

Quality observations are structurally valid. Wall time remains a local
conversion only.

## Recommendation

1. Keep 2 ply as the main refinement and 3 ply as the first optional deeper
   stage. Its measured marginal value was about forty times the 4-ply
   increment.
2. Under a genuinely ample budget, `{16,8,4}` is a reasonable low-priority
   final refinement only after a conservative two-candidate admission check.
3. Do not start 4 ply merely because some wall time remains. The observed
   first-two-candidate tail was much larger than the median.
4. Treat `{24,12,6}` as a strength-biased experimental option, especially for
   a broad/unstable bag-2 frontier. Its only gain was rare and did not
   replicate, while its median 4-ply node cost was substantially higher.
5. Do not prioritize 4 ply at bag 4 from these data. No bag-4 move changed.

## Artifacts

Raw and generated artifacts remain ignored under:

- `obj/peg_time_calibration/run-20260730-4ply/`
- `obj/peg_time_calibration/run-20260730-4ply-expansion/`

Each directory contains `records.jsonl`, `raw.log`, `manifest.json`,
`analysis.json`, and `analysis.md`. The initial directory also contains
`analysis.combined.json` and `analysis.combined.md`. Together the directories
are approximately 94 MB and are not committed.

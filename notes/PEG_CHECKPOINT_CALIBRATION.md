# PEG candidate-checkpoint calibration

The follow-up comparison of completed 2-ply and 3-ply stages across halving
cap schedules is in `notes/PEG_3PLY_STAGE_CAP_CALIBRATION.md`.

## Decision summary

The 2-ply stage has substantial value, but its value is strongly front-loaded:

- one completed 2-ply candidate has exactly zero decision value because it is
  the greedy seed evaluated alone;
- two candidates captured about 42% of the full-stage mean utility gain, but
  the confidence interval still included zero;
- eight candidates captured about 87% of the full-stage utility and 88% of its
  win gain;
- twelve candidates captured all observed win gain and all but a small
  spread-only tail;
- candidates 12–24 changed no moves; candidates 24–32 changed two moves, with
  zero win gain and +0.5375 mean spread over all 40 positions.

This makes two separate controls appropriate:

1. Do not enter a stage unless there is enough remaining budget to complete at
   least two candidates. Work below that boundary is discarded and cannot
   compare moves.
2. Once admitted, do not assume that two or four candidates are enough. A
   minimum near eight candidates followed by a conservative stability test is
   more consistent with these data.

No live TimeManager or PEG stopping policy is changed on this branch.

## Protocol

The same 40 prospectively locked positions were used, with ten positions at
each bag size 1–4. Each position received one uncapped, full 32-candidate
2-ply trace using `no_pgo_release`, CSW24 RIT, and ten PEG workers. Candidate
events recorded the returned move, self value, timestamp, process CPU,
cumulative scenarios, and the solve-wide nested-node snapshot at each
completed-candidate boundary.

Every distinct running-best move—including exact self-utility co-leaders—was
judged together by a direct 3-ply pair-conditioned judge using one
deterministic weight-stratified stride-4 root sample. The original
representative subset was repeated at stride 2. Execution remained one
position/process at a time, with identical sentinels before, between, and
after the block.

All 40 traces completed all 32 candidates. The checkpoint frontiers contained
84 position-nominees, with a median of two and maximum of six per position.
All 21 required stride-4 judges and all 13 stride-2 sensitivity judges
completed; none was censored or replaced with a shallower value.

## Fixed stopping points

| Completed 2-ply candidates | Mean utility gain vs greedy (95% CI) | Win gain, percentage points | Disagreement with full stage | Mean full-minus-stop utility |
| ---: | ---: | ---: | ---: | ---: |
| 0 or 1 | 0.0000 [0.0000, 0.0000] | 0.0000 | 21/40 | +0.0903 |
| 2 | +0.0377 [-0.0015, +0.0984] | +3.7500 | 16/40 | +0.0527 |
| 4 | +0.0371 [-0.0083, +0.0988] | +3.6787 | 12/40 | +0.0533 |
| 8 | +0.0790 [+0.0251, +0.1476] | +7.8592 | 7/40 | +0.0113 |
| 12 | +0.0903 [+0.0363, +0.1567] | +8.9703 | 2/40 | +0.0001 |
| 32 | +0.0903 [+0.0363, +0.1568] | +8.9703 | 0/40 | 0.0000 |

The marginal chunks were:

| Candidate chunk | Mean utility gain (95% CI) | Win gain, percentage points | Spread gain | Mean nested-node cost |
| --- | ---: | ---: | ---: | ---: |
| 0→1 | 0.0000 [0.0000, 0.0000] | 0.0000 | 0.0000 | 127,531 |
| 1→2 | +0.0377 [-0.0015, +0.0984] | +3.7500 | +1.8311 | 109,054 |
| 2→4 | -0.0006 [-0.0187, +0.0147] | -0.0713 | +1.1784 | 247,229 |
| 4→8 | +0.0419 [+0.0030, +0.0890] | +4.1806 | +1.2753 | 845,928 |
| 8→12 | +0.0113 [-0.0028, +0.0294] | +1.1111 | +1.5375 | 784,549 |
| 12→16 | 0.0000 | 0.0000 | 0.0000 | 790,084 |
| 16→24 | 0.0000 | 0.0000 | 0.0000 | 1,469,044 |
| 24→32 | +0.0001 [0.0000, +0.0002] | 0.0000 | +0.5375 | 1,202,672 |

Later self-ranked candidates do not monotonically improve the 3-ply judgment:
the 2→4 mean utility change was slightly negative, and the full 2-ply move was
in the direct-3-ply best checkpoint-nominee set on 34/40 positions. Choosing
the hindsight-best checkpoint nominee would improve on the full 2-ply move by
only +0.0054 utility and +0.535 win percentage points on average. That is an
oracle diagnostic, not a usable stopping rule.

## Adaptive stopping

A retrospective plateau rule was also evaluated. `min N, patience P` means:
evaluate at least N candidates, then stop after P consecutive candidates have
failed to change the running winner. The rule uses no oracle result to decide
when to stop.

| Rule | Mean / median candidates | Different from full | Mean gain vs greedy | Mean full-minus-stop utility | Median nested nodes |
| --- | ---: | ---: | ---: | ---: | ---: |
| min 8, patience 4 | 8.78 / 8 | 4/40 | +0.0875 | +0.0029 | 612,552 |
| min 8, patience 8 | 10.88 / 10 | 2/40 | +0.0903 | +0.0001 | 717,656 |
| min 12, patience 4 | 12.22 / 12 | 2/40 | +0.0903 | +0.0001 | 956,568 |
| min 12, patience 8 | 13.07 / 12 | 2/40 | +0.0903 | +0.0001 | 956,568 |
| Full 32 | 32 / 32 | 0/40 | +0.0903 | 0.0000 | 3,004,547 |

`min 8, patience 8` is the most interesting exploratory rule. It retained all
observed win and utility gain, missed only +0.5375 all-position mean spread,
and used about 76% fewer median nested nodes than the full stage. Its result
replicated across the original and expansion panels:

| Cohort | Mean adaptive stop | Adaptive gain | Full gain | Full-minus-adaptive |
| --- | ---: | ---: | ---: | ---: |
| Initial 20 | 10.35 candidates | +0.0850 | +0.0851 | +0.0001 |
| Expansion 20 | 11.40 candidates | +0.0956 | +0.0956 | 0.0000 |

This remains an in-sample exploratory policy comparison. It should be
validated on a fresh panel before changing production.

## Two-candidate admission boundary

The incremental work from the completed greedy stage through two completed
2-ply candidates was:

| Bag | Scenario count, median / p90 | Nested nodes, median / p90 | Local wall seconds, median / p90 / maximum |
| ---: | ---: | ---: | ---: |
| 1 | 14 / 16 | 215,670 / 571,470 | 0.49 / 1.66 / 3.66 |
| 2 | 45 / 90 | 173,912 / 931,784 | 0.47 / 2.12 / 7.17 |
| 3 | 184 / 432 | 330,600 / 534,795 | 0.71 / 1.33 / 1.65 |
| 4 | 1,632 / 3,816 | 0 / 0 | 0.01 / 0.03 / 0.03 |

Bag-4 2-ply work has zero nested exact-endgame nodes at this boundary, so its
scenario count remains the relevant work coordinate. Overall, the
two-candidate median/p90 cost was 98,566/607,232 nested nodes and
0.34/1.46 local seconds, but the bag-2 maximum was 7.17 seconds under load.

The eventual admission test should be made at the stage boundary against
remaining time, using a conservative per-bag estimate of the first two
candidates plus safety margin. A p90-only threshold would still have failed
one observed bag-2 position. The data therefore support the rule’s structure,
but not yet a hard-coded seconds constant.

## Bag-size and sensitivity notes

Mean full-stage utility gains were +0.1761, +0.1006, +0.0759, and +0.0088 for
bags 1–4. Bag 1 benefited most from continuing toward twelve candidates; bag 2
was slightly negative at two and four candidates before becoming clearly
positive at eight; bag 3 stabilized early; bag 4 usually retained the greedy
move.

Stride-2 and stride-4 best checkpoint nominees agreed on 12/13 sensitivity
positions. On that subset, full-stage gain was +0.1120 at stride 4 and +0.1617
at stride 2. The disagreement was `x-b1-p01`: stride 4 preferred intermediate
`i8 XU`, while stride 2 preferred the eventual `pass`. Thus stride 4 appears
conservative for the main full-stage gain, but the sensitivity is material and
should remain explicit.

One position had exact running-best 2-ply ties. A supplemental common-sample
judge included every tied move; all completed, and the tied final candidates
had the same stride-4 utility. The tie did not change aggregate conclusions.

## Load and artifacts

Sentinel normalized-throughput CV was 0.053 with +10.0% before-to-after drift.
The early segment was flagged because the first sentinel had 0.747
scheduled-core occupancy; the between/after occupancies were 0.844/0.816.
Quality observations remain structurally valid, while wall seconds are only a
local work conversion.

Artifacts are under
`obj/peg_time_calibration/run-20260729-checkpoints/`:

- `records.jsonl` and `raw.log` contain the raw traces and judges;
- `analysis.json` contains the full machine-readable result;
- `analysis.md` contains the generated tables;
- `manifest.json` records the panels, assets, commit, and protocol.

The raw directory is approximately 32 MB and remains ignored rather than
committed.

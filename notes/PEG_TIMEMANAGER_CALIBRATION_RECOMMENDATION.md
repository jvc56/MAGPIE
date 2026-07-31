# PEG TimeManager calibration recommendation

## Decision

Do not change the live TimeManager policy on this branch.

For the next policy implementation/validation, use completed candidates as
the only stopping boundary:

1. Complete the greedy seed.
2. Do not admit a new fidelity stage unless its remaining portable budget can
   conservatively complete at least two candidates.
3. Admit the first two candidates as one wave. After that, admit one candidate
   at a time and replan only at a completed-candidate boundary.
4. Keep 2 ply as the production workhorse. The fresh panel supports getting to
   eight completed 2-ply candidates when work permits, then using a short
   stability patience rather than automatically filling 12 or 32.
5. Treat completed 3-ply `{16,8}` as an optional, separately admitted stage.
   The admission corpus currently supports a deadline guard only for bags
   2–3. `{24,12}` is a secondary strength-biased option in those bags when
   the larger completed-work reserve fits.
6. With roughly ten local minutes, prefer completed shallow width and a
   completed 3-ply stage over starting 4 ply. Existing 4-ply evidence shows a
   very small marginal gain, and the fresh representative experiment did not
   run production 4-ply arms.

This preserves the owner's direction: production PEG remains shallow and gets
longer budgets, while deeper work is an explicit surplus-budget escalation.

## Admission guard

The fresh completion-tail corpus contains 320 independent positions per bag,
with no final censoring. A future guard should use the following full-corpus
empirical maxima as its starting work envelope, then add an explicit safety
factor:

| stage admission | bag | scenarios | nested nodes | candidates |
|---|---:|---:|---:|---:|
| first two 2-ply candidates | 1 | 16 | 3,378,394 | 2 |
| first two 2-ply candidates | 2 | 144 | 5,110,942 | 2 |
| first two 2-ply candidates | 3 | 1,056 | 7,399,314 | 2 |
| first two 2-ply candidates | 4 | 15,840 | 29,684,225 | 2 |
| first two 3-ply candidates after 2-ply 16 | 2 | 144 | 637,317,412 | 2 |
| first two 3-ply candidates after 2-ply 16 | 3 | 1,440 | 452,287,912 | 2 |

These are portable componentwise work bounds, not seconds. On the M5, the
first-two 2-ply maxima converted to 5.38/15.50/13.83/155.75 seconds for bags
1–4. The two-candidate 3-ply maxima converted to 263.36 and 293.59 seconds for
bags 2 and 3. The feature-conditioned conformal model was rejected: it missed
more held-out cells and was often much more conservative than the flat bound.

The empirical maximum has `1 - 0.99^320 = 95.99%` probability of covering a
population p99 under exchangeability. It is not a universal deadline
guarantee. Bag 1 also had one held-out false start against its prefit
train/calibration maximum, so production still needs margin and telemetry.

## Value of 2-ply work and early stopping

The representative quality panel has 200 independent source games, 50 per
bag. Every distinct policy nominee was judged at direct 4 ply on the same
fixed stride-4 sample; all 73 disagreement positions completed.

| added completed candidates | panel utility gain (95% CI) | move changes | mean added nested nodes | M5 mean added seconds |
|---|---:|---:|---:|---:|
| 2→4 | +0.00780 [+0.00170, +0.01664] | 30/200 | 444,791 | 0.80 |
| 4→8 | +0.00492 [+0.00017, +0.01144] | 18/200 | 799,677 | 1.46 |
| 8→12 | -0.00357 [-0.00891, +0.00004] | 11/200 | 950,357 | 1.71 |
| 12→32 | +0.00410 [+0.00005, +0.01061] | 18/200 | 5,060,633 | 9.21 |

The non-monotone point estimates are possible because each checkpoint can
nominate a different move and the common oracle, not the later search stage,
defines value. They argue against treating a half-finished stage or raw
elapsed time as progress.

The reconstructed stopping policies were:

| stop policy | full-32 regret (95% CI) | disagreement | mean candidates | mean nested nodes | M5 mean seconds |
|---|---:|---:|---:|---:|---:|
| min 8 / patience 4 | +0.00025 [-0.00683, +0.00772] | 25/200 | 8.20 | 1,812,799 | 6.05 |
| min 8 / patience 8 | +0.00229 [-0.00264, +0.00896] | 23/200 | 9.33 | 2,103,295 | 6.52 |
| min 12 / patience 8 | +0.00407 [+0.00002, +0.01026] | 15/200 | 12.62 | 2,858,268 | 7.95 |
| full 32 | 0 | 0/200 | 32.00 | 7,789,044 | 16.90 |

Extending patience 4→8 after candidate 8 cost 1.14 candidates and 290,496
mean nodes while changing only 2/200 policy moves; its measured marginal
utility was -0.00204 [-0.00617, +0.00004]. Raising the minimum from 8→12 at
patience 8 cost 3.29 candidates and 754,973 nodes; its marginal utility was
-0.00178 [-0.00431, +0.00004].

Therefore min-8/patience-4 is the best next adaptive policy to validate, not a
proven drop-in replacement. Full-32 minus min-8/patience-8 failed
noninferiority at utility margins 0.00025, 0.0005, and 0.001; the other
adaptive intervals are also too wide to claim a deadline-safe strength bound.
When there is genuine surplus budget, continuing toward 32 retains measurable
rare-rescue value.

## Value of 3 ply and wider initial fields

| comparison | panel utility (95% CI) | win pp (95% CI) | spread (95% CI) | move changes |
|---|---:|---:|---:|---:|
| `{16,8}` 3 ply minus min-8/patience-8 2 ply | +0.00282 [-0.00028, +0.00763] | +0.267 [-0.040, +0.764] | +1.436 [+0.675, +2.280] | 42/200 |
| `{24,12}` minus `{16,8}` | +0.00165 [-0.00000, +0.00443] | +0.164 [+0.000, +0.428] | +0.103 [-0.262, +0.472] | 10/200 |

Conditional on a move change, `{16,8}` gained +0.01341 utility and `{24,12}`
gained +0.03298. Exact direct-4 root outcomes were 31 better / 10 worse / 159
ties for `{16,8}` versus adaptive, and 6 / 3 / 191 for `{24,12}` versus
`{16,8}`.

The `{16,8}` effect is a trend, not a conclusive overall win. Its panel utility
CI crosses zero and it costs 79,984,057 mean incremental nested nodes.
Bag-conditioned utility points were +0.00028, -0.00157, +0.00293, and
+0.00962 for bags 1–4. That pattern does **not** validate a simple bag-2/3-only
strength prior; it differs from the earlier 40-position contested panel.

The extra `{24,12}` width was concentrated in bags 2–3: +0.00502 and +0.00160
panel utility, versus -0.00003 at bag 1 and exact zero at bag 4. It costs
31,701,791 mean additional nested nodes over `{16,8}`. Thus:

- `{16,8}` remains the efficient deeper baseline.
- `{24,12}` is the preferred ample-budget width experiment for bags 2–3.
- The combined earlier cap sweep still gives no production case for
  `{48,24}` or `{64,32}`: their width gain did not replicate, and `{64,32}`
  added zero observed value over `{48,24}`.
- Do not add bag-1/4 3-ply admission until their naturalistic two-candidate
  completion tails are calibrated, regardless of the fresh bag-4 quality
  point estimate.

## What the direct-4 evidence says

Direct-4 was essential as an oracle. Across all 73 disagreements it changed
the best nominee versus direct 3 on 9 positions (12.3%). Direct-3 judging
overstated `{16,8}` versus adaptive by 0.00303 panel utility:

| comparison | direct-3 utility | direct-4 utility | correction |
|---|---:|---:|---:|
| full 32 minus adaptive | +0.00276 | +0.00229 | -0.00047 |
| `{16,8}` minus adaptive | +0.00585 | +0.00282 | -0.00303 |
| `{24,12}` minus `{16,8}` | +0.00165 | +0.00165 | -0.00001 |

This confirms matching-horizon bias in the earlier 3-ply-judged result. It
does not by itself measure the value of a production 4-ply candidate search:
the direct-4 oracle only evaluated moves nominated by shallower policies.

The prior 40-position 4-ply arm experiment measured only +0.0002 utility for
the narrow 3→4 stage, with 3/40 move changes, versus +0.0105 for 2→3. Its
`{16,8,4}` arm maximum was 573.69 local seconds under a 600-second cap. In the
fresh oracle, merely adjudicating the nominated moves had a 9.45-second
median, 109.48-second p95, and 2202.14-second maximum at direct 4. Consequently
a nominal ten minutes is not a portable guarantee for arbitrary 4-ply work.

With ample time, the present evidence prefers `{24,12}` width after completing
the narrow 3-ply path over routinely adding a 4-ply stage. A future
representative production-4-ply experiment should compare those choices
directly under a deeper oracle and must use the same two-candidate admission
rule.

## Oracle and load sensitivity

- Direct-3/direct-4 stride-4: 73/73 common disagreement positions accepted at
  both depths; no rejected or censored judgments.
- Prospectively locked stride-2 subset: 8 positions, two per bag; 8/8 accepted
  at both depths. Stride 2 changed the best nominee on 1/8 at both direct 3 and
  direct 4. Mean absolute nominee utility movement was about 0.0060.
- Quality-run sentinels: 41; normalized-throughput CV 0.085, early/late drift
  -4.1%, median scheduled-core occupancy 0.853, no flagged noisy segment.
- Completion-tail sentinels: 41; CV 0.026, drift -0.5%, occupancy 0.944, no
  flagged segment.

The M5 was loaded, but completed scenarios/nodes/candidates make the quality
observations structurally valid. All seconds in this report are local
conversion estimates.

## Evidence classification

Measured:

- 1,280/1,280 completion-tail positions and 640/640 deeper bag-2/3 traces,
  with zero final censoring.
- 200/200 representative quality maps; 600/600 experiment arms completed.
- Direct-3 and direct-4 stride-4 coverage of all 73 policy disagreements;
  stride-2 sensitivity on 8/8 locked positions.
- Every agreement is exact zero and every distinct nominee used by an
  accepted comparison completed under the common oracle.

Extrapolated:

- Generalization from the finite independent panels, represented by
  bag-stratified source-game bootstrap intervals.
- Population-tail coverage of empirical completion maxima.
- Conversion of portable work to seconds on this loaded M5.

Remaining uncertainty:

- The stride-2 sensitivity panel is small and had one best-move flip.
- Rare, high-impact move rescues make the adaptive-policy CIs wide; no tested
  adaptive rule established the requested noninferiority margins.
- Naturalistic two-candidate 3-ply bounds exist only for bags 2–3.
- The representative panel did not run production 4-ply arms or a 5-ply
  oracle, so direct-4 search value remains less certain than direct-4
  evaluation value.
- The admission maxima need an explicit production safety factor and online
  miss telemetry; this experiment does not select that factor.

Raw logs remain ignored under `obj/`. Exact commands, hashes, accounting,
censoring, load diagnostics, and projected/actual wall time are in
`tools/peg_time_calibration/fresh_quality_20260730.manifest.json` and
`tools/peg_time_calibration/admission_completion_tail_20260730.manifest.json`.

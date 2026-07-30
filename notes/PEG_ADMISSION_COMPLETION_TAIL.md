# PEG admission/completion-tail calibration

Status: **1280/1280 positions complete**. This report is final only when the numerator is 1,280.

The strength-portable boundary is the completed stage work vector `(scenarios, nested endgame nodes, candidate overhead)`. Wall time is reported separately as an M5 conversion and is not used as a strength feature.

## Completion work

| bag | stage | n | scenarios median / p90 / p95 / p99 / max | nested nodes median / p90 / p95 / p99 / max | wall s median / p90 / p95 / p99 / max | occupancy median |
|---:|---|---:|---|---|---|---:|
| 1 | first2:2ply_wave | 320 | 14 / 16 / 16 / 16 / 16 | 62,832 / 725,637 / 1,073,829 / 2,306,677 / 3,378,394 | 0.25 / 1.13 / 1.61 / 3.07 / 5.38 | 0.373 |
| 1 | first2:greedy_seed | 320 | 4,326 / 26,178 / 44,052 / 80,395 / 131,072 | 0 / 0 / 0 / 0 / 0 | 0.02 / 0.08 / 0.10 / 0.20 / 0.40 | 0.628 |
| 2 | deep:2ply_to_16 | 320 | 492 / 800 / 884 / 1,116 / 1,152 | 1,524,129 / 7,710,183 / 11,165,263 / 19,115,116 / 26,268,983 | 2.10 / 11.66 / 18.48 / 37.78 / 76.21 | 0.814 |
| 2 | deep:3ply_wave_2 | 320 | 64 / 114 / 114 / 144 / 144 | 2,417,603 / 26,110,044 / 56,359,174 / 173,175,320 / 637,317,412 | 0.73 / 11.90 / 22.30 / 78.82 / 263.36 | 0.928 |
| 2 | deep:greedy_seed | 320 | 15,422 / 102,865 / 167,852 / 367,992 / 483,256 | 0 / 0 / 0 / 0 / 0 | 0.06 / 0.29 / 0.53 / 0.80 / 1.43 | 0.843 |
| 2 | first2:2ply_wave | 320 | 58 / 114 / 114 / 144 / 144 | 155,348 / 1,203,892 / 1,846,411 / 2,790,781 / 5,110,942 | 0.34 / 1.59 / 2.74 / 7.25 / 15.50 | 0.719 |
| 2 | first2:greedy_seed | 320 | 15,422 / 102,865 / 167,852 / 367,992 / 483,256 | 0 / 0 / 0 / 0 / 0 | 0.06 / 0.28 / 0.52 / 0.79 / 1.44 | 0.839 |
| 3 | deep:2ply_to_16 | 320 | 2,379 / 4,632 / 6,000 / 7,320 / 8,400 | 1,896,635 / 8,719,597 / 16,199,550 / 33,393,394 / 47,367,860 | 2.63 / 13.43 / 25.11 / 78.32 / 91.46 | 0.886 |
| 3 | deep:3ply_wave_2 | 320 | 317 / 716 / 796 / 1,056 / 1,440 | 1,205,910 / 21,790,150 / 52,044,278 / 130,966,268 / 452,287,912 | 0.32 / 11.80 / 21.74 / 84.06 / 293.59 | 0.916 |
| 3 | deep:greedy_seed | 320 | 91,692 / 440,056 / 1,046,108 / 1,556,160 / 2,279,624 | 0 / 0 / 0 / 0 / 0 | 0.31 / 1.86 / 3.73 / 7.96 / 17.25 | 0.929 |
| 3 | first2:2ply_wave | 320 | 266 / 620 / 756 / 1,056 / 1,056 | 212,699 / 1,258,342 / 2,599,431 / 4,571,562 / 7,399,314 | 0.39 / 1.75 / 3.43 / 10.64 / 13.83 | 0.813 |
| 3 | first2:greedy_seed | 320 | 91,692 / 440,056 / 1,046,108 / 1,556,160 / 2,279,624 | 0 / 0 / 0 / 0 / 0 | 0.31 / 1.91 / 3.72 / 8.11 / 17.50 | 0.931 |
| 4 | first2:2ply_wave | 320 | 1,208 / 4,290 / 6,420 / 10,944 / 15,840 | 186,176 / 2,021,715 / 3,818,602 / 11,326,478 / 29,684,225 | 0.36 / 2.93 / 6.18 / 30.09 / 155.75 | 0.902 |
| 4 | first2:greedy_seed | 320 | 849,365 / 2,964,883 / 6,286,575 / 12,015,318 / 15,635,070 | 0 / 0 / 0 / 0 / 0 | 2.84 / 15.17 / 27.09 / 55.02 / 109.07 | 0.956 |

## Admission-bound audit

Each bag uses 128 training, 128 conformal-calibration, and 64 prospectively ordered held-out positions. The 99% conformal rank is the maximum calibration residual (rank 128/128).

| bag / stage | flat vector bound (scenarios; nodes; candidates) | flat false starts | conditioned false starts | conditioned median bound (scenarios; nodes; candidates) |
|---:|---|---:|---:|---|
| 1 first2:2ply_wave | 16; 3,157,139; 2 | 1/64 | 1/64 | 250; 1,592,047; 2 |
| 2 first2:2ply_wave | 144; 5,110,942; 2 | 0/64 | 0/64 | 57,509; 43,108,229; 2 |
| 3 first2:2ply_wave | 1,056; 7,399,314; 2 | 0/64 | 1/64 | 683,558; 9,895,517; 2 |
| 4 first2:2ply_wave | 15,840; 29,684,225; 2 | 0/64 | 1/64 | 2,120,929; 9,615,892; 2 |
| 2 deep:3ply_wave_2 | 144; 637,317,412; 2 | 0/64 | 0/64 | 279,227; 653,673,683; 2 |
| 3 deep:3ply_wave_2 | 1,440; 452,287,912; 2 | 0/64 | 1/64 | 1,847,406; 75,547,407; 2 |

## Load diagnostics

41 identical sentinels: normalized throughput CV 0.026; early-to-late drift -0.5%; median scheduled-core occupancy 0.944. Noisy segments: none.

Structurally complete work observations remain valid when a sentinel marks their local wall conversion as noisy.

## Accounting

- Complete explicit two-candidate 2-ply traces: 1280/1,280
- Complete 16-candidate 2-ply + two-candidate 3-ply traces: 640/640
- Censored final corpus observations: 0
- Failed/interrupted process records: 0
- Interrupted attempts without a process footer, both resumed: 2
- Observed run span: 4.20 h; projected remaining arm wall time at current per-bag means: 0.00 h (median/p90 scenario: 0.00/0.00 h; future sentinel overhead excluded)

The two footerless invocations were operator interruptions while correcting the
early trace topology. Both positions were safely resumed and completed, so
they are interrupted *attempts* for process accounting but not censored corpus
observations or completion bounds.

## Interpretation

- With 320 uncensored observations per bag, the probability that an empirical
  maximum covers a population p99 is `1 - 0.99^320 = 95.99%`, assuming the
  prospectively sampled positions are exchangeable. This is p99 coverage
  evidence, not a universal deadline guarantee.
- The prefit flat componentwise maximum had zero held-out false starts in five
  of six bag/stage cells. Bag 1's first two 2-ply candidates exceeded the
  prefit nested-node maximum once (1/64): 3,378,394 nodes versus 3,157,139.
  The full-corpus empirical maximum is therefore the more appropriate
  conservative calibration value, with an explicit safety factor still
  required in production.
- The feature-conditioned split-conformal model is not supported for
  production admission. It missed in four of six cells, compared with one for
  the flat bound, and its component predictions are often drastically more
  conservative without improving held-out reliability.
- The first-two admission wave is cheap in the median but has a material tail:
  the M5 maxima were 5.38 s (bag 1), 15.50 s (bag 2), 13.83 s (bag 3), and
  155.75 s (bag 4). These are local conversions only. A TimeManager stage
  should be admitted only when its portable work budget covers a complete
  two-candidate wave plus safety margin; it should never begin a one-candidate
  fragment.
- The first two 3-ply candidates after a completed 16-candidate 2-ply boundary
  are much heavier-tailed: up to 637,317,412 nested nodes / 263.36 M5 seconds
  for bag 2 and 452,287,912 nodes / 293.59 seconds for bag 3. This supports
  treating 3-ply escalation as a separately admitted stage rather than an
  automatic continuation.

## Reproduction and artifacts

- Collection:
  `PYTHONDONTWRITEBYTECODE=1 python3 -B tools/run_peg_completion_tail.py --output-dir obj/peg_completion_tail_20260730 --threads 18`
- Analysis:
  `PYTHONDONTWRITEBYTECODE=1 python3 -B tools/analyze_peg_completion_tail.py --json tools/peg_time_calibration/admission_completion_tail_20260730.summary.json --markdown notes/PEG_ADMISSION_COMPLETION_TAIL.md`
- At 163/1,280 positions, the mean-duration projection estimated 3.23 h of arm
  work remaining (median/p90 alternatives 1.32/6.08 h, excluding future
  sentinels). The actual end-to-end collection span was 4.20 h.
- Raw records and logs are under `obj/peg_completion_tail_20260730/` and are
  intentionally excluded from git. Their hashes and full machine/build
  provenance are committed in
  `tools/peg_time_calibration/admission_completion_tail_20260730.manifest.json`.

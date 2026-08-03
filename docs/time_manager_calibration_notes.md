# TimeManager calibration decision log

This file records calibration decisions that determine whether learned live
allocation may be enabled. The complete design and historical results remain
in [`notes/TIME_MANAGER.md`](../notes/TIME_MANAGER.md).

## Conditional checkpoint-regret panel (2026-08-03)

Status: **failed point-reliability gate; no prospective run scheduled; live
allocation disabled**.

The audited development panel contains 96 complete-game roots, one sorted
top-60 p6 cumulative trace per root, checkpoints every 256 iterations through
3M nodes, and a common 10-ply risk-set judge with 100,000 samples per distinct
nominee. Accepted computation starts at attempt 2 because attempt 1 produced
no output after a frozen-runner working-directory relocation. The final audit
accepted the contiguous 0--95 prefix and reconciled 160,704 checkpoints, 982
judge nominees, 98.2 million judge iterations, move identities, stability
counters, and zero dropped events.

The five-outer/four-inner-fold conditional model uses complete games as folds
across the corrected nested-width, combined-regret, and checkpoint panels. Its
labels decompose total current-turn regret into candidate-generation and
within-set BAI components. Oracle judge values are label-only, the future/full
selected move is not a feature, and the scope ends at the checkpoint-observable
sorted top-60 union.

At width 60, the stability-aware model has:

- 844 rows from 192 games;
- through-origin reliability slope `0.7819`, game-clustered bootstrap 95% CI
  `[0.4171,1.2790]`;
- 2/10 reliability bins underpredicting actual regret by more than 2x, worst
  ratio `43.97`;
- 98.70% marginal q95-bound coverage and 96.35% simultaneous game coverage;
- mean upper bound `0.006498` versus mean actual regret `0.000366`.

A matched no-stability ablation improves the slope to `0.9311` and reduces
game-weighted absolute error by `0.0000375` (95% CI
`[0.0000198,0.0000552]`), but four reliability bins still exceed the 2x
underprediction limit. The existing stability bands are therefore rejected as
a calibration improvement, not promoted because their aggregate slope happens
to lie in range.

Checkpoint regret is sparse and heterogeneous: 51/652 checkpoint rows are
positive, concentrated in PlayChooser trajectories, bag 16--35, and states
with multiple near-tie challengers. The next model iteration should address
that event process and continuous move-stability history. It must pass the
same held-out point and optional-stopping gates on genuinely fresh games before
the separately required terminal-game match is considered.

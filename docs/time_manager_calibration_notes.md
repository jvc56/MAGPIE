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

## Same-arm horizon-differential replay (2026-08-03)

Status: **target substantially improved; strict reliability gate still
failed; no fresh panel scheduled; live allocation disabled**.

The absolute common-judge regret target contains error that additional work
from the current SIM arm cannot remove. In the 96-root checkpoint panel, every
choice already matched its 3M-node horizon by 750K nodes, while the horizon
moves retained mean absolute judge regret `0.00023479`. The revised target is
the signed common-judge improvement from the current incumbent to the same-arm
3M horizon, represented as horizon-mismatch probability times conditional
signed improvement. Horizon identities and judge values remain labels only;
five folds contain complete source games; candidates outside the sorted top-60
union remain unmeasured.

Across 1,056 fixed-landmark rows, horizon mismatch falls from 49/96 roots at
25K nodes to 18/96 at 300K, 3/96 at 400K, and zero by 750K. The event predictor
has `AUC=0.9087`, through-origin slope `0.9281`, and 2/10 bins underpredicting
by more than 2x (worst `2.404`). The signed target averages `0.002449` actual
versus `0.002616` predicted. This is a large calibration improvement over the
absolute target's worst `43.97x` miss, but it does not pass the unchanged
no-bad-decile gate.

Exploratory replay is encouraging but non-decisive. A 100K-node Rule of Zero
with two stable checkpoints and no near-tie challengers stops 89/96 roots,
saves 82.26% of nodes on average, and has zero horizon-choice mismatches. An
examined cross-fitted score threshold of `0.000025` stops 94/96 and saves
81.91%, also with zero mismatches. These rules were evaluated on reused
development outcomes; the zero-event one-sided 95% mismatch upper bound is
still 3.07%. Neither result is a prospective pass, and the prior
absolute-regret conformal bound cannot be transferred to the new label.

The implementation therefore exports reproducible model and replay artifacts
but does not freeze a production threshold or launch another judged panel.
The next candidate must be specified before fresh outcomes—preferably a
reviewed Rule-of-Zero controller or game-level conformal risk rule—and must
still pass fresh stopped/matched/full controls and the separate mirrored
terminal-game gate.

## Rule-of-Zero prospective preregistration (2026-08-03)

Status: **frozen before fresh trajectory generation; live allocation remains
disabled**.

The prospective candidate stops only when `nodes >= 100000`, the normalized
incumbent has survived at least two consecutive 256-iteration checkpoints,
and the existing two-sided 99% difference-CI near-tie counter is zero. Missing
telemetry runs to the p6 3M-node slice. The constants were selected on the
96-root development replay and that provenance is part of the frozen claim.
The previously examined `0.000025` model threshold is shadow-only and cannot
affect this panel.

Collection is 320 fresh, one-root-per-complete-game units: 40 per static or
PlayChooser-g3000ms policy crossed with each of four SIM bag bands. A
SHA-ranked panel order fixes an independent exact-stopped-iteration control on
indices `0 mod 3` and a common-judge audit on indices `0 mod 10`. Every choice
mismatch is also judged; all other primary labels require no judge because the
stopped choice is an incumbent from the same cumulative trace. The frozen
shadow model is evaluated at every checkpoint without horizon or judge labels.
Matched arms reproduce the cumulative scheduler's initial round-robin regime
when a stop precedes the full arm's uniform floor, then use normal top-two IDS
with the same absolute floor for later stops; every matched iteration count,
sampling regime, and floor is logged.

Historical 3-minute equal-slice turns all implied more than the experiment's
3M-node arm cap (minimum 3,652,554 nodes), so the separately logged
equal-slice landmark is capped at 3M and coincides with the full-arm endpoint
in this panel. The rule must meet both named mismatch summaries, even though
they have the same endpoint here.

The exact gates are: one-sided 95% mismatch upper bound at most 1.5%;
complete-game judged missed-value 95% CI upper at most 0.001; mean node saving
at least 50%; and a matched stopped-minus-independent interval containing zero
with upper endpoint at most 0.001. Any failure rejects Rule of Zero and sends
the shadow scores to a separately frozen CRC replay. It does not authorize a
threshold search on these outcomes. A pass remains only a surrogate pass; the
mirrored terminal-game gate is still mandatory.

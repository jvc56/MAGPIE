> Historical experiment: PAT inference support has been removed. Current validation uses static evaluation; see [infbench](infbench.md).

# Softmax calibration by move type, leave size, and game stage

This on-demand experiment fits the best tested margin/temperature settings by
minimizing one-percent-prior-mixture log loss. It reports KLV, vowel-count,
face-score, and summed tile-count MSE separately; these are not tuning targets.
An uninformed prior is an explicit candidate. Temperature zero denotes the old
hard-margin model; positive temperatures are softmax. No production defaults
are changed by this experiment.

The grid is margins 10, 20, 40, 80 and temperatures 0, 2, 5, 10, 20. A winner
at a grid boundary is a reason to extend a later training experiment, not proof
of a global optimum. The prior mixture is fixed at 1%, not fitted. Raw NLL is
infinite when any true leave is excluded.

Placements keeping 1–4 tiles are exhaustively enumerated. Each rack is evaluated
with static equity and exact PAT best-move generation, including PAT's utility
adjustment. Regrets are reused across the entire grid. Static 20/5 is checked
against production inference on every such observation, within 1e-6 for all five
metrics. Placements keeping 5–6 tiles and all exchanges use the census-backed
production static model for each grid point. PAT exchanges are not implemented
in this experiment: inferred exchanged identities would also need PAT-consistent
exchange selection. All exchange NLLs score the retained leave, not the discarded
set or the rack after replacement draws.

The generating policy is the earlier CSW21 infbench two-ply simulator: 15
candidates, 1,000 iterations, one simulation thread, no inference and no PAT.
Its opponent is static equity. This is not the 15-second CSW24 overnight policy;
validation against that stronger policy is still needed before deployment.

## Fixed data and split

Main batch: 180 game pairs / 360 games, seeds 2026131000–2026131179. Pairs 0–119
are training; pairs 120–179 are validation. Per game, sample the first placement
in each leave-size/bag-bin cell and every natural exchange, up to 20 observations.
This deliberately covers the cells; aggregate metrics describe this sampling
scheme rather than the natural frequency of all moves.

Bag bins are 1–14, 15–42, and 43+. With both racks full these correspond to
15–28, 29–56, and 57+ unplayed tiles, including both racks. Bag-empty positions
are excluded. Both games in a pair always stay in the same split.

Additional exchange batch: 360 pairs / 720 games, seeds 2026140000–2026140359.
Only natural exchanges are retained; no exchanges are forced and games without
an exchange are not replaced. The first 240 pairs train exchange groups; the
last 120 validate them. Global tuning and primary aggregate validation use only
the main batch. Exchange-specific fitting and reporting can use both batches.
The extra batch was selected because the main exchange count was sparse, before
examining parameter quality results.

A cell gets its own setting only with at least 40 training observations from
30 distinct game pairs. Otherwise it backs off in this order: move type + leave
size; move type; global. Settings and model choice are fitted only on training
pairs. Validation results never choose a setting. The PAT hybrid uses PAT for
1–4-tile placements and static equity otherwise. Candidate families are static,
hybrid, and the prior. Reported paired intervals are approximate normal intervals
across validation game pairs, not independent moves.

## Reproduction

The registered on-demand test accepts:

```sh
./bin/magpie_test infbench:180:20:2026131000:calibrate
./bin/magpie_test infbench:360:20:2026140000:calexchanges
python3 scripts/analyze_inference_calibration.py data/benchmarks/softmax-calibration-20261103
```

The saved run used 18 workers × 10 pairs for the main batch and 12 workers × 30
pairs for the exchange batch, with disjoint consecutive seeds. `manifest.json`
records the split and grid; `observations.log` and `exchange-observations.log`
contain `CALIBRATION` and `CALTIMING` records. The merged exchange log offsets
pair indices by 180. The analysis writes `calibration-results.json`. Preserve
this indexing when rerunning the single-worker commands above.

## Results — November 3, 2026

Both batches completed: 1,080 games. There are 1,484 training observations,
714 primary validation observations, and 37 validation exchanges across both
batches (some are also in the primary sample). Exchange training has 72
observations. No settings were chosen using validation losses. Perturbing every
validation loss left every fitted setting unchanged in a leakage check.

| Policy | Validation NLL | KLV MSE | Vowel MSE | Score MSE | Tile MSE |
|---|---:|---:|---:|---:|---:|
| prior | 6.262433 | 91.095634 | 0.701131 | 9.073621 | 2.535351 |
| static20t5 | 5.798933 | 77.016063 | 0.549291 | 8.278021 | 2.385216 |
| pat1to4_20t5 | 5.844539 | 77.108677 | 0.549876 | 8.388857 | 2.392498 |
| global_static_tuned | 5.763345 | 77.903377 | 0.553610 | 8.343595 | 2.389684 |
| global_hybrid_tuned | 5.775127 | 77.975560 | 0.554249 | 8.382696 | 2.392254 |
| stratified_tuned | 5.736521 | 78.269325 | 0.555209 | 8.334081 | 2.392089 |

Global training selects static 80/5; the hybrid family also selects 80/5.
Stratification reduces primary validation NLL from 5.798933 to 5.736521. Its
pair-weighted difference against 20/5 is -0.058769 nats, 95% interval
[-0.102636, -0.014902]. Against global 80/5, however, its additional reduction
has interval [-0.055208, +0.006701], so the value of stratification beyond
simply widening the margin is not established. KLV and the other reported
feature MSEs worsen slightly against 20/5. These are exploratory comparisons,
without multiple-comparison correction, not evidence of better game results.

The pooled exchange fit chooses static 80/2. Validation exchange NLL falls
4.414500 → 4.334237, but the paired interval is [-0.265027, +0.109557]; there is
not enough evidence for a confident exchange improvement or separate settings
for exchange size/stage. Empty retained leaves are deterministic and use prior.

PAT at 20/5 worsens held-out NLL for each of leave sizes 1–4. Training selects
PAT only for three-tile early-game and four-tile late-game placements in the
stratified policy. This does not establish those individual choices as reliable;
they are part of the complete fitted policy evaluated above.

### Fitted placement settings

Entries are margin/temperature. `PAT` marks the two selected PAT cells; all
others use static equity. Stage is pre-move bag count. Asterisk marks a fallback
to a broader training group rather than an independently fitted stage cell.

| Leave size | Bag 43+ | Bag 15–42 | Bag 1–14 |
|---|---|---|---|
| 1 | 80/10 | 40/10 | 80/10* |
| 2 | 80/5 | 40/5 | 80/20 |
| 3 | 40/5 PAT | 80/5 | 40/10 |
| 4 | 40/5 | 20/5 | 80/10 PAT |
| 5 | 40/5 | 80/5 | 40/10 |
| 6 | 80/5* | 80/5* | 80/5* |

80 is the upper tested margin and 20 the upper positive temperature. Boundary
winners require broader training-grid exploration before calling them optimal.
All choices remain experimental; production defaults are unchanged.

### Cell support

| Type | Leave | Bag | Training n | Validation n | Fit level |
|---|---:|---|---:|---:|---|
| exchange | 0 | 1-14 | 1 | 0 | deterministic-empty-leave |
| exchange | 0 | 15-42 | 1 | 1 | deterministic-empty-leave |
| exchange | 0 | 43+ | 2 | 3 | deterministic-empty-leave |
| exchange | 1 | 1-14 | 1 | 0 | type |
| exchange | 1 | 15-42 | 4 | 2 | type |
| exchange | 1 | 43+ | 9 | 1 | type |
| exchange | 2 | 1-14 | 0 | 4 | type |
| exchange | 2 | 15-42 | 4 | 2 | type |
| exchange | 2 | 43+ | 7 | 4 | type |
| exchange | 3 | 1-14 | 4 | 2 | type |
| exchange | 3 | 15-42 | 6 | 1 | type |
| exchange | 3 | 43+ | 19 | 11 | type |
| exchange | 4 | 1-14 | 1 | 1 | type |
| exchange | 4 | 15-42 | 1 | 1 | type |
| exchange | 4 | 43+ | 7 | 2 | type |
| exchange | 5 | 43+ | 4 | 1 | type |
| exchange | 6 | 15-42 | 1 | 0 | type |
| exchange | 6 | 43+ | 0 | 1 | type |
| play | 1 | 1-14 | 19 | 7 | type+leave |
| play | 1 | 15-42 | 52 | 16 | type+leave+stage |
| play | 1 | 43+ | 61 | 34 | type+leave+stage |
| play | 2 | 1-14 | 56 | 32 | type+leave+stage |
| play | 2 | 15-42 | 111 | 53 | type+leave+stage |
| play | 2 | 43+ | 142 | 66 | type+leave+stage |
| play | 3 | 1-14 | 75 | 39 | type+leave+stage |
| play | 3 | 15-42 | 145 | 66 | type+leave+stage |
| play | 3 | 43+ | 165 | 90 | type+leave+stage |
| play | 4 | 1-14 | 89 | 38 | type+leave+stage |
| play | 4 | 15-42 | 117 | 67 | type+leave+stage |
| play | 4 | 43+ | 161 | 78 | type+leave+stage |
| play | 5 | 1-14 | 51 | 29 | type+leave+stage |
| play | 5 | 15-42 | 69 | 39 | type+leave+stage |
| play | 5 | 43+ | 73 | 38 | type+leave+stage |
| play | 6 | 1-14 | 10 | 4 | type |
| play | 6 | 15-42 | 8 | 5 | type |
| play | 6 | 43+ | 8 | 2 | type |

## Cost of margin 80 versus 20

`infbench:10:20:2026150000:margintiming` compares the production static model
at temperature 5 on identical positions, alternating evaluation order between
the two starting-player games. The optimized single-thread run completed 20
games and 117 paired observations. Both calls reuse one inference context;
first-call initialization is therefore distributed between the two margins.

| Observations | n | Margin 20 total seconds | Margin 80 total seconds | Ratio |
|---|---:|---:|---:|---:|
| All | 117 | 10.265662 | 11.898721 | 1.159 |
| Placements | 113 | 4.307462 | 4.691297 | 1.089 |
| Exchanges | 4 | 5.958200 | 7.207424 | 1.210 |

These are inference times, including posterior finalization, not complete game
times. Sampling covers leave-size/stage cells and is not weighted to their natural
frequency. The four exchanges are too few for a stable exchange timing estimate.
Wider margins admit more racks and reduce cutoff opportunities; census building
itself does not depend on the margin. Production census is currently used when
at least five tiles are unknown, including exchanges.

PAT cannot be substituted into the existing single-score-per-played-multiset
census exactly: placement geometry and candidate-rack-dependent unseen counts
can change which move is best. The subsequent [exact PAT hybrid](pat-inference-hybrid.md)
caches board setup, uses conservative census/PAT bounds to settle easy racks,
and invokes exact PAT movegen for ambiguous racks. It is opt-in for experiments;
the calibration results above were collected with the original exact PAT path.

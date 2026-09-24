# PAT post-Champion-X investigation (2026-09-23)

Worktree `~/sources/magpie-pat-postx`, branch `claude/pat-postx` (from a1d665b5).
Baseline: `hookscore_x.pat` (SHA-256 e38d8692...de8e). CSW21 primary, 10 threads,
`BUILD=no_pgo_release`. Data dirs are symlinks to `magpie-pat-setup-value`.

Binaries:
- `bin/magpie_base`, `bin/magpie_test_base`: unpatched a1d665b5.
- `bin/magpie`, `bin/magpie_test`: + `plain_hook.patch` + `patdecide` harness.
- `bin/magpie_isolive`: plain-hook patch with dead-square-aware isolation (`PAT_ISO_LIVE 1`).
- `bin/magpie_isodbg`: counts geometric vs live isolated hooks (`PAT_ISO_DEBUG`).

Hazard: `bin/magpie` persists `settings.txt` (including `-pat1/-pat2`) in the
working directory. A stale `settings.txt` made later runs load the wrong PAT file.
Delete it or pass every PAT explicitly.

## Decision diagnostic (`patdecide`)

`./bin/magpie_test patdecide:<lex>:<patA>:<patB>:<seed>:<positions>:<worlds>[:shard:nshards]`
(`test/pat_move_choice_test.c`). How it works:

- Positions come from A's self-play, stopped at bag = 1 + seed%86.
- Each model chooses its own move independently.
- For each world, the opponent rack is resampled before either move. The worlds are paired, with world_seed = seed+5e8+w*1000003.
- The continuation is a fixed no-PAT static player.
- Horizon: bag > 21 gets 4 plies, the mover's KLV leave at the horizon, and a winpct lookup. Bag ≤ 21 plays to game end.
- Utility is `sim_utility_blend(win, spread, 1.0, 0.5, 100)`.
- Results are reported as B − A.

Runner: `postx/run_decide.sh` (10 shards), pooler: `postx/pool_decide.py`.

### Hook weight sweep vs X (seed 991000000, 39,119 positions, 16 worlds)

| candidate | disagree | utility pp / disagreement | utility pp / decision |
|---|---|---|---|
| plain x1 (fitted -1/-4) | 0.71% | -0.325 ± 0.227 | -0.0023 ± 0.0016 |
| plain x3 | 2.05% | -0.141 ± 0.127 | -0.0029 ± 0.0026 |
| plain x10 | 6.04% | -0.140 ± 0.073 | -0.0084 ± 0.0044 |
| plain x30 | 14.5% | -0.311 ± 0.050 | -0.0451 ± 0.0072 |
| isolated x1 (fitted -1/-3) | ~0 (0/292 in smoke) | — | ~0 |
| isolated x3 | 0.18% | -0.866 ± 0.422 | -0.0016 ± 0.0008 |
| isolated x10 | 0.59% | -0.301 ± 0.258 | -0.0018 ± 0.0015 |
| isolated x30 | 1.59% | -0.233 ± 0.166 | -0.0037 ± 0.0026 |

No scale of either feature improves decisions. The plain-hook loss grows
monotonically with the weight. The only positive stratum was opening-type
bingo/bingo decisions (plain x1: +0.62 ± 0.35 on 64 cases), which is not
significant after this many strata.

## Isolation misclassification

The prototype's `pat_hook_reaches_premium` counts a route through a dead square
(empty cross set) as reaching a premium. X's `pat_scan_unit` walk stops at dead
squares, so those hooks are priced by neither X nor the isolated channels.

Over 600 games (`magpie_isodbg`, `-pat px_isohook`, seed 7):

- plain hook evaluations: 4,598,442
- geometrically isolated: 240,170 (5.2%)
- isolated when dead squares are respected: 770,340 (16.8%)

So the dead-square-aware definition finds 3.2x as many isolated hooks. The refit
under that definition follows below.

### Refit with the dead-aware definition

Command:
`bin/magpie_isolive patgen 150000 px_isolive_fit -lex CSW21 -gp true -threads 10 -seed 92600003 -wmp true -pat px_isolive_input`
(same seed and input as the original isolated fit).

Raw coefficients: isolated_hook_flex -0.0034, isolated_hook_score -0.0036. In
the report's sign convention, negative means a bonus, not a penalty, so both are
clipped to 0 at every shrinkage, including 0. Validation MSE equals X's
(549.0751). Once isolation is defined consistently with X's walks, isolated
hooks carry no penalty signal. The original -1/-3 fit came from hooks behind dead
squares that were misfiled as reaching a premium. The resulting model is exactly
X.

## Z salvage and seed bagging (whole games vs X, 1M mirrored pairs each, `bin/magpie_base`)

Z is the same recipe as X on a different training seed (Z: seed 1 of a
doubled-data build; X: seed 4). The seed tournament inside the X build had all
seeds within noise of one another (s2 -0.07, s3 +0.0003, s4 -0.025 vs s1, SE ~0.06).

| candidate | seed | spread / pair | 95% CI | divergent pairs | P1 wins / ties | games/s |
|---|---|---|---|---|---|---|
| bag8: mean of X and Z seeds 1-4, X opening rows | 93100001 | -0.1351 ± 0.0482 | [-0.230, -0.041] | 29.7% | 49.77% / 0.41% | 4020 |
| bagx: mean of X seeds 1-4, X opening rows | 93100002 | -0.0840 ± 0.0480 | [-0.178, +0.010] | 29.3% | 49.77% / 0.40% | 4020 |
| X weights + Z opening rows | 93100003 | -0.0026 ± 0.0154 | [-0.033, +0.028] | 2.0% | 49.80% / 0.40% | 4240 |

Averaging seed fits does not beat the selected seed. The averaged model plays
like an average seed, which suggests X's seed is somewhat better than its
siblings. Z's smaller opening penalties are worth nothing measurable (bounded to
±0.03 per pair). Z's own untouched confirmation (+0.035 ± 0.040) is consistent
with zero. Nothing in Z is worth salvaging.

Divergent-pair outcomes (P1 = candidate): bag8 -0.456 ± 0.163 on 296,548
pairs; bagx -0.286 ± 0.164 on 293,427; X+Z-opening -0.131 ± 0.769 on 20,097.

## Isolated-hook candidate (geometric definition, `px_isohook`), whole games vs X

The new batch used `bin/magpie autoplay games 2000000 -pat hookscore_x -lex CSW21
-gp true -threads 10 -wmp true -rit true -ritmmap true -wit true -seed 93200001
-pat1 px_isohook -pat2 hookscore_x`.

| batch | pairs | spread / pair | divergent pairs |
|---|---|---|---|
| prior 92640004 | 500K | +0.0226 ± 0.0115 | 1.27% |
| prior 92650005 | 1M | +0.0066 ± 0.0081 | ~1.3% |
| new 93200001 | 2M | +0.0089 ± 0.0058, CI [-0.0024, +0.0202] | 1.28% (25,649 pairs, +0.694 ± 0.451 each) |
| **pooled (inverse variance)** | 3.5M | **+0.0102 ± 0.0044, CI [+0.0017, +0.0188]** | |

Per game that is +0.005 ± 0.0022 points. P1 wins 49.80% and ties 0.39%. The
upper bound is 0.019 spread per pair (0.009 per game). The lower bound is barely
above zero after several candidates and screens, so it is not decisive. The
mechanism behind it does not survive a consistent definition (see the refit
above). Speed was 3,575 games/s in the mixed match versus about 4,200 for X vs X,
roughly 15% slower for the pair and about 24% for isohook alone. The per-move
full-board scan would need the cached-baseline incremental delta before
promotion. Given the size of the effect, that work is not justified.

## Correctness

`./bin/magpie_test pat` passes on the patched build (`postx/pat_test.log`),
including runtime/training-row and movegen path parity: 40 positions, 1,288
within-margin moves, 800 overlay rows.

## Conclusions

- Champion X is preserved. No post-X candidate wins.
- Plain hooks and isolated hooks make decisions worse at every scale under the
  decision oracle.
- Isolated hooks defined consistently with X's walks fit to zero.
- The geometric isolated model's whole-game edge is bounded to at most +0.019
  spread per pair (+0.009 per game) and costs about 15-24% speed.
- Seed bagging loses (bag8 -0.135 ± 0.048). Z's opening rows are null
  (±0.03 per pair).
- Earlier post-X screens were also null: tile-count +0.036 ± 0.058, learned
  floater +0.017 ± 0.023.

Seed-to-seed variation within the X recipe (0.03-0.1 per pair) is an order of
magnitude larger than any post-X feature effect. That makes training noise and
objective mismatch the binding constraint, not missing features. The fit
minimizes immediate-reply MSE, and none of the post-X channels moved held-out MSE
by more than 0.1.

## Next most promising experiment

Optimize for decision quality rather than reply MSE. Take the highest-variance
X weights across seeds (dd_hook_only SD 78, w9_floater SD 63, dd_floater SD 35,
dd_tiles_saved SD 26) and search them directly:

1. Screen with the paired decision diagnostic (`patdecide`, 40K positions x 16
   worlds).
2. Confirm survivors with 1M-pair whole-game runs on untouched seeds.

A cheaper alternative is a larger seed tournament of the unchanged X recipe (8+
seeds, 1M-pair screens against X). X's edge over the seed average (bagx
-0.084 ± 0.048) suggests the selection step itself carries signal.

## Files

- `postx/postx_plainhook_isolive_harness.patch`: plain_hook.patch plus the
  dead-aware isolation (`PAT_ISO_LIVE`, default 0), debug counters
  (`PAT_ISO_DEBUG`), and the `patdecide` harness, all against a1d665b5.
- `postx/run_decide.sh`, `postx/pool_decide.py`, `postx/queue1.sh`,
  `postx/decide/*`, `postx/games/*.log`, `postx/sweep_hooks.log`.
- PAT files (in the shared `data/strategy`, not committed; they belong in
  MAGPIE-DATA): `px_isohook*.pat`, `px_plainhook*.pat`, `px_bag8.pat`,
  `px_bagx.pat`, `px_bagz.pat`, `px_x_zopen.pat`, `px_isolive_*`.

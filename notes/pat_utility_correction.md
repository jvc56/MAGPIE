# PAT utility correction

A PAT file row `utility_adjust,V` adds a margin- and stage-dependent term to every move's PAT
term:

    0.5 * V * kappa(margin + score, bag after the move)

- `kappa = -U''/U'` of the default sim utility (win 1.0, spread 0.5, scale 100), read off the
  letter distribution's win percentage table (`winpct_<ld>`, `winpct_<ld>_super` on 21x21) with
  the opponent on turn.
- `margin` is the mover's lead before the move.
- `score` is the move's score.

The term is exactly zero without the row, and it is bag-gated like the rest of PAT. The shipped
`<LEX>.pat` and `<LEX>_super21.pat` files carry `utility_adjust,350`.

It is the only PAT term that can be positive.

## What it does

Static equity is calibrated for spread, but the target is the default utility, which is mostly
win probability. Where the utility curve is concave (ahead) or convex (behind), a point banked
now is worth a different amount than a point of leave or PAT equity. The correction's slope in
score is `0.5 * V * d(kappa)/d(margin)`. That slope is positive over the usual range of leads
(about -100 to +100), so it favors banked points over leave value by an amount set by the curve.

Because kappa is read on the row the move's own draw leads to, the term also shifts the value
of tile turnover by margin and stage.

A decomposition at V = 173 (100K pairs each) attributed about 60% of the win-rate gain to the
score-vs-rest reweighting and about 40% to turnover.

The textbook certainty-equivalent reading, with V as extra future variance, has the opposite
sign. V = -173 lost -0.34pp of win share, so do not read V as a variance.

## Implementation

- **Load.** When weights with the row are used with a letter distribution
  (`pat_prepare_utility`), they build `Equity` tables of the correction from `winpct_<ld>`,
  indexed by (bag, margin), plus suffix maxima over margin. A missing table is an error that
  names it.
- **Per position.** Movegen calls `pat_eval_context_set_utility(margin, bag)`.
- **Per move.** Each move gets one table lookup, inside `pat_eval_move_penalty`. Exchanges,
  every record type and WMP get it through that one function.
- **Bounds.** Because this term can be positive, its per-position maximum
  (`pat_eval_utility_bound`) is added to every pruning bound:
  - the lane bound, which feeds the shadow and anchor bounds;
  - the three WMP precomputed-equity skips.

  The per-move bound carries the exact value.
- **Parity test.** `test_pat_path_parity` also runs with V = 2000 to stress every bound.
  Best, exhaustive and within-margin agree, WMP on and off agree, and every move's term stays
  within both bounds.

## Validation

Whole games, CSW21, the same weights with the row vs without it, 1M mirrored pairs each on
untouched seeds:

| V | win share | spread / pair | divergent pairs | approx. utility |
|---|---|---|---|---|
| 120 | +0.163pp | -0.014 ± 0.025 | 17.9% | +0.108pp |
| 173 | +0.203pp | -0.101 ± 0.030 | 24.5% | +0.131pp |
| 250 | +0.254pp | -0.307 ± 0.035 | 33.0% | +0.159pp |
| 300 | +0.306pp | -0.414 ± 0.038 | 38.0% | +0.189pp |
| **350** | **+0.337pp (~9.5 SE)** | **-0.596 ± 0.041** | **42.5%** | **+0.204pp** |
| 400 | +0.347pp | -0.889 ± 0.043 | 46.7% | +0.200pp |
| 450 | +0.318pp | -1.190 ± 0.045 | 50.5% | +0.170pp |
| 600 | +0.333pp | -2.047 ± 0.051 | 59.8% | +0.149pp |
| 800 | +0.271pp | -3.432 ± 0.056 | 69.2% | +0.059pp |

"Approx. utility" is `2/3 * win share + 1/3 * (spread per game / 470)`, the default blend with
the spread sigmoid's slope averaged over typical final spreads.

- **Where it peaks.** Utility is flat across V ≈ 350–400, and win share tops out near 400. At
  350, win share is nearly 400's while the spread given up is two-thirds as much.
- **SE convention.** The SE column in the win share treats games as independent (autoplay's own
  convention). Mirrored pairs make the true SE smaller.

Absolute comparisons (1M pairs each):

| matchup | win share | spread / pair |
|---|---|---|
| PAT + V=250 vs no PAT | 51.058% | +3.90 ± 0.10 |
| PAT vs no PAT (same seed) | 50.767% | +4.10 ± 0.10 |

CSW24 transfer, earlier PAT weights + V=173 vs the same weights, 1M pairs: **+0.189pp** win
share (~5.4 SE), -0.166 ± 0.029 spread per pair.

Speed (10 threads, self-play, median of 3 interleaved rounds):

| player | games/s |
|---|---|
| no PAT | 13,459 |
| PAT | 4,337 |
| PAT + V=250 | 4,203 |

The correction costs about 3% on top of PAT.

## Reproduce

```sh
make magpie BUILD=no_pgo_release
grep -v '^utility_adjust,' data/strategy/CSW21.pat > data/strategy/CSW21_no_utility.pat
./bin/magpie autoplay games 1000000 -lex CSW21 -gp true -threads 10 -wmp true \
  -pat CSW21 -pat1 CSW21 -pat2 CSW21_no_utility -seed 97200004
```

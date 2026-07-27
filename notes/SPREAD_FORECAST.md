# Terminal spread forecasting

## Problem

MAGPIE currently evaluates a nonterminal simulation horizon in two different
ways:

- `winpct.csv` estimates win probability from horizon spread plus leave value
  and the total number of unseen tiles;
- spread/equity statistics use the observed horizon spread plus a leave-value
  residual.

Neither representation models how many turns each player is likely to receive
after the horizon. Two positions with the same total unseen tiles can have
different timing after the bag empties because the players can hold different
numbers of tiles. That makes the horizon spread a poor common scale for
blending win probability with spread, and it would also make static evaluation
a poorly calibrated prior for simulation utility.

The first version deliberately leaves win probability alone. It adds an
independent forecast of terminal spread:

```text
E[S_final | state] = S_current
                   + mean_swing[bag tiles][on-turn rack tiles]
                                           [off-turn rack tiles]
```

The table also stores the expected remaining turn count for both players and
the training sample count. Those fields are diagnostics rather than runtime
inputs: they let us test the proposed timing mechanism directly instead of
inferring it from spread error alone.

Canonicalizing the state around the player on turn means that one table covers
both players. A lookup is one indexed cell load. The standard English table is
about 89 KiB, so its runtime and memory cost should be negligible relative to
move generation.

## Runtime semantics

Load a model explicitly:

```bash
./bin/magpie set -spreadforecast CSW24_spread_v1
```

Use `-spreadforecast none` to disable it. The default is off.

At a nonterminal simulation horizon, the model supplies:

- the residual recorded in the play's equity and leftover statistics; and
- the spread input to blended simulation utility.

The existing `-winpct` calculation intentionally continues to use ordinary
horizon spread plus leave value. Keeping the probability and spread models
separate makes the experiment attributable and avoids silently invalidating
the existing win-percentage calibration. At a terminal horizon, the actual
final spread is used.

The same game-state API can later supply a spread-scale static prior, but this
change does not add that prior. First the forecast itself must improve
held-out prediction and simulation choices.

## Training and file format

FJ autoplay now retains bag-empty positions. Generate a corpus with a frozen
static policy and no RIT-derived leave cache:

```bash
make BUILD=no_pgo_release magpie
mkdir -p obj/spread-forecast-csw24
cd obj/spread-forecast-csw24
ln -s ../../data data
../../bin/magpie autoplay fj 50000 -lex CSW24 -threads 10 \
  -seed 20260727 -pfreq 1000000 -rit false -savesettings false
python3 ../../tools/train_spread_forecast.py \
  --fj-glob 'autoplay_record_fj_*' \
  --output data/strategy/CSW24_spread_v1.sfc
```

The trainer splits whole games deterministically, fits cell means on 80%, and
reports terminal-spread and turn-count errors on the held-out 20%. Sparse rack
states are shrunk toward their bag-count mean. This is important post-bag,
where many exact rack-size pairs are uncommon.

An `.sfc` file contains:

1. eight-byte magic `MAGSPRD\0`;
2. four little-endian `uint32_t` values: version, maximum bag size, maximum
   rack size, and cell count;
3. one 16-byte record per state: `float32` expected spread swing, `float32`
   expected on-turn turns, `float32` expected off-turn turns, and a
   little-endian `uint32_t` sample count.

## Initial check and limitations

A fresh 50,000-game CSW24 static corpus contains 1,105,546 positions,
including 128,318 bag-empty positions. The held-out fifth contained 219,541
positions:

| Stratum | Positions | Forecast MAE | Current-spread MAE | Forecast RMSE | Current-spread RMSE |
| --- | ---: | ---: | ---: | ---: | ---: |
| All | 219,541 | **54.221** | 57.393 | **72.460** | 75.539 |
| Bag empty | 25,601 | **17.446** | 24.555 | **26.442** | 34.335 |
| Bag 1–7 | 17,051 | **33.640** | 37.637 | **44.164** | 47.990 |
| Bag 8–20 | 30,301 | **42.371** | 45.734 | **53.751** | 57.686 |
| Bag 21+ | 146,588 | **65.487** | 67.835 | **83.170** | 85.932 |

Overall terminal-spread MAE falls by 3.172 points (5.5%). The bag-empty
reduction is 7.109 points (29.0%), which is the expected signature if separate
rack sizes are capturing end-of-game turn timing. On the same holdout, the
expected-turn MAE is 0.581 turns for the player on turn and 0.589 for the
other player; in bag-empty positions it is 0.294 and 0.354 turns.

This is evidence that phase/timing contains signal, not a playing-strength
result. Version 1 omits rack composition, board openness, score-dependent
strategy, and position-specific scoring rates. While the bag is nonempty,
both racks are normally full, so the model is principally a bag-count
forecast; the separate rack sizes become informative at bag exhaustion.

The lookup was also checked with nine interleaved simbench pairs using
no-PGO release builds, 10 threads, RIT on, a fixed trajectory, and 0.2-second
turn limits. Forecast-on throughput divided by forecast-off throughput had a
median of 0.9989; the mean of the middle three ratios was 0.9978 (-0.22%).
That is below this protocol's roughly 1% resolution, so no runtime cost is
currently measurable.

Validation should proceed in this order:

1. regenerate a corpus that includes bag-empty positions and report held-out
   spread and turn calibration by bag/rack stratum;
2. compare equal-position, equal-sample simulations with the forecast off and
   on;
3. oracle only the positions where their candidate rankings differ;
4. add static evaluation as a prior only if the forecast improves those
   decisions, and calibrate the prior separately.

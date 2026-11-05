# Initial inference benchmark — 2026-11-02

Exploratory run of `infbench:20:2:20261102`, using the optimized
`BUILD=no_pgo_release` binary on macOS. See [benchmark definitions](infbench.md)
for the exact sampling, player budgets, prior mixture, and metrics.

Twenty paired game prefixes per policy produced 187 observed leaves: 60 static,
62 PAT, and 65 two-ply. All were placements; this run provides no exchange-quality
evidence. A separate [exchange-focused run](infbench-exchange-results-20261102.md)
measures discarded-tile prediction. Leave sizes ranged from one to six tiles. There were 14, 13, and 15
preendgame observations (bag size 1–6), respectively. Requested second observations
were sometimes unavailable before the bag emptied. Seeds and the mode grid were
fixed before this run; no parameters were optimized. The comparisons below are
exploratory and need confirmation on held-out seeds before changing defaults.

## Findings

- For static players, hard margin zero had the best log loss in this grid.
  Softmax recovered much of the information lost by widening the hard margin.
- For PAT players, margin 10 / temperature 2 reduced mixed log loss from 4.967
  to 4.353 nats and KLV MSE from 82.063 to 72.804 versus hard margin 10.
  Hard margin zero excluded 12.9% of actual leaves.
- For two-ply players, hard margin zero excluded 67.7% of actual leaves.
  Margin 10 / temperature 2 did not clearly improve log loss over hard margin 10.
  Margin 20 / temperature 5 reduced mixed log loss from 5.939 to 5.709 nats,
  and KLV MSE from 110.402 to 99.811 versus hard margin 20.
- Predictions of different features can disagree: a lower KLV MSE does not
  guarantee a lower vowel-count MSE. The benchmark reports them separately.

## Full mode grid

NLL is the declared 1% prior-mixture log loss, in nats. Feature MSE uses the raw
posterior mean, with prior fallback only for an empty posterior. KLV and tile-score
MSE are in points squared. Tile MSE is the sum across tile types. Lower is better.
“Excluded” counts zero probability before smoothing; raw NLL is infinite whenever
this rate is nonzero.

### static

| Mode | NLL | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Excluded | Empty |
|---|---:|---:|---:|---:|---:|---:|---:|
| prior | 5.123 | 107.986 | 0.558 | 5.894 | 2.052 | 0.0% | 0.0% |
| hard0 | 3.561 | 67.771 | 0.253 | 4.699 | 1.757 | 0.0% | 0.0% |
| hard5 | 4.351 | 86.436 | 0.332 | 4.778 | 1.904 | 0.0% | 0.0% |
| hard10 | 4.650 | 105.194 | 0.399 | 5.147 | 1.953 | 0.0% | 0.0% |
| hard20 | 4.883 | 119.195 | 0.470 | 5.983 | 2.013 | 0.0% | 0.0% |
| soft5t2 | 3.933 | 75.562 | 0.283 | 4.667 | 1.809 | 0.0% | 0.0% |
| soft5t5 | 4.139 | 81.291 | 0.306 | 4.719 | 1.855 | 0.0% | 0.0% |
| soft10t2 | 3.958 | 77.404 | 0.288 | 4.684 | 1.813 | 0.0% | 0.0% |
| soft10t5 | 4.256 | 90.179 | 0.330 | 4.847 | 1.875 | 0.0% | 0.0% |
| soft20t2 | 3.960 | 77.630 | 0.289 | 4.689 | 1.814 | 0.0% | 0.0% |
| soft20t5 | 4.294 | 94.015 | 0.343 | 4.975 | 1.886 | 0.0% | 0.0% |

### pat

| Mode | NLL | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Excluded | Empty |
|---|---:|---:|---:|---:|---:|---:|---:|
| prior | 5.479 | 96.164 | 0.575 | 7.125 | 2.261 | 0.0% | 0.0% |
| hard0 | 4.653 | 70.153 | 0.332 | 6.420 | 1.962 | 12.9% | 3.2% |
| hard5 | 4.683 | 76.357 | 0.317 | 5.413 | 2.052 | 1.6% | 0.0% |
| hard10 | 4.967 | 82.063 | 0.382 | 6.360 | 2.124 | 0.0% | 0.0% |
| hard20 | 5.273 | 94.802 | 0.486 | 6.943 | 2.218 | 0.0% | 0.0% |
| soft5t2 | 4.395 | 72.669 | 0.287 | 5.396 | 1.968 | 1.6% | 0.0% |
| soft5t5 | 4.529 | 74.503 | 0.300 | 5.375 | 2.008 | 1.6% | 0.0% |
| soft10t2 | 4.353 | 72.804 | 0.294 | 5.450 | 1.972 | 0.0% | 0.0% |
| soft10t5 | 4.587 | 76.236 | 0.326 | 5.746 | 2.034 | 0.0% | 0.0% |
| soft20t2 | 4.356 | 72.880 | 0.295 | 5.452 | 1.974 | 0.0% | 0.0% |
| soft20t5 | 4.644 | 78.831 | 0.346 | 5.820 | 2.054 | 0.0% | 0.0% |

### sim2

| Mode | NLL | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Excluded | Empty |
|---|---:|---:|---:|---:|---:|---:|---:|
| prior | 5.987 | 110.019 | 0.577 | 6.885 | 2.435 | 0.0% | 0.0% |
| hard0 | 7.560 | 103.511 | 0.593 | 6.463 | 2.386 | 67.7% | 21.5% |
| hard5 | 6.777 | 95.259 | 0.506 | 5.663 | 2.328 | 35.4% | 1.5% |
| hard10 | 6.124 | 101.126 | 0.476 | 5.535 | 2.324 | 16.9% | 0.0% |
| hard20 | 5.939 | 110.402 | 0.511 | 6.571 | 2.389 | 4.6% | 0.0% |
| soft5t2 | 6.785 | 95.715 | 0.556 | 5.752 | 2.346 | 35.4% | 1.5% |
| soft5t5 | 6.749 | 95.297 | 0.527 | 5.703 | 2.331 | 35.4% | 1.5% |
| soft10t2 | 6.171 | 96.233 | 0.527 | 5.715 | 2.304 | 16.9% | 0.0% |
| soft10t5 | 6.024 | 98.092 | 0.489 | 5.564 | 2.296 | 16.9% | 0.0% |
| soft20t2 | 6.044 | 96.084 | 0.526 | 5.710 | 2.303 | 4.6% | 0.0% |
| soft20t5 | 5.709 | 99.811 | 0.486 | 5.671 | 2.299 | 4.6% | 0.0% |

## Paired differences and uncertainty

Differences average within each pair first; intervals are approximate normal 95%
intervals across the 20 pairs. Negative values favor softmax. They are not adjusted
for multiple comparisons. Because pairs can supply unequal numbers of observations,
these means need not equal the differences of the observation-weighted tables above.

| Policy | Comparison | Metric | Mean difference | 95% interval |
|---|---|---|---:|---:|
| static | soft10t2 − hard10 | mixed_nll | -0.681 | [-0.800, -0.562] |
| static | soft10t2 − hard10 | klv_mse | -30.268 | [-50.164, -10.371] |
| static | soft10t2 − hard10 | vowel_mse | -0.098 | [-0.144, -0.052] |
| static | soft10t2 − hard10 | score_mse | -0.381 | [-1.168, 0.407] |
| static | soft10t2 − hard10 | tile_mse | -0.125 | [-0.178, -0.072] |
| pat | soft10t2 − hard10 | mixed_nll | -0.603 | [-0.788, -0.419] |
| pat | soft10t2 − hard10 | klv_mse | -7.813 | [-16.722, 1.096] |
| pat | soft10t2 − hard10 | vowel_mse | -0.074 | [-0.133, -0.015] |
| pat | soft10t2 − hard10 | score_mse | -0.820 | [-1.893, 0.253] |
| pat | soft10t2 − hard10 | tile_mse | -0.146 | [-0.214, -0.078] |
| sim2 | soft20t5 − hard20 | mixed_nll | -0.277 | [-0.505, -0.048] |
| sim2 | soft20t5 − hard20 | klv_mse | -9.387 | [-17.320, -1.454] |
| sim2 | soft20t5 − hard20 | vowel_mse | -0.024 | [-0.098, 0.049] |
| sim2 | soft20t5 − hard20 | score_mse | -0.947 | [-1.446, -0.448] |
| sim2 | soft20t5 − hard20 | tile_mse | -0.086 | [-0.147, -0.025] |

## Per-tile count MSE

Each cell gives hard → softmax MSE. Static/PAT use margin 10, temperature 2;
two-ply uses margin 20, temperature 5. Blanks are shown as `?`.

| Tile | Static | PAT | Two-ply |
|---|---:|---:|---:|
| ? | 0.0804 → 0.0711 | 0.0918 → 0.0853 | 0.0504 → 0.0515 |
| A | 0.1845 → 0.1680 | 0.2413 → 0.2198 | 0.1935 → 0.2025 |
| B | 0.0293 → 0.0293 | 0.0319 → 0.0272 | 0.0264 → 0.0222 |
| C | 0.0131 → 0.0111 | 0.0314 → 0.0293 | 0.0530 → 0.0543 |
| D | 0.0800 → 0.0745 | 0.0715 → 0.0607 | 0.0979 → 0.0954 |
| E | 0.2248 → 0.2236 | 0.2574 → 0.2529 | 0.2261 → 0.2138 |
| F | 0.0226 → 0.0203 | 0.0301 → 0.0288 | 0.0579 → 0.0562 |
| G | 0.0226 → 0.0194 | 0.0749 → 0.0770 | 0.0764 → 0.0780 |
| H | 0.0423 → 0.0415 | 0.0181 → 0.0156 | 0.0534 → 0.0465 |
| I | 0.1419 → 0.1296 | 0.1562 → 0.1380 | 0.1742 → 0.1555 |
| J | 0.0149 → 0.0151 | 0.0020 → 0.0021 | 0.0013 → 0.0009 |
| K | 0.0011 → 0.0009 | 0.0173 → 0.0126 | 0.0096 → 0.0069 |
| L | 0.0868 → 0.0807 | 0.0892 → 0.0801 | 0.1212 → 0.1148 |
| M | 0.0058 → 0.0037 | 0.0222 → 0.0183 | 0.0458 → 0.0468 |
| N | 0.1337 → 0.1249 | 0.1278 → 0.1293 | 0.1898 → 0.1906 |
| O | 0.1431 → 0.1356 | 0.1308 → 0.1153 | 0.1347 → 0.1330 |
| P | 0.0124 → 0.0100 | 0.0984 → 0.0914 | 0.0341 → 0.0308 |
| Q | 0.0050 → 0.0043 | 0.0367 → 0.0309 | 0.0325 → 0.0316 |
| R | 0.1748 → 0.1568 | 0.1372 → 0.1193 | 0.2522 → 0.2401 |
| S | 0.1327 → 0.1275 | 0.1194 → 0.1114 | 0.1454 → 0.1426 |
| T | 0.2206 → 0.2012 | 0.1331 → 0.1310 | 0.2446 → 0.2336 |
| U | 0.0575 → 0.0522 | 0.1152 → 0.1073 | 0.0532 → 0.0458 |
| V | 0.0114 → 0.0107 | 0.0300 → 0.0292 | 0.0457 → 0.0411 |
| W | 0.0432 → 0.0414 | 0.0129 → 0.0138 | 0.0175 → 0.0182 |
| X | 0.0146 → 0.0149 | 0.0138 → 0.0140 | 0.0045 → 0.0047 |
| Y | 0.0296 → 0.0201 | 0.0154 → 0.0149 | 0.0180 → 0.0151 |
| Z | 0.0246 → 0.0242 | 0.0182 → 0.0170 | 0.0294 → 0.0261 |

## Reproduction

```sh
make magpie_test BUILD=no_pgo_release
./bin/magpie_test infbench:20:2:20261102 > infbench.log
rg '^INFBENCH (settings|summary|delta|tile)' infbench.log
```

Input SHA-256 fingerprints:

- `data/lexica/CSW21.kwg`: `3def63951c55eef62e7d480b3f0222b2fc4cad3fe5f82b961d51aae5ed74cc9f`
- `data/lexica/CSW21.klv2`: `d33c30e637d53ee6250fac943a01c3c9a18f93c638499ff5e13fa358c2ca03bb`
- `data/strategy/CSW21.pat`: `f36fc8921b78015e56db64254bfec129b425ce8060a8c7c4295d169a3a714ae8`

The sanitizer scoring test and six-observation smoke run passed. All 66
decision/mode scores in that smoke run agreed with the optimized binary to
1e-8 absolute/relative tolerance, excluding timing. This is validation of the
measurement code; the exploratory sample above is the quality evidence.

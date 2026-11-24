# Exchanged-tile prediction benchmark — 2026-11-02

Exploratory optimized run of `infbench:20:20:20261102:exchanges`, with the same
CSW21 inputs and fixed two-ply budget as the [initial kept-leave report](infbench-results-20261102.md).
See [metric definitions](infbench.md#exchanged-tile-accuracy). The later
[census optimization](census-inference-performance.md) reproduces these metrics
within `1e-8`, including opening exchanges.
Each policy played 20 game pairs, retaining natural exchanges by player 0
and discarding non-exchange turns. The quota was 20 exchanges per game; no game
reached it, so all exchanges before the bag emptied or game ended were scored.
Games without an exchange contribute no observation and are not replaced.
Exchanges are not forced. The sample describes exchanges along these seeded
policy trajectories, not all moves.

## Sample and limits

| Policy | Exchanges | Pairs with an exchange | Games without an exchange |
|---|---:|---:|---:|
| static | 6 | 5 | 34 |
| pat | 6 | 4 | 34 |
| sim2 | 8 | 6 | 33 |

These small samples do not establish a preferred mode. Policies encounter
different positions, so compare inference modes within a policy rather than
ranking policies by their absolute errors. Exact exchanged-set log loss and
exclusion rates remain unreported: the existing stored exchange statistics
contain feature marginals, not a complete joint distribution over discarded
multisets. No independence approximation is used.

## Findings

At margin 10 / temperature 2, softmax reduces discarded-tile KLV MSE versus the
matching hard margin for static (103.015 → 70.235), PAT (133.787 → 93.070), and
two-ply (110.704 → 81.470). Vowel-count MSE worsens for all three; face-score MSE
worsens for static and changes little for PAT and two-ply. Thus the KLV improvement
does not indicate uniformly better tile predictions. Several inference modes
also have worse vowel-count or face-score MSE than the uninformed prior.

## Full mode grid

All values below score the **discarded tiles**, not the retained leave or the
rack after drawing replacements. Lower is better. KLV and face-score MSE are
in points squared; vowel MSE is in count squared. Tile MSE sums the per-type
count MSE, including blanks. Predictions are raw posterior means, with the
uninformed prior used only when the posterior is empty. KLV is the table value
of the discarded multiset, not its value to the player.

### static

| Mode | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Empty rate |
|---|---:|---:|---:|---:|---:|
| prior | 814.129 | 4.517 | 36.294 | 6.953 | 0.000 |
| hard0 | 60.070 | 5.859 | 63.157 | 6.494 | 0.000 |
| hard5 | 73.081 | 5.342 | 47.973 | 6.306 | 0.000 |
| hard10 | 103.015 | 4.824 | 47.481 | 6.238 | 0.000 |
| hard20 | 192.171 | 4.197 | 36.108 | 6.023 | 0.000 |
| soft5t2 | 67.556 | 5.487 | 50.887 | 6.350 | 0.000 |
| soft5t5 | 70.421 | 5.392 | 48.982 | 6.316 | 0.000 |
| soft10t2 | 70.235 | 5.391 | 50.298 | 6.334 | 0.000 |
| soft10t5 | 84.180 | 5.077 | 48.073 | 6.275 | 0.000 |
| soft20t2 | 71.092 | 5.364 | 50.048 | 6.326 | 0.000 |
| soft20t5 | 108.993 | 4.735 | 43.599 | 6.172 | 0.000 |

### pat

| Mode | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Empty rate |
|---|---:|---:|---:|---:|---:|
| prior | 684.310 | 3.531 | 28.138 | 5.036 | 0.000 |
| hard0 | 78.641 | 4.851 | 34.139 | 4.772 | 0.000 |
| hard5 | 96.805 | 4.147 | 25.697 | 4.567 | 0.000 |
| hard10 | 133.787 | 3.742 | 26.504 | 4.499 | 0.000 |
| hard20 | 208.664 | 3.272 | 22.694 | 4.349 | 0.000 |
| soft5t2 | 88.784 | 4.284 | 26.735 | 4.637 | 0.000 |
| soft5t5 | 93.060 | 4.187 | 25.898 | 4.589 | 0.000 |
| soft10t2 | 93.070 | 4.199 | 26.481 | 4.613 | 0.000 |
| soft10t5 | 111.712 | 3.929 | 25.938 | 4.535 | 0.000 |
| soft20t2 | 94.006 | 4.177 | 26.415 | 4.606 | 0.000 |
| soft20t5 | 134.044 | 3.667 | 24.697 | 4.453 | 0.000 |

### sim2

| Mode | KLV MSE | Vowel MSE | Score MSE | Tile MSE | Empty rate |
|---|---:|---:|---:|---:|---:|
| prior | 731.500 | 4.628 | 25.346 | 6.404 | 0.000 |
| hard0 | 77.667 | 5.818 | 49.281 | 5.496 | 0.000 |
| hard5 | 83.612 | 5.390 | 40.211 | 5.495 | 0.000 |
| hard10 | 110.704 | 4.871 | 40.332 | 5.486 | 0.000 |
| hard20 | 179.576 | 4.333 | 32.960 | 5.470 | 0.000 |
| soft5t2 | 80.599 | 5.541 | 40.376 | 5.473 | 0.000 |
| soft5t5 | 81.758 | 5.448 | 40.180 | 5.479 | 0.000 |
| soft10t2 | 81.470 | 5.460 | 40.272 | 5.466 | 0.000 |
| soft10t5 | 93.304 | 5.152 | 40.118 | 5.491 | 0.000 |
| soft20t2 | 81.739 | 5.440 | 40.168 | 5.468 | 0.000 |
| soft20t5 | 111.233 | 4.846 | 37.658 | 5.483 | 0.000 |

## Paired comparisons

Selected comparisons specified by the same margin/temperature grid as the
kept-leave report. Entries are mean MSE difference ± approximate 95% half-width
(1.96 standard errors across game pairs containing exchanges). Negative favors
softmax. Each pair receives equal weight, so these means can differ from the
differences between observation-weighted tables above. With so few pairs these
normal intervals are descriptive, not strong significance evidence.

| Policy | Softmax vs matching hard margin | KLV Δ | Vowel Δ | Score Δ | Tile Δ |
|---|---|---:|---:|---:|---:|
| static | soft10t2 vs hard10 | -34.215 ± 46.115 | 0.371 ± 0.685 | 2.479 ± 9.498 | 0.033 ± 0.231 |
| static | soft20t5 vs hard20 | -79.598 ± 54.164 | 0.445 ± 0.534 | 7.075 ± 9.261 | 0.126 ± 0.134 |
| pat | soft10t2 vs hard10 | -46.299 ± 54.182 | 0.293 ± 0.835 | -1.251 ± 6.728 | 0.054 ± 0.282 |
| pat | soft20t5 vs hard20 | -77.582 ± 74.190 | 0.371 ± 0.547 | 1.460 ± 5.509 | 0.089 ± 0.127 |
| sim2 | soft10t2 vs hard10 | -27.633 ± 19.868 | 0.556 ± 0.895 | 0.165 ± 2.731 | 0.020 ± 0.447 |
| sim2 | soft20t5 vs hard20 | -62.840 ± 15.821 | 0.374 ± 0.977 | 4.598 ± 5.260 | -0.041 ± 0.471 |

## Per-tile count MSE

Hard margin 10 → softmax margin 10 / temperature 2; blanks are `?`.

| Tile | static | pat | sim2 |
|---|---:|---:|---:|
| ? | 0.0000 → 0.0000 | 0.0000 → 0.0000 | 0.0000 → 0.0000 |
| A | 0.0514 → 0.0314 | 0.0532 → 0.0324 | 0.1174 → 0.1096 |
| B | 0.2536 → 0.2556 | 0.2380 → 0.2364 | 0.2593 → 0.2465 |
| C | 0.0170 → 0.0183 | 0.0168 → 0.0181 | 0.0086 → 0.0102 |
| D | 0.0399 → 0.0564 | 0.0309 → 0.0431 | 0.3017 → 0.2917 |
| E | 0.0619 → 0.0584 | 0.0684 → 0.0665 | 0.1318 → 0.1534 |
| F | 0.2603 → 0.2795 | 0.2377 → 0.2529 | 0.1445 → 0.1486 |
| G | 0.1330 → 0.1311 | 0.0352 → 0.0392 | 0.1071 → 0.1067 |
| H | 0.0024 → 0.0014 | 0.0011 → 0.0008 | 0.0063 → 0.0048 |
| I | 0.5769 → 0.6188 | 0.4159 → 0.4801 | 0.7144 → 0.6556 |
| J | 0.0624 → 0.0569 | 0.0514 → 0.0452 | 0.1151 → 0.1110 |
| K | 0.0018 → 0.0019 | 0.0018 → 0.0019 | 0.0008 → 0.0013 |
| L | 0.1755 → 0.1589 | 0.1537 → 0.1377 | 0.4140 → 0.3929 |
| M | 0.1379 → 0.1388 | 0.1362 → 0.1370 | 0.0168 → 0.0186 |
| N | 0.1464 → 0.1591 | 0.0713 → 0.0690 | 0.1059 → 0.1365 |
| O | 2.1419 → 2.2110 | 1.1953 → 1.2407 | 1.6194 → 1.6636 |
| P | 0.0405 → 0.0449 | 0.0110 → 0.0076 | 0.1254 → 0.1202 |
| Q | 0.0820 → 0.0982 | 0.0255 → 0.0328 | 0.0813 → 0.0923 |
| R | 0.1364 → 0.1551 | 0.1276 → 0.1512 | 0.0618 → 0.0681 |
| S | 0.0005 → 0.0002 | 0.0005 → 0.0002 | 0.0004 → 0.0002 |
| T | 1.0086 → 0.9911 | 0.1611 → 0.1715 | 0.1144 → 0.1135 |
| U | 0.1608 → 0.1536 | 0.5989 → 0.6108 | 0.4589 → 0.4676 |
| V | 0.5772 → 0.5531 | 0.5732 → 0.5483 | 0.4496 → 0.4235 |
| W | 0.1658 → 0.1570 | 0.2920 → 0.2877 | 0.1212 → 0.1259 |
| X | 0.0024 → 0.0025 | 0.0012 → 0.0016 | 0.0028 → 0.0016 |
| Y | 0.0010 → 0.0007 | 0.0009 → 0.0006 | 0.0063 → 0.0019 |
| Z | 0.0000 → 0.0000 | 0.0000 → 0.0000 | 0.0002 → 0.0000 |

## Reproduce

```sh
make magpie_test BUILD=no_pgo_release
./bin/magpie_test infbench:20:20:20261102:exchanges > exchanges.log
rg '^INFBENCH exchanged_' exchanges.log
```

The log also contains kept-leave scores on the same exchanges, actual discarded
multisets, and pre-move CGPs. Input fingerprints are unchanged from the linked
initial report. No production inference behavior changed to collect these metrics.

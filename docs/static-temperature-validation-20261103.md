# Static-only inference temperature validation — 2026-11-03

Production inference defaults to static evaluation, T8, and an effectively unlimited (10,000-point) margin. Experimental PAT inference and its dedicated board-cache/movegen interface have been removed. Neural inference was external experimental code, never a production integration. General PAT move generation remains available.

## Protocol

Fresh NWL23 games and explicitly selected NWL23 leave tables: 324 game pairs for one tile, 108 for each of two through five tiles, and 432 for six tiles and exchanges. Seeds share a common prefix across sizes, alternating the starter. Both players’ loaded leave-table names are asserted before play. Player 0 chooses from 64 static candidates using 4,000 two-ply simulation iterations; player 1 is static. PAT and inference are disabled in game generation. Every qualifying player-0 placement before the bag empties is sampled (quota 20 per game). Seven hidden tiles means natural exchanges, with kept-leave and exchanged-tile metrics reported separately.

Temperatures 3, 5, 7, 8, 9, 10, and 15 run production static inference on each identical observation, with a fresh context and rotated order. One inference thread per job, WMP enabled, production census enabled. Eighteen independent jobs run concurrently; these are wall-time latencies under that load, not isolated-machine latencies. Timing covers the full `infer` call, excluding benchmark scoring/prior enumeration and context destruction. P95 uses nearest rank.

NLL is in nats, lower is better, with the existing 1% prior mixture. MSE uses the raw posterior mean, falling back to the prior only for an empty posterior. Duplicate count means copies beyond the first of each tile type. Confidence intervals cluster paired observation differences by game pair; they are nominal, without multiple-comparison adjustment. This is a temperature comparison, not a match-win-rate experiment.

## Fresh results

| Unknown tiles | Observations | T3 NLL | T5 | T7 | T8 | T9 | T10 | T15 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 380 | 2.46370 | 2.42106 | 2.41631 | 2.41766 | 2.42042 | 2.42422 | 2.45074 |
| 2 | 356 | 4.68200 | 4.53483 | 4.49796 | 4.49196 | 4.48976 | 4.48995 | 4.50553 |
| 3 | 497 | 6.29171 | 6.10922 | 6.08108 | 6.08303 | 6.08907 | 6.09712 | 6.14190 |
| 4 | 514 | 7.59443 | 7.40822 | 7.39115 | 7.39870 | 7.40989 | 7.42263 | 7.48534 |
| 5 | 281 | 8.36017 | 8.09122 | 8.03921 | 8.03528 | 8.03753 | 8.04323 | 8.08638 |
| 6 | 237 | 7.96542 | 7.58958 | 7.53444 | 7.53794 | 7.54887 | 7.56328 | 7.64113 |
| 7 (exchange) | 141 | 5.48030 | 5.52368 | 5.58835 | 5.61604 | 5.63981 | 5.66000 | 5.72411 |

## T8 timing

| Unknown tiles | Average (ms) | P95 (ms) | Max (ms) |
|---|---:|---:|---:|
| 1 | 0.388 | 1.067 | 4.121 |
| 2 | 3.888 | 8.246 | 17.774 |
| 3 | 28.191 | 60.372 | 162.672 |
| 4 | 64.900 | 151.109 | 229.087 |
| 5 | 87.241 | 221.847 | 298.715 |
| 6 | 137.819 | 523.559 | 730.838 |
| 7 | 2039.576 | 5003.429 | 5510.085 |

## T8 feature errors

| Unknown tiles | KLV MSE | Vowel-count MSE | Tile-score MSE | Tile-count MSE | Duplicate-count MSE |
|---|---:|---:|---:|---:|---:|
| 1 | 14.207373 | 0.215951 | 3.599638 | 0.883710 | 0.000000 |
| 2 | 44.178382 | 0.416927 | 6.217492 | 1.740259 | 0.045488 |
| 3 | 107.077583 | 0.594838 | 11.083509 | 2.466651 | 0.112121 |
| 4 | 153.468793 | 0.714570 | 11.159714 | 3.122918 | 0.191168 |
| 5 | 215.106722 | 0.720123 | 11.144609 | 3.490820 | 0.294834 |
| 6 | 297.234158 | 0.551253 | 12.908915 | 3.280806 | 0.368648 |
| 7 | 116.794952 | 0.394249 | 5.944854 | 2.083174 | 0.040867 |

## Exchanged-tile feature errors

These score the hidden discarded tiles. The NLL in the main table scores the kept leave, not the whole pre-exchange rack. Joint exchanged-set NLL is not available.

| T | KLV MSE | Vowel-count MSE | Tile-score MSE | Tile-count MSE | Duplicate-count MSE |
|---|---:|---:|---:|---:|---:|
| 3 | 30.910726 | 2.299938 | 21.191549 | 4.093326 | 0.451121 |
| 5 | 27.396414 | 2.321489 | 22.314466 | 4.126772 | 0.427750 |
| 7 | 33.986662 | 2.348286 | 22.964598 | 4.180448 | 0.444430 |
| 8 | 38.504441 | 2.361191 | 23.171803 | 4.204714 | 0.457387 |
| 9 | 43.045355 | 2.372920 | 23.330943 | 4.226150 | 0.470501 |
| 10 | 47.374072 | 2.383381 | 23.456480 | 4.244885 | 0.482962 |
| 15 | 64.383746 | 2.420679 | 23.828332 | 4.309348 | 0.530895 |

## Paired temperature comparisons

Intervals below are for NLL(T) − NLL(T8); positive means T8 is better.

| Unknown tiles | T | Difference | 95% CI |
|---|---:|---:|---|
| 1 | 3 | +0.046040 | [+0.000532, +0.091549] |
| 1 | 5 | +0.003397 | [-0.016841, +0.023634] |
| 1 | 7 | -0.001349 | [-0.007044, +0.004346] |
| 1 | 9 | +0.002761 | [-0.002127, +0.007648] |
| 1 | 10 | +0.006562 | [-0.002503, +0.015627] |
| 1 | 15 | +0.033081 | [+0.010311, +0.055852] |
| 2 | 3 | +0.190044 | [+0.112490, +0.267599] |
| 2 | 5 | +0.042872 | [+0.011938, +0.073806] |
| 2 | 7 | +0.005996 | [-0.001825, +0.013817] |
| 2 | 9 | -0.002196 | [-0.008576, +0.004183] |
| 2 | 10 | -0.002013 | [-0.013693, +0.009667] |
| 2 | 15 | +0.013572 | [-0.014858, +0.042002] |
| 3 | 3 | +0.208680 | [+0.137612, +0.279747] |
| 3 | 5 | +0.026187 | [-0.001469, +0.053844] |
| 3 | 7 | -0.001950 | [-0.008696, +0.004796] |
| 3 | 9 | +0.006037 | [+0.000734, +0.011341] |
| 3 | 10 | +0.014086 | [+0.004504, +0.023667] |
| 3 | 15 | +0.058863 | [+0.036128, +0.081599] |
| 4 | 3 | +0.195723 | [+0.102442, +0.289004] |
| 4 | 5 | +0.009518 | [-0.029816, +0.048853] |
| 4 | 7 | -0.007554 | [-0.017661, +0.002553] |
| 4 | 9 | +0.011189 | [+0.002945, +0.019433] |
| 4 | 10 | +0.023930 | [+0.008858, +0.039003] |
| 4 | 15 | +0.086637 | [+0.050085, +0.123189] |
| 5 | 3 | +0.324888 | [+0.175116, +0.474661] |
| 5 | 5 | +0.055934 | [-0.008983, +0.120851] |
| 5 | 7 | +0.003925 | [-0.013028, +0.020878] |
| 5 | 9 | +0.002248 | [-0.011650, +0.016145] |
| 5 | 10 | +0.007947 | [-0.017491, +0.033384] |
| 5 | 15 | +0.051096 | [-0.010694, +0.112887] |
| 6 | 3 | +0.427482 | [+0.238381, +0.616582] |
| 6 | 5 | +0.051639 | [-0.031299, +0.134577] |
| 6 | 7 | -0.003500 | [-0.024828, +0.017828] |
| 6 | 9 | +0.010928 | [-0.006347, +0.028203] |
| 6 | 10 | +0.025343 | [-0.006165, +0.056851] |
| 6 | 15 | +0.103187 | [+0.026728, +0.179646] |
| 7 | 3 | -0.135737 | [-0.231030, -0.040444] |
| 7 | 5 | -0.092361 | [-0.133917, -0.050804] |
| 7 | 7 | -0.027692 | [-0.038318, -0.017066] |
| 7 | 9 | +0.023771 | [+0.015213, +0.032330] |
| 7 | 10 | +0.043959 | [+0.028362, +0.059556] |
| 7 | 15 | +0.108074 | [+0.069879, +0.146270] |

## Correction to earlier experiments

The earlier exporters switched lexicon/PAT but retained CSW21 KLV. This also affected the initial fresh run; those results are superseded here. A retrospective static-only sweep of the older 1,148-leaf NWL23-word / CSW21-KLV 64-candidate/4,000-iteration corpus gives T8 2.411530 nats, T9 2.411204, and T10 2.413078. T9−T8 = −0.000326 (95% CI −0.003408 to +0.002756). T3, T5, and T15 are worse than T8 with nominal 95% confidence. These are reused observations, not additional fresh validation games.

## Reproduction

```sh
make magpie_test BUILD=no_pgo_release
python3 scripts/run_static_temperature.py --output data/benchmarks/static-temperature-20261103/nwl23 --workers 18 --sizes 1 2 3 4 5 6 7 --pairs-per-shard 6 --size-pairs 1:18 6:24 7:24 --seed 2026330000
```

Raw logs, frozen benchmark binary, and JSON summaries are in `data/benchmarks/static-temperature-20261103/`. Per-temperature timing and all six quality metrics are retained in each `sizeN-results.json`.

## Feature errors at every temperature

Temperature comparisons above use NLL as the primary metric; a different temperature can minimize a particular feature MSE.

| Unknown tiles | T | KLV MSE | Vowel-count MSE | Tile-score MSE | Tile-count MSE | Duplicate-count MSE |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 14.437092 | 0.222240 | 3.593871 | 0.894685 | 0.000000 |
| 1 | 5 | 14.229560 | 0.217276 | 3.579074 | 0.884888 | 0.000000 |
| 1 | 7 | 14.204285 | 0.216087 | 3.590277 | 0.883547 | 0.000000 |
| 1 | 8 | 14.207373 | 0.215951 | 3.599638 | 0.883710 | 0.000000 |
| 1 | 9 | 14.214961 | 0.215963 | 3.609843 | 0.884090 | 0.000000 |
| 1 | 10 | 14.225380 | 0.216072 | 3.620188 | 0.884587 | 0.000000 |
| 1 | 15 | 14.296381 | 0.217334 | 3.664529 | 0.887582 | 0.000000 |
| 2 | 3 | 44.883589 | 0.411681 | 6.381553 | 1.777445 | 0.046366 |
| 2 | 5 | 44.417081 | 0.411541 | 6.273848 | 1.748695 | 0.045706 |
| 2 | 7 | 44.226442 | 0.415032 | 6.229265 | 1.741469 | 0.045529 |
| 2 | 8 | 44.178382 | 0.416927 | 6.217492 | 1.740259 | 0.045488 |
| 2 | 9 | 44.148966 | 0.418786 | 6.209947 | 1.739751 | 0.045462 |
| 2 | 10 | 44.132739 | 0.420578 | 6.205397 | 1.739656 | 0.045446 |
| 2 | 15 | 44.152494 | 0.428352 | 6.205027 | 1.741478 | 0.045424 |
| 3 | 3 | 104.544655 | 0.537183 | 11.511703 | 2.477576 | 0.111575 |
| 3 | 5 | 106.130995 | 0.559582 | 11.215774 | 2.459409 | 0.111678 |
| 3 | 7 | 106.876923 | 0.584029 | 11.108572 | 2.463104 | 0.111989 |
| 3 | 8 | 107.077583 | 0.594838 | 11.083509 | 2.466651 | 0.112121 |
| 3 | 9 | 107.207798 | 0.604597 | 11.068464 | 2.470396 | 0.112234 |
| 3 | 10 | 107.288874 | 0.613383 | 11.059918 | 2.474091 | 0.112330 |
| 3 | 15 | 107.346771 | 0.646325 | 11.059177 | 2.489656 | 0.112641 |
| 4 | 3 | 145.524656 | 0.663625 | 11.382545 | 3.125410 | 0.189881 |
| 4 | 5 | 149.483687 | 0.681028 | 11.185965 | 3.108415 | 0.190327 |
| 4 | 7 | 152.427849 | 0.703716 | 11.153697 | 3.116876 | 0.190920 |
| 4 | 8 | 153.468793 | 0.714570 | 11.159714 | 3.122918 | 0.191168 |
| 4 | 9 | 154.292289 | 0.724662 | 11.171877 | 3.128995 | 0.191387 |
| 4 | 10 | 154.946945 | 0.733934 | 11.187164 | 3.134816 | 0.191579 |
| 4 | 15 | 156.752184 | 0.769625 | 11.269717 | 3.158201 | 0.192243 |
| 5 | 3 | 183.401806 | 0.723357 | 11.201372 | 3.482182 | 0.292367 |
| 5 | 5 | 199.865959 | 0.716985 | 11.076088 | 3.471863 | 0.290504 |
| 5 | 7 | 211.172212 | 0.717475 | 11.108122 | 3.483690 | 0.293331 |
| 5 | 8 | 215.106722 | 0.720123 | 11.144609 | 3.490820 | 0.294834 |
| 5 | 9 | 218.226461 | 0.723657 | 11.185052 | 3.497837 | 0.296240 |
| 5 | 10 | 220.723751 | 0.727683 | 11.226168 | 3.504498 | 0.297525 |
| 5 | 15 | 227.862079 | 0.748661 | 11.404794 | 3.531011 | 0.302299 |
| 6 | 3 | 194.189414 | 0.517583 | 11.516144 | 3.310536 | 0.348008 |
| 6 | 5 | 241.589968 | 0.525541 | 12.050453 | 3.261059 | 0.352583 |
| 6 | 7 | 282.490744 | 0.542397 | 12.669894 | 3.271921 | 0.363992 |
| 6 | 8 | 297.234158 | 0.551253 | 12.908915 | 3.280806 | 0.368648 |
| 6 | 9 | 309.044069 | 0.559824 | 13.104916 | 3.289745 | 0.372573 |
| 6 | 10 | 318.579478 | 0.567947 | 13.265815 | 3.298245 | 0.375881 |
| 6 | 15 | 346.477279 | 0.600905 | 13.749373 | 3.331574 | 0.386479 |
| 7 | 3 | 101.857387 | 0.383907 | 5.823367 | 2.062994 | 0.034711 |
| 7 | 5 | 109.849788 | 0.387991 | 5.821334 | 2.070252 | 0.038166 |
| 7 | 7 | 115.089382 | 0.392455 | 5.902226 | 2.079531 | 0.040223 |
| 7 | 8 | 116.794952 | 0.394249 | 5.944854 | 2.083174 | 0.040867 |
| 7 | 9 | 118.080145 | 0.395743 | 5.984180 | 2.086150 | 0.041349 |
| 7 | 10 | 119.054934 | 0.396984 | 6.019427 | 2.088570 | 0.041714 |
| 7 | 15 | 121.466656 | 0.400810 | 6.141681 | 2.095560 | 0.042633 |

## Interpretation

Across placements with one through six unknown tiles, no tested temperature has a nominal 95% paired NLL advantage over T8. T8 is within 0.008 nats of the lowest observed mean at every size. This does not establish exact optimality or a win-rate benefit.

Exchanges favor lower temperatures: T3 beats T8 by 0.135737 nats (95% CI 0.040444 to 0.231030), and T5 beats T8 by 0.092361 (0.050804 to 0.133917). T3 is the best tested point estimate, not an established optimum; the minimum is at the edge of the tested grid. The implementation retains the requested global T8 default.

## Four-tile census experiment

A separate replay changes only the minimum census size from five to four. Both optimized builds use identical NWL23 observations and the same 18-job, one-inference-thread-per-job protocol. Following this experiment, the production threshold is four: per-rack move generation for one through three unknown tiles and census for four through seven. The measurements use one inference thread.

All 21,588 reported prediction-metric comparisons across 514 observations and seven temperatures agree to within 0.

| Four-tile T8 evaluator | Average (ms) | P95 (ms) | Max (ms) |
|---|---:|---:|---:|
| Per-rack move generation | 159.362 | 361.678 | 865.872 |
| Census | 64.900 | 151.109 | 229.087 |

Census is **2.46× faster** by mean T8 inference time on this sample. This does not measure an inference using 18 internal workers; previous crossover measurements depend on the worker count.

## Three-tile census experiment

The matched three-tile replay uses the same protocol and changes only the census threshold to three.

All 20,874 reported metric comparisons across 497 observations and seven temperatures match exactly.

| Three-tile T8 evaluator | Average (ms) | P95 (ms) | Max (ms) |
|---|---:|---:|---:|
| Per-rack move generation | 24.966 | 53.212 | 146.783 |
| Census | 55.929 | 123.163 | 217.873 |

Census takes **2.24× as long** on average. Keep per-rack move generation for three unknown tiles. On these samples the single-worker crossover lies between three and four tiles.

## Inference cost per game

Reconstructed by summing T8 call latencies on the same first 108 pairs (216 games), replacing four-tile timings with the matched census replay. Values are per game for the inference-using player. They exclude simulation and other game overhead and are not a direct timed both-sim match.

| Inference policy | Mean (ms) | P95 (ms) | Maximum (ms) |
|---|---:|---:|---:|
| All placements, no exchanges | 370.486 | 642.829 | 1233.410 |
| Placements and exchanges | 658.087 | 3052.062 | 9288.700 |
| Only one- to three-tile keeps | 71.467 | 157.242 | 302.849 |

At a 750 ms per-player game clock, omitting exchange inference leaves about 380 ms on average for simulations before other overhead. The tail still requires deadline enforcement or skipping calls. With 18 independent single-thread game workers, 25,000 pairs in two hours permits 2.592 seconds per complete game for both players combined, or 1.296 seconds per player before overhead.

## Exchange inference only with 30 or fewer tiles in the bag

On the matching 216 games, this gate raises mean inference cost from 370.486 ms to **371.369 ms**; p95 is **653.398 ms**, and the maximum remains **1233.410 ms**. Only 5 exchanges qualify in that subset.

Across all 864 games in the exchange corpus, 28 of 141 exchanges qualify: **67.653 ms mean per qualifying call**, **252.550 ms p95**, **267.673 ms maximum**. Amortized across all 864 games, those calls add 2.192 ms per game. This is an analyzed policy, not an implemented gate or a measured time-limited match.

## Implementation validation

Optimized inference, census/cutoff parity, benchmark metrics, configuration,
contribution, move generation, PAT, and PAT-evaluation tests pass. At the new
four-tile threshold, the census parity test compares complete alias tables on
29 positions including two exchanges, independently for hard-margin and softmax
inference; both match exactly. WMP and KWG cutoff comparisons each pass 11 games
and 204 scoring plays. The inference and metric tests also pass ASAN/UBSAN at
the four-tile threshold. A static-temperature smoke run passes under sanitizers.

The existing contribution counter test assumed each inferred leave required a
move-generation call. Its bound now counts turns and simulations, while a
separate per-rack inference test verifies move-generation accounting on one and
three workers. Optimized contribution tests pass with that corrected contract.

Formatting, full cppcheck (including Windows analysis), portability, and the
dependency checker on a clean tracked-source tree pass. The latter avoids
untracked benchmark/vendor files creating false module-name cycles. Changed
inference/benchmark/contribution tests pass local clang-tidy-18. Windows
cross-compilation requires CI's toolchain, which is not installed locally.

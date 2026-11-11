> Historical experiment: PAT inference support has been removed. See [current static validation](static-temperature-validation-20261103.md).

# PAT64 softmax experiment — November 3, 2026

This test-only experiment interprets PAT64 as rescoring the best 64 static
(score-plus-leave) candidates with the installed CSW21 PAT evaluator. The
observed move is always evaluated as well, even when outside the shortlist.
PAT augments static equity; it does not replace the move's score or leave value.
The shortlist is an approximation to the global PAT-best move. The test also
runs built-in PAT best-move generation, alternating which path runs first by
candidate letter. Both paths include PAT's score-and-bag utility adjustment.

Both posteriors use the existing regret likelihood, margin 20 and temperature 5,
and the same available-tile prior. For every possible one-tile leave, the test
reconstructs the candidate rack and rebuilds PAT's position context. PAT scores
both the observed move and the alternatives. It does not see the true hidden
leave. All five static-control metrics are checked against production inference
to an absolute tolerance of 1e-7 on every observation.

The fixed run uses 100 game pairs, seed 20261103, and collects every player-0
placement leaving exactly one tile while the bag is nonempty, up to 20 per game.
No exchanges or larger leaves are included. Player 0 uses two-ply simulations
with 15 candidates, 1,000 iterations, one thread, and PAT disabled; player 1
uses static equity. CSW21 and the installed CSW21 PAT/KLV files match the earlier
infbench protocol. This is an exploratory prediction test, not a match-strength
comparison or parameter tuning.

Log loss uses the declared 1% prior mixture. Feature MSEs use raw posterior
means, falling back to the prior only for an empty posterior. KLV and face-score
MSE are points squared. Tile-count MSE sums over tile types, including blanks.
Paired intervals average differences within each game pair, then use normal
95% intervals across pairs containing observations; they are exploratory and
not corrected for multiple comparisons.

Reproduce:

```sh
make magpie_test BUILD=no_pgo_release
./bin/magpie_test infbench:100:20:20261103:onepat
```

`PATBENCH` records contain each observation and each mode's errors. The regular
`infbench` sampling and inference behavior are unchanged.

## Corrected results and direct PAT comparison

The original manual-scoring experiment omitted PAT's score-and-bag utility
adjustment. The results below supersede those provisional numbers. The corrected
run covers the same 112 leaves and 2,207 candidate racks from 100 game pairs.
It was split across four independent workers with disjoint seeds; all static
control parity checks passed again.

| Metric | Static 20/5 | PAT64 20/5 | Direct PAT 20/5 |
|---|---:|---:|---:|
| Log loss (nats) | 2.495001 | 2.465552 | 2.465552 |
| KLV MSE | 16.330729 | 16.867463 | 16.867463 |
| Vowel-count MSE | 0.201642 | 0.201695 | 0.201695 |
| Tile-score MSE | 3.951298 | 3.928210 | 3.928210 |
| Tile-count MSE | 0.882345 | 0.877575 | 0.877575 |

Direct PAT and PAT64 found identical best equities on all 2,207 candidate racks
and identical posteriors on all 112 observations. Timed candidate evaluation
(including move generation and PAT context setup) took 0.585764 seconds for the
shortlist and 0.082392 seconds for direct best-move generation: **7.11× faster**.
These are summed wall times under four-worker load, not complete inference
latency; direct-path observed-move scoring and posterior assembly remain extra.
The shortlist measurement includes observed-move scoring, so the timing ratio
slightly favors the direct path. No production inference default changed.

All inferred modes excluded 4/112 actual leaves; finite NLL uses the declared
1% prior mixture. Unsmoothed mean NLL is infinite. PAT improves log loss by
0.029449 nats (1.18%), but KLV MSE worsens by 3.29%. Other changes are small.

| Direct PAT minus static | Pair-weighted mean | 95% interval |
|---|---:|---:|
| nll | -0.043978 | [-0.084301, -0.003656] |
| klv_mse | 0.238765 | [-0.419711, 0.897242] |
| vowel_mse | 0.000117 | [-0.006136, 0.006371] |
| score_mse | -0.019345 | [-0.098959, 0.060269] |
| tile_mse | -0.008567 | [-0.020085, 0.002951] |

Input hashes match `infbench-results-20261102.md`. Raw corrected records and
metrics are `data/benchmarks/pat64-one-tile-20261103/direct-100-pairs.log` and
`direct-metrics.json`.

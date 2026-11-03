# PAT64 softmax experiment — November 3, 2026

This test-only experiment interprets PAT64 as rescoring the best 64 static
(score-plus-leave) candidates with the installed CSW21 PAT evaluator. The
observed move is always evaluated as well, even when outside the shortlist.
PAT augments static equity; it does not replace the move's score or leave value.
The shortlist is an approximation to the global PAT-best move.

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

## Results

The completed run produced 112 observations from 70 of the 100 game pairs.
All production-control checks passed.

| Metric | Prior | Static 20/5 | PAT64 20/5 |
|---|---:|---:|---:|
| Log loss (nats) | 2.696854 | 2.495001 | 2.470392 |
| KLV MSE | 17.720164 | 16.330729 | 16.254818 |
| Vowel-count MSE | 0.255643 | 0.201642 | 0.202491 |
| Tile-score MSE | 4.351647 | 3.951298 | 3.921135 |
| Tile-count MSE | 0.911209 | 0.882345 | 0.878384 |

Both inference modes excluded 4/112 true leaves and had no empty posteriors;
therefore their unsmoothed mean log loss is infinite. The finite values above
use the declared mixture. PAT64 improves observation-weighted mixed log loss by
0.024609 nats (0.99%). No paired interval excludes zero.

| PAT64 minus static | Pair-weighted mean | 95% interval |
|---|---:|---:|
| nll | -0.038420 | [-0.080445, 0.003604] |
| klv_mse | -0.090692 | [-0.272385, 0.091000] |
| vowel_mse | 0.001066 | [-0.005198, 0.007330] |
| score_mse | -0.031422 | [-0.113252, 0.050408] |
| tile_mse | -0.007372 | [-0.019223, 0.004480] |

The benefit is small and uncertain at these settings. This does not establish
a game-strength benefit or justify changing production inference defaults.
Input hashes are identical to `infbench-results-20261102.md`. Raw records and
summary JSON are in `data/benchmarks/pat64-one-tile-20261103/`.

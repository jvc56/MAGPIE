# Inference prediction benchmark

`infbench` compares the draw-count prior, hard-margin inference, and softmax
inference against known leaves immediately after a move and before replenishment.
It is an on-demand quality benchmark, not part of the default test suite.
The small `infbenchmetrics` scoring test does run in the default suite.

```sh
make magpie_test BUILD=no_pgo_release
./bin/magpie_test infbench > infbench.log
# Explicit game pairs, requested observations per game, and seed:
./bin/magpie_test infbench:20:2:20261102 > infbench.log
# Scan games for exchanges, with a generous quota (including exchange-all):
./bin/magpie_test infbench:20:20:20261102:exchanges > exchanges.log
# Disable census to compare the same observations with per-rack move generation:
./bin/magpie_test infbench:20:20:20261102:exchanges:movegen > exchanges-movegen.log
# Compact results (other MAGPIE output also appears in the log):
rg '^INFBENCH (settings|summary|delta)' infbench.log
```

The default is four game pairs per policy, two requested observations per game,
and seed 20261102. The benchmark uses the standard 15x15 board and CSW21 data.
Each pair uses the same game seed, alternating the starting player exactly as
autoplay game pairs do. Player 0 uses the evaluated policy and player 1 uses
plain static equity:

- `static`: static equity with PAT disabled.
- `pat`: static equity with the installed `CSW21.pat` enabled for player 0.
- `sim2`: 15 static candidates, two plies, 1,000 total simulation iterations,
  minimum 10 per candidate, round-robin sampling, one simulation thread, no
  time cutoff, no inference, no PAT, and no bag cycling. Its selection utility
  uses win weight 1, spread weight 0.5, and spread scale 100.

All inference models use the current static inference evaluator (PAT disabled),
WMP enabled, four inference workers, and the same information: pre-move board,
observed move, and observer's rack. The actual target rack is retained only for
scoring; inference reconstructs its candidates without using it. For exchanges,
only the number exchanged is passed to inference, never tile identities.
Comparisons across modes are paired on identical observations **within** each
policy. Policies generate different game trajectories, so their absolute errors
are not a head-to-head comparison on identical boards.

## Sampling and modes

Observe player 0's fourth, tenth, sixteenth, ... decisions. Defer passes and
bingos to the next move with a nonempty leave. Only observe while the bag is
nonempty. Stop a game after collecting the requested observations or reaching
its end; report shortfalls rather than inventing replacements. Thus this is a
specified sample of early/midgame and sometimes preendgame moves, not an unbiased
sample of all turns. Exchanges are included when encountered, not forced.

The optional `:exchanges` suffix instead collects the first requested number of
natural exchanges by player 0 in each game, stopping when the bag empties or the
game ends. It includes exchanges of all seven tiles (an empty kept leave). It
does not force exchanges or replace games without exchanges. Both kept-leave
and discarded-tile predictions are scored on these observations. This selects
a different population from the scheduled sample above.

The fixed grid is:

| Modes | Margin (points) | Temperature (points) |
|---|---:|---:|
| `prior` | — | — |
| `hard0`, `hard5`, `hard10`, `hard20` | 0, 5, 10, 20 | disabled |
| `soft5t2`, `soft5t5` | 5 | 2, 5 |
| `soft10t2`, `soft10t5` | 10 | 2, 5 |
| `soft20t2`, `soft20t5` | 20 | 2, 5 |

The prior is the exact multivariate hypergeometric distribution for a leave of
that size drawn from unseen tiles, conditioned on revealed played tiles for
placements. For exchanges it is an uninformed random kept subset. Prior KLV is
computed by exact multiset enumeration. Posterior probabilities and feature means
are reconstructed from the complete finalized alias table; no sampling noise or
common-leave-list truncation is introduced. Duplicate kept leaves from exchanges
have their probability masses summed.

## Output and interpretation

Every record starts with `INFBENCH`. `observation` and `truth` records identify the
policy, pair, game, turn, bag count, leave size, exchange status, move, actual leave,
prior probability, and pre-move CGP. These let decision results be grouped by
leave size, exchanges, or stage without rerunning inference.

`decision` and `summary` records report:

- `raw_nll`: negative natural log of the actual leave's posterior probability.
  Lower is better. A single zero-probability observation makes the mean infinite.
- `mixed_nll`: finite log loss of `0.99 * posterior + 0.01 * prior`. An entirely
  empty posterior explicitly falls back to the prior. This is a diagnostic
  scoring convention; it does not change production inference or sampling.
- `gain_nats`: log probability improvement of that mixture over the prior.
  Higher is better; zero is the prior baseline.
- `klv_mse`, `vowel_mse`, `score_mse`: squared error of posterior expected KLV
  (points), vowel count, and total tile face score (points), respectively.
  KLV is averaged over leaves, not evaluated on expected tile counts.
- `tile_mse`: sum of squared errors in expected counts over all tile types,
  including blanks. `tile` summary records give each type's MSE separately.
- `excluded` / `excluded_rate`: actual leave has zero posterior probability.
- `empty` / `empty_rate`: no representable posterior mass. Feature predictions
  fall back to the prior in these cases; no observations are dropped.
- `seconds`: mean inference plus posterior reconstruction/scoring time. For the
  prior row it is prior construction time. Other modes also need that common
  prior cost to calculate their reported diagnostics.

Feature MSE uses raw posterior means, without the 1% mixture, except for the
explicit empty-posterior fallback. Vowels follow the letter distribution; blanks
are their own tile type and have zero face value.

`delta` records compare each softmax mode against hard inference at the **same
margin**; hard modes compare against the prior. Negative differences favor the
mode. Differences are first averaged within each game pair. The reported
`normal95_half_width` is 1.96 times the standard error across independent pairs,
not across correlated turns. It is NaN for one pair. Four pairs is a smoke test;
normal intervals are only approximate and small samples need caution. Summary
means weight observations equally, whereas delta means weight game pairs equally;
these differ if games yield unequal observation counts.

The grid is fixed rather than tuned on the reported sample. To select defaults,
use separate tuning seeds and a held-out run, and assess downstream game-pair
playing strength as a separate experiment. Changing lexica, KLV/PAT data,
simulation budget, or the sampling schedule changes the benchmark population.

## Exchanged-tile accuracy

Whenever a sampled move is an exchange, `exchanged_truth`, `exchanged_decision`,
`exchanged_summary`, `exchanged_tile`, and `exchanged_delta` records separately
score the tiles discarded. The kept-leave records continue to score only the
retained tiles before replenishment. A summary with `n=0` means no exchange was
observed for that policy.

Exchanged-tile metrics include KLV, vowel-count, total face-score, and per-tile
count MSE, plus the sum of per-tile MSE. The prediction uses the complete
exchange statistics: weighted KLV mean and tile-count marginals for softmax,
and draw-count-weighted statistics for hard inference. KLV here is the table
value of the discarded multiset, a descriptive feature rather than its value
to the player. The baseline is a random subset of the observed exchange size
from unseen tiles; actual exchanged identities are used only for scoring.
Empty predictions fall back to this baseline. Paired deltas use the same
controls and game-pair averaging as kept-leave metrics.

Exact exchanged-set log loss and exclusion rate are **not available** from the
stored marginal statistics. Their summary fields are `nan`, and no exchanged
log-loss delta is emitted. Marginal probabilities are not multiplied together
to invent a joint probability. Inference runs once per mode for both reports;
exchange timing repeats that common cost, rather than adding a second run.
Prior timing includes construction of both kept and exchanged priors.

## Census inference performance

Inference defaults to census play scores when at least five tiles are unknown
and the position satisfies the exactness conditions (a classic board, shared
word graph, and the target on turn). Other positions use per-rack move generation.
Empty boards use an opening builder that includes vowel placement penalties. The census builds board play scores
once per inference; it does not change which candidate racks are considered.
Played tiles and explicitly known target tiles remain fixed in every candidate.
For softmax, accepted candidates are fully evaluated to retain the exact best
alternative equity needed for weighting. Early termination only discards racks
outside the margin.

The optional `:movegen` suffix after `:exchanges` disables census for controlled
speed comparisons. `settings` records include `census=0` or `census=1` (permission
to use census, not a guarantee that every observation qualifies). Quality metrics
must agree between the paths. Run timing comparisons sequentially with optimized
binaries, and compare inference `seconds` separately from game generation.

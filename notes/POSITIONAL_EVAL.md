# Candidate-relative positional evaluation

## Design

The first observational experiment tried to predict later game outcomes from
the move autoplay happened to choose. That target contained substantial
game-level noise and supplied no counterfactual value for the unplayed moves.

The replacement experiment generates the top eight KLV2 static candidates and
evaluates every candidate under the same shuffled unseen tiles. Each scenario
plays four continuation plies with the KLV2 static policy. Training centers
both features and oracle spread within a position and fits only the residual
over existing static equity:

```
oracle_spread(candidate) - mean_position_oracle_spread
    = static_equity(candidate) - mean_position_static_equity
    + positional_adjustment(candidate)
```

Games, not positions, are assigned to folds. The oracle uses round-robin shared
scenarios, so candidate differences do not inherit candidate-specific draw
noise.

The experimental oracle is enabled with `KLV3_ORACLE_POSITIONAL=1` on the
existing on-demand `klv3oracle` test. The trainer is
`tools/train_positional_candidates.py`.

## Corpus and held-out result

Training corpus:

- 2,000 positions from 153 games
- top eight candidates
- 128 shared scenarios per candidate
- four static continuation plies
- bag at least 29 tiles

An untouched corpus used different game seeds:

- 2,000 positions from 156 games
- identical oracle protocol

The model retains static equity with coefficient exactly one and adds only the
learned correction.

| Model | Features | Holdout gain/position | 95% CI | Game-clustered p |
|---|---:|---:|---:|---:|
| Compact | 35 | +0.148 points | +0.057 to +0.242 | 0.0015 |
| Local | 263 | +0.159 points | +0.064 to +0.263 | 0.0013 |
| Full | 495 | +0.220 points | +0.113 to +0.328 | 0.000061 |

The compact model consists of five integer features with bag-phase and
score-state coefficient tables:

- open perpendicular sides beside newly placed tiles;
- the same exposure weighted by tile score;
- visible premium access within seven squares;
- premium access weighted by tile score;
- premium weight immediately outside the first and last newly placed tile.

The original deployment restricted the compact model to the top two
candidates within three static-equity points. A later deployment sweep kept
the fitted model frozen and varied candidate count, static margin, adjustment
scale, adjustment cap, and the minimum active bag size.

The sweep used three independent 2,000-position evaluation corpora (6,000
positions total) after the original training corpus:

| Deployment | Mean candidates retained | Oracle gain/position | 95% CI |
|---|---:|---:|---:|
| Top 2, margin 3, scale 1.00 | 1.566 | +0.090 | +0.044 to +0.137 |
| Top 2, margin 1, scale 1.00 | 1.304 | +0.076 | +0.035 to +0.117 |
| Top 3, margin 1, scale 1.00 | 1.434 | +0.078 | +0.035 to +0.121 |
| Top 3, margin 3, scale 0.75 | 1.913 | +0.094 | +0.048 to +0.140 |
| Top 3, margin 3, scale 0.70 | 1.913 | +0.096 | +0.051 to +0.142 |

The 0.70 and 0.75 scales are indistinguishable at this resolution. The 0.75
version had the strongest direct game result and is exactly 3/4 in fixed-point
arithmetic, so it is the deployment default. Symmetric adjustment caps
consistently reduced oracle quality. Raising the minimum active bag size also
discarded useful signal.

## Cross-set and leave-aware refinement

The second model revision evaluates the exact board after each hypothetical
candidate. It scans every nontrivial directional cross set and separates two
effects:

- opportunity: whether the mover's resulting leave holds one or more legal
  hooking tiles;
- defensive risk: whether one or more legal hooking tiles remain in the
  public unseen multiset.

The public unseen multiset is the bag plus the opponent rack as a union. This
uses only information available to the player; it does not inspect which
unseen tiles are actually on the opponent rack. Blank is included naturally
as a legal hooking tile. If a cross set has no intersection with that public
unseen multiset, the hook is exhausted and contributes no defensive-risk
features. A hook held by the mover can therefore be safe, contested, or dead
to both players.

The frozen training fit uses held/live counts, legal-option counts, unseen
tile copies, premium-square weights, and cross-set specificity. Per-letter
held-hook features were tested but rejected because they were unstable across
the independent corpora. Redundant diagnostic partitions such as total,
dead, safe-held, contested, and opponent-only remain available to the
extractor but are omitted from the regression.

The cross-set model was trained once on the original 2,000-position corpus and
then evaluated without refitting on three independent 2,000-position corpora
and a final 20,000-position confirmation:

| Corpus | Compact regret reduction | Cross-set regret reduction | Increment |
|---|---:|---:|---:|
| Holdout | +0.1481 | +0.1672 | +0.0191 |
| Confirm | +0.0353 | +0.0722 | +0.0369 |
| Confirm 2 | +0.0877 | +0.1281 | +0.0404 |
| Final confirmation (20k) | +0.1053 | +0.1447 | +0.0394 |

The 20,000-position endpoint was fixed before inspecting partial results. The
hook and base models selected different moves in 1,684 positions: the hook
model won 893 oracle comparisons, lost 788, and tied three. Its paired
increment was +0.0394 points per position; clustered by 1,543 source games it
was +0.0387 with 95% CI +0.0183 to +0.0592 and p = 0.000205. This independently
confirms that exact cross sets, leave-held hooks, and exhausted-hook handling
add signal beyond the original compact positional features.

The production extractor temporarily places only the candidate's tiles,
regenerates only the affected cross sets, scans the board, and restores the
saved squares. It deliberately skips cross-score, extension-set, and WIT
cache writes. A 532-line ASan/UBSan relabel differential was byte-identical
to the original full-update extractor, and the focused test verifies the
whole game and WIT cache are unchanged.

Five 50,000-game, eight-thread `no_pgo_release` timing rounds with positional
evaluation active for player 1 measured 44.61 mean CPU seconds, versus 43.09
for the pre-cross-set model: +3.5% CPU. Median wall time was 5.91 versus 5.74
seconds: +3.0%. These timings include the complete match; only one player uses
the cross-set model.

## C implementation

`src/ent/positional_eval.h` contains the fixed-point coefficient table and
move-local feature calculation. `get_top_positional_move` generates at most
three candidates within three points of the static leader, multiplies the
learned correction by 3/4, then reranks them. It does not change move
generation or shadow values.

An 800-candidate differential check compared the C implementation against the
Python features and fitted coefficients. Maximum quantization difference was
0.000573 points; mean absolute difference was 0.000237.

For experimental autoplay, set one of:

```
MAGPIE_AUTOPLAY_POSITIONAL=p1
MAGPIE_AUTOPLAY_POSITIONAL=p2
MAGPIE_AUTOPLAY_POSITIONAL=both
```

The variable affects only zero-ply autoplay players. It is deliberately not a
general config setting yet.

## Game result

The top-three, three-point-window, 3/4-scale compact evaluator played 100,000
paired games against ordinary KLV2 static evaluation:

- positional 100,433 wins
- baseline 98,800 wins
- 767 ties
- 50.410% of decisive games
- 95% normal CI 50.190% to 50.629%
- exact two-sided binomial p = 0.000256
- mean score advantage = +0.831 points/game

The score advantage is descriptive because the aggregate autoplay report does
not retain paired spread deltas.

Nine 10,000-pair timing rounds on eight threads, after bypassing the reranker
in its untrained late-game region, had medians:

- baseline: 1.69 seconds
- positional for player 1 only: 2.13 seconds

That is a 26.0% whole-match increase for the strength A/B. A one-point window
reduced the increase to about 22.5%, but also reduced both oracle and direct
game strength. These are end-to-end game timings, so the different policies
also produce different game trajectories.

A 30-second macOS sample attributed only about 0.3% of total CPU to
`positional_eval_get_adjustment`; optimizing the feature arithmetic cannot
materially erase the whole-match overhead. A repeated environment lookup cost
about the same and was removed from the hot loop, as was an unnecessary final
move-list sort. The remaining cost is overwhelmingly from asking move
generation to retain multiple near-best candidates. Even so, the final
strength run processed roughly 9,400 complete games per second, still orders
of magnitude cheaper than time-limited simulation.

## Initial sim-candidate selection

A separate experiment asked whether the compact adjustment can improve the
initial candidates supplied to a KLV2 sim. It overgenerates the static list,
adds the unshrunk positional adjustment, then retains a smaller list. The
rollout and horizon policies remain ordinary KLV2; positional evaluation only
changes candidate-set membership.

The discovery sweep used 2,000 positions, top 30 static candidates, 128 shared
four-ply oracle scenarios per candidate. Before looking at a fresh corpus, two
selectors were fixed for confirmation: top 10 from top 20 and top 15 from top
30. The fresh run used 5,000 positions and 256 scenarios per candidate
(38.4 million candidate rollouts and 153.6 million continuation nodes).

| Selector | Set changes | Mean swapped | Oracle utility ceiling | 95% game-clustered CI | p |
|---|---:|---:|---:|---:|---:|
| Top 10 from 20 | 65.94% | 0.992 | +0.0000415 | +0.0000060 to +0.0000769 | 0.028 |
| Top 15 from 30 | 78.02% | 1.410 | +0.0000280 | -0.0000010 to +0.0000570 | 0.069 |

Thus top 10 from top 20 replicated the discovery result, while top 15 from top
30 did not clear significance. The metric above is only the oracle ceiling of
each set and can overstate the downstream effect of an actual noisy sim.

For the downstream check, both top-10 lists received exactly 1,500 samples per
arm under the same two-ply KLV2 policy. Only positions where the two sims chose
different moves were adjudicated, using 512 shared four-ply KLV2-static
scenarios. The first 100 disagreements and an independently seeded
200-disagreement replication were kept separate:

| Batch | Source positions | Candidate-set changes | Selected disagreements | Conditional utility | p |
|---|---:|---:|---:|---:|---:|
| First | 4,031 | 2,607 | 100 | +0.001112 | 0.179 |
| Independent replication | 7,297 | 4,815 | 200 | +0.000963 | 0.157 |

Neither batch independently confirmed the utility magnitude. Descriptively
pooling the frozen protocol gives:

- 300 disagreements across 11,328 source positions;
- +0.001013 conditional utility, 95% CI -0.000026 to +0.002052,
  game-clustered p = 0.056;
- +0.0000268 utility per source position, 95% CI -0.0000008 to +0.0000544,
  p = 0.057;
- +0.325 spread on disagreement positions, p = 0.0366;
- 170 positional oracle wins to 130 static, exact two-sided p = 0.0242.

The candidate-set ceiling improvement is confirmed, but its production impact
is very small: the final selected move changes on only 2.65% of source
positions. Utility remains just outside the prespecified significance
threshold, so this is suggestive rather than a clean strength confirmation.
An exploratory bag split found the positive effect concentrated at bag 61+
(168 disagreements, +0.002014 utility, nominal p = 0.0061); the middle and
later strata were null. That split is hypothesis-generating and needs a fresh,
preregistered test before it is used as a gate.

## Positional evaluation as the simulation rollout policy

An experimental `SimArgs` switch changes only the intermediate greedy rollout
moves from KLV2 static to the deployed positional reranker. Root candidates,
random seeds, KLV2 horizon leave residuals, two-ply depth, and all other sim
settings remain identical. The switch defaults off and has no config surface.

The first fixed look reused 1,000 untouched positions from the 20,000-position
confirmation corpus. Both policies simulated the same eight static root
candidates. The stored candidate labels use 128 shared four-ply KLV2-static
oracle scenarios.

| Nomination budget | Agreements | Disagreements | Positional wins-losses | Oracle spread delta/source | 95% CI | p |
|---|---:|---:|---:|---:|---:|---:|
| Equal samples, 1,500/root | 957 | 43 | 20-23 | -0.0261 | -0.0782 to +0.0260 | 0.33 |
| Equal time, 100 ms | 945 | 55 | 22-33 | -0.0315 | -0.0842 to +0.0212 | 0.24 |

Game-clustered estimates are essentially identical: -0.0264 (p = 0.32) and
-0.0325 (p = 0.28), respectively. Positional rollout selection ran at 102,899
iterations/second versus 139,471 for static in the equal-sample arm, a 26.2%
loss. Under equal time it completed 10.06 million iterations versus 13.79
million, 27.0% fewer.

This is no evidence that positional evaluation helps as an always-on rollout
policy. Importantly, equal samples were also slightly negative, so the result
is not presently explained only by lost throughput. The confidence interval
still permits a very small benefit. The stored oracle continues with KLV2
static play and is therefore not policy-neutral; a more expensive nested or
cross-policy adjudication of the 43-55 disagreements would be required before
claiming the positional policy is intrinsically worse. It is not warranted as
a production default from these results.

### High-adjustment positions

A fresh conditional confirmation tested whether positional rollouts help when
the source position has unusually large adjustments. The selection statistic
was the mean absolute deployed adjustment (including 3/4 shrinkage) across the
up-to-three static candidates within the three-point deployment window. This
is preferable to the signed mean, because large positive and negative
candidate adjustments can otherwise cancel.

The test selected the 2,000 largest values from the 19,000 confirmation-corpus
positions not used by the first rollout-policy experiment. Every selected
position had a mean absolute adjustment of at least 5.093 points. At the same
1,500 samples per root used by the original equal-sample test:

- static and positional agreed in 1,911 positions;
- positional won 47 and lost 42 of 89 disagreements;
- oracle spread delta was -0.00028 per source position;
- the game-clustered 95% CI was -0.02958 to +0.02903 (p = 0.985);
- positional ran at 107,914 iterations/second versus 150,752 for static.

There was no dose response inside this already-high subset. Its highest
quartile (mean absolute adjustment at least 6.299) was 15-14 with -0.0074
spread per position; the highest adjustment range quartile was 17-20 with
-0.0345. Thus a simple source-position gate based on average magnitude, peak
magnitude, or candidate-to-candidate range does not recover rollout-policy
value. This does not directly test a gate based on the average signal observed
over the actual descendant positions visited during rollouts, but measuring
that signal would already incur most of the positional evaluator's cost.

## KLV3 plus positional candidate count and BAI floor

A constant-work sweep tested KLV3 and positional evaluation together as the
root candidate selector while retaining ordinary KLV2 for rollout moves and
the horizon leave residual. KLV3 overgenerated 128 moves, the deployed
three-quarter positional correction reranked them, and the top 60 were saved.
The corpus contains 1,000 positions. Each saved candidate received 256 shared
four-ply KLV2-static oracle scenarios.

Every sweep cell received exactly 36,000 move-generation nodes per position:
12,000 two-ply sim iterations at three nodes per iteration. Root
overgeneration is the same for every cell and is outside this node budget.
Regret is decomposed into candidate-exclusion regret and BAI sampling regret.

A 23-cell discovery sweep used 200 positions. The best cell was 30 plays with
a 200-iteration-per-play minimum, but 20/300 was essentially identical
(paired excess regret +0.0000041, p = 0.96). The useful pattern was that the
strong cells spent about half of the 12,000 iterations establishing a uniform
floor and left the other half for adaptive top-two sampling.

Twelve cells near that ridge were frozen and evaluated on the remaining 799
positions:

| Plays | Minimum/play | Uniform floor | Mean utility regret | Oracle hit rate | Mean spread delta |
|---:|---:|---:|---:|---:|---:|
| 20 | 300 | 6,000 | 0.0017045 | 74.2% | -0.501 |
| 25 | 240 | 6,000 | 0.0017682 | 74.0% | -0.529 |
| 30 | 150 | 4,500 | 0.0017890 | 73.1% | -0.519 |
| 30 | 200 | 6,000 | 0.0018313 | 73.5% | -0.540 |
| 15 | 400 | 6,000 | 0.0018469 | 72.8% | -0.532 |
| 40 | 150 | 6,000 | 0.0019063 | 72.8% | -0.546 |
| 50 | 120 | 6,000 | 0.0020313 | 71.7% | -0.595 |

The best tested setting is therefore **20 plays and a 300-iteration
minimum**. It significantly beat 40/150 (paired p = 0.018), 25/300
(p = 0.0068), 30/250 (p = 0.010), and 50/120 (p = 0.0023). It was not
distinguishable from 25/240 or 30/150, so 20/300 is the recommended point
estimate rather than a uniquely established mathematical optimum.

On the held-out corpus, the best candidate among all 60 was already in the
top 20 on 98.87% of positions. Mean candidate-exclusion regret was only
0.0000776 at 20 plays, 0.0000566 at 25, and 0.0000273 at 30. Once that term is
this small, adding arms costs more sampling precision than it recovers from
the tail of the root list.

Doubling the budget to 72,000 nodes gives 24,000 two-ply iterations per
position. A separate 14-cell discovery sweep again found a broad ridge around
a uniform floor equal to half the budget. Eight cells were frozen for
held-out confirmation:

| Plays | Minimum/play | Uniform floor | Mean utility regret | Oracle hit rate | Mean spread delta |
|---:|---:|---:|---:|---:|---:|
| 30 | 400 | 12,000 | 0.0016028 | 74.3% | -0.460 |
| 20 | 600 | 12,000 | 0.0016244 | 74.5% | -0.452 |
| 25 | 600 | 15,000 | 0.0016351 | 74.5% | -0.469 |
| 40 | 300 | 12,000 | 0.0016470 | 74.6% | -0.471 |
| 15 | 800 | 12,000 | 0.0016887 | 74.1% | -0.477 |
| 60 | 200 | 12,000 | 0.0016896 | 74.0% | -0.475 |
| 30 | 300 | 9,000 | 0.0016978 | 73.8% | -0.482 |
| 25 | 480 | 12,000 | 0.0017164 | 73.8% | -0.462 |

The best tested 72,000-node setting is therefore **30 plays and a
400-iteration minimum**. Its paired advantage over 30/300 was significant
(p = 0.035), as was its advantage over 25/480 (p = 0.049). It remained tied
with 20/600, 25/600, and 40/300, so candidate-count precision is still
limited.

Relative to the 36,000-node winner, the 72,000-node winner reduced mean
regret by 0.0001018, or 6.0%; that best-to-best comparison was not significant
(p = 0.17). Holding candidate count and allocation ratio fixed gives a cleaner
comparison: doubling 30/200 to 30/400 reduced regret by 0.0002286, or 12.5%
(p = 0.0265), and improved mean spread by 0.0799 points (p = 0.0277).

### Generalizing breadth, floor, and rollout depth

The earlier four-ply labels are unsuitable for choosing sim depth because
they structurally favor a policy with the same horizon. A separate corpus
therefore saved the top 60 KLV3-plus-positional candidates from each of 1,000
positions and evaluated every candidate with 256 shared **terminal**
KLV2-static playouts. This consumed 15.36 million candidate rollouts and
238.87 million continuation nodes. It is still a policy-relative teacher, not
perfect-play truth, but it has no arbitrary four-ply cutoff.

An ASAN pass over the depth sweep found a pre-existing negative-index WIT
lookup when a shadow anchor was shorter than a cached board block. The fix
conservatively skips that optional prune and leaves normal playthrough
validation in charge. Root candidate sets and ranks did not change, but 186
of 1,000 terminal-oracle winners did, so the corpus and every result below
were regenerated after the fix. No pre-fix labels were pooled into this
analysis.

Discovery used positions 0..199 at 18,000, 36,000, 72,000, and 144,000
requested movegen nodes. Positions 200..999 remained untouched while cells
were chosen. Sparse boundary checks covered one and four plies at every
budget, plus six and eight plies at 72,000 and 144,000. The corrected
discovery minima were:

| Nodes | 1 ply | 2 plies | 3 plies | 4 plies | 6 plies | 8 plies |
|---:|---:|---:|---:|---:|---:|---:|
| 18,000 | .01254 | **.01027** | .01116 | .01267 | — | — |
| 36,000 | .01288 | **.01018** | .01069 | .01229 | — | — |
| 72,000 | .01292 | .01048 | **.01047** | .01060 | .01167 | .01244 |
| 144,000 | .01287 | **.01032** | .01051 | .01079 | .01145 | .01136 |

These are noisy discovery minima, not effect estimates. They exclude the
obviously shallow/deep tails and retain two through four plies at the larger
budgets.

For a fixed continuation depth `p`, the discovery ridge is described well by:

```
I = floor(B / (p + 1))
N = round_to_5(20 * sqrt(I / 12000))
m = round_to_25(I / (2 * N))
```

Here `B` is the requested movegen-node budget, `I` is the sim-iteration
budget, `N` is the number of root candidates, and `m` is the minimum samples
per candidate. `N` should be clamped to the available candidate count and to
10..60 over the measured range, and `m` must be adjusted downward after
rounding if `N*m > I`. The important invariant is that roughly half of all
iterations establish a uniform floor and half remain for top-two BAI. This
formula produced the following preregistered held-out cells:

| Nodes | 2-ply `(N,m)` | 3-ply `(N,m)` |
|---:|---:|---:|
| 18,000 | (15, 200) | (10, 225) |
| 36,000 | (20, 300) | (15, 300) |
| 72,000 | (30, 400) | (25, 350) |
| 144,000 | (40, 600) | (35, 525) |

The held-out results for those smooth cells were:

| Nodes | 2-ply regret | 3-ply regret | 3-ply minus 2-ply | p |
|---:|---:|---:|---:|---:|
| 18,000 | .010613 | .010698 | +.000085 | .82 |
| 36,000 | .010862 | .010453 | -.000409 | .20 |
| 72,000 | .010710 | .010643 | -.000067 | .82 |
| 144,000 | .010518 | .010711 | +.000192 | .55 |

Averaging the four paired differences per position and clustering by source
game gives 3-ply minus 2-ply regret **-0.000050** (95% CI -0.000554 to
+0.000455, p = 0.85). Three-ply spread is 0.079 points better (95% CI -0.051
to +0.210, p = 0.23). Neither metric establishes a quality difference.

The formula also generalized more reliably than selecting a noisy discovery
minimum. At 72,000 nodes, the two smooth two-ply cells (25/475 and 30/400)
tied at .010692 and .010710, while the discovery winner 20/750 regressed to
.011052; the latter gap was significant at p = 0.049. At 144,000 nodes,
40/600 was the best held-out two-ply cell, though all three alternatives
remained statistically tied. The original square-root rule is retained
because it is smooth, predeclared, and at least tied across all budgets.

Four plies was retained for held-out confirmation where discovery was
competitive. It beat the primary two-ply cell by 0.000202 at 72,000 nodes
(p = 0.42), then lost by 0.000233 at 144,000 (p = 0.41). There is no
consistent four-ply advantage. Bag-stratified two-versus-three-ply utility
comparisons also found no useful depth interaction.

Deeper policies cost wall time even at equal movegen nodes. Across the four
budgets, two plies sustained 391k-405k nodes/s and three plies 336k-351k
nodes/s, a 13%-14% loss. Four plies sustained 309k-313k nodes/s, about 23%
below two plies. Since equal-node quality is tied, the deeper policies are
strictly worse at equal wall time.

The production recommendation is therefore **two plies plus the formula
above** over the measured 18,000-144,000-node range. Keep about half of the
iteration budget for the per-arm floor and half for adaptive BAI. One, six,
and eight plies should not be selected in this range; four plies and a
bag-dependent depth rule have no validated advantage. Budgets outside the
measured range remain extrapolations.

## Limitations

- Most earlier experiments use a four-ply KLV2-static continuation, not an
  exact game-value oracle. The generalized depth sweep instead uses terminal
  KLV2-static playouts, but those are still relative to the chosen
  continuation policy. Actual game results remain more important than either
  teacher metric.
- Positions below 29 bag tiles were absent from training, so the adjustment is
  disabled there.
- The model only reranks the top three moves within three static-equity points
  and shrinks its raw correction by 25%.
  This is intentional regularization and a speed/quality compromise.
- A production version needs a real config surface and should regenerate a
  larger, frozen training corpus before treating the coefficient table as
  stable data.

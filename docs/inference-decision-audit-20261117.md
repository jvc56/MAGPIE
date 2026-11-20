# What rack inference buys at the next turn

MAGPIE's static T8 inference has no demonstrated win advantage at the tested clocks. Modeling a searching opponent's move selection improved downstream equity and blended utility in two pre-endgame populations. Against the four-ply player of interest, the cheaper models improved rack prediction, but the tested replies offered very little opportunity to convert that information into wins.

## Original experiments

I reaggregated the completed game pairs from the raw worker logs. With PAT on, T8 won 49.919% across 77,314 pairs (95% interval 49.744–50.095%) and averaged −0.13 spread points per game (−0.43 to +0.16). The PAT-off result also reproduced. The half-clock PAT control won 49.885% (49.573–50.197%) and lost 0.96 spread points per game. Those are net effects at the specified clocks. Estimating the price of 10% fewer iterations from a 50% clock handicap requires an assumed shape for the clock–strength relationship. The handicap's own win interval includes zero.

For the decision analysis I froze the live soft-only log through game 68 and used complete games in the four four-ply runs. A decision contributes zero if the players agree. Intervals cluster all decisions within each game. The previous summary diluted per-game means for sparsely tested arms by inserting zeros for games from other runs. That error affected the per-game column. Its per-decision standard errors also treated turns as independent. I corrected both. The per-game column sums local oracle effects along a control trajectory and should be read as a diagnostic.

| Arm | Decisions | Move changed | Sim win change, pp (95% interval) | Sim equity change, points (95% interval) |
|---|---:|---:|---:|---:|
| Static T8 | 6,513 | 4.9% | −0.038 (−0.093 to +0.017) | +0.033 (−0.055 to +0.121) |
| Second control | 5,198 | 2.4% | +0.005 (−0.004 to +0.014) | +0.027 (−0.004 to +0.058) |
| Perfect current rack | 5,198 | 60.8% | +3.185 (+2.934 to +3.437) | +8.287 (+7.940 to +8.635) |

The full-rack figure includes information about newly drawn tiles. After a bingo, the previous move reveals an empty leave with certainty and all seven tiles are new. Such turns account for 1,447 of 6,513 T8 decisions. T8 changed one of them. In the 1,154 such positions with a perfect-rack arm, knowing the rack was still worth +2.41 win points per decision. This portion of the apparent headroom is unavailable from the last leave. Where the opponent kept six tiles, T8 changed 18.4% of 141 replies. Comparing T8 and the second control directly on shared positions gives −0.038 ±0.061 win points. The no-information and six-tile cases behave differently, as a working sampler should.

I traced the posterior from inference through the alias table into the rollout rack. The recorded differing-move sim-oracle comparisons had 29,991–30,008 rollouts per move. The control's median eligible search took 0.57 s. Of 6,513 eligible searches, 265 took at least 14.9 s. Per seat, the median total control search time before the bag emptied was 15.0 s and the maximum was 55.5 s. These observations describe the 15 s per-move cap, default stopping rule and truncated games. A 25-minute strategy that spends its clock on more candidates, a stricter stop, deeper searches or an endgame solver needs its own test.

## A reproduced rollout bug

The alias sampler and the Fisher–Yates shuffle initialized identical PRNGs with the same seed. The first random word thus helped choose both the kept leave and the tile at the front of the shuffle. A correct leave histogram alone cannot detect a biased refill.

I tested the normal simulation entry point on a CSW24 board with eight different unseen tiles and an equal posterior on EGLNRS and GLNRSY. The three possible final bag tiles E, I and Y have probabilities ¼, ½ and ¼. Separate known-rack sims give an exact expected win probability of 0.75 for the tested reply. Over 8,192 seeded rollouts with bag cycling off, the original sampler gave 0.80713. Giving the alias sampler a separate seed stream gave 0.75000 on the same test. The regression is available as `infrefill`. The size of the effect in the original bag-cycled T8 experiments is unmeasured. Their frozen binaries still contain the original sampler.

## Downstream replay

For replies with one tile left in the bag, its identity determines the entire current opponent rack. I reconstructed the public board and the replying player's rack, inserted an arbitrary feasible rack to replay the previous move, and generated up to 100 PAT reply candidates once. For each possible final bag tile, I evaluated every reply using MAGPIE's four-ply sim with the complementary seven tiles fixed. With one tile left, the draw order is determined. Each policy chose the reply maximizing the same posterior-mean blended utility, with a common static-order tiebreak. The reported win and equity differences use those chosen replies. Two different feasible input racks produced identical T8 marginals and reply matrices on all six boards checked.

For scoring, each possible refill is weighted by its probability given the logged true leave. If the previous bag contained B tiles and the post-play unseen pool contains U copies of a letter, a candidate leave containing L copies puts probability (U − L) / B on that letter being the last bag tile. This integrates out draw luck and keeps the actual leave out of move selection, except in the labeled oracle. The oracle uses MAGPIE's four-ply continuation. The quantities below measure changes per qualifying reply.

All entries use nominal 95% half-widths with seed-pair or game clustering. There was one qualifying position per cluster in the reported replay samples. “Bag gain” is the improvement in log probability, in nats, for the final bag tile and therefore for the opponent's current seven-tile rack. It is a different target from the six-tile leave.

| Opponent and inference | Replies | Changed | Bag gain | Win pp | Utility pp | Equity points |
|---|---:|---:|---:|---:|---:|---:|
| Staged solver, T8 | 662 | 19.9% | +0.048 | −0.44 ±0.47 | −0.32 ±0.34 | −0.12 ±0.77 |
| Staged solver, greedy-solver regret | 662 | 28.7% | +0.335 | +0.45 ±0.48 | +0.47 ±0.35 | **+2.59 ±1.05** |
| Staged solver, greedy top-eight membership | 662 | 31.6% | +0.430 | +0.37 ±0.54 | +0.48 ±0.39 | **+3.61 ±1.11** |
| Two-ply sim, T8 | 576 | 22.0% | +0.137 | +0.13 ±0.64 | +0.23 ±0.46 | +2.32 ±1.03 |
| Two-ply sim, static top-15 membership | 576 | 28.0% | +0.398 | +0.48 ±0.60 | +0.62 ±0.43 | **+4.52 ±1.20** |
| Two-ply sim, eight samples per candidate | 576 | 36.1% | +0.348 | +0.26 ±0.73 | +0.47 ±0.53 | +4.53 ±1.34 |
| Four-ply control, T8 | 139 | 5.0% | +0.006 | −0.36 ±0.71 | −0.23 ±0.47 | +0.06 ±0.56 |
| Four-ply control, PAT top-100 membership | 139 | 8.6% | +0.045 | ~0 | +0.03 ±0.03 | +0.34 ±0.40 |
| Four-ply control, sampled four-ply regret | 139 | 6.5% | +0.070 | ~0 | +0.01 ±0.03 | +0.17 ±0.40 |
| Four-ply control, true kept leave | 139 | 37.4% | +0.648 | +0.07 ±0.14 | +0.28 ±0.12 | +3.44 ±1.21 |

**Staged solver.** I used the handoff's fixed A=200 and T=8 with a 1% posterior mixture with the prior. On the odd-pair sample the six-tile log gain reproduced at 0.970 nats. The original cache reader accepted truncated enumerations after the true rack appeared. The corrected reader checks uniqueness and total combinatorial mass, which is 28 in this setting. Three staged cache entries containing the truth failed the mass check across both splits. The alternative rank model selected the top eight and a relative likelihood of 0.001 outside that shortlist on the even pairs. It uses the same cached greedy solves. On the same replies, the regret model beat T8 by +0.90 win points (95% interval +0.31 to +1.48), +0.78 utility points and +2.71 equity points. Its win difference from the plain prior is sensitive to the interval method and rests on 11 positions with a win change greater than one millionth of a percentage point. The cost still matters. Summing the original per-rack solve times on these test cases gives a 28 s median and 275 s p95 on one thread.

**Two-ply sim.** I used the first 32 odd-pair positions in each of 18 workers. These are selected one-tile plays from mostly static trajectories. The opponent generated 15 static candidates and searched them with PAT off. All 576 actual racks admitted the observed move. For each possible pre-play rack I generated that same shortlist and retained the racks that admitted the move. I used the combinatorial prior over full racks capable of making the play, then mixed 1% of the original leave prior into the posterior. Correcting the combinatorial prior by itself gave −0.15 equity. Including shortlist membership beat T8 by +2.20 equity points (95% interval +1.34 to +3.06) and +0.39 utility points (+0.05 to +0.73). The candidate portion took about 2.3 ms median and 12.5 ms p95 on one thread under load, as timed inside the replay. Adding eight two-ply rollouts per candidate and normalizing the move likelihood across candidates at utility temperature 0.04 took 36 ms median. Its win and equity estimates gave no reason here to pay the extra cost. Exact known-leave equity headroom in this sample was +10.98 points.

**Four-ply target.** I reconstructed every completed four-ply game in the snapshot and used all placements retaining at least one tile that left one tile in the bag after replenishment. These include multi-tile plays. The observed move belonged to the true rack's top 100 PAT candidates in all 139 cases. That wider shortlist contributed much less evidence. The sampled model compared the observed move to the four best static PAT candidates using eight four-ply rollouts per move, with regret temperature 0.04 in utility units. Median cost was 28 ms for the candidate portion and 100 ms with sampling. Even a player told the true leave and allowed to choose purely for win could improve the prior choice on only **13 of 139** positions. Its mean maximum win gain was **0.088 percentage points**. Revealing the actual replenishment as well raised this pure-win bound to 1.67 points. The prediction and equity gains in the other two populations therefore give limited evidence about actual wins against this opponent.

## Most useful next experiment

For the 3-minute and 25-minute use case, first measure the true-kept-leave arm in ordinary four-ply positions. The local `infdecide` arm `l` fixes exactly the tiles retained after a placement and samples the refill normally. It completed a one-game integration check with 19 eligible turns. `INFDEC_SET` now applies as a separate command so a stricter `-minp` can override the harness default. Exchanges use the control behavior. Compare `l`, T8, the second control and perfect information on the same positions. Break results out by bag size, tiles retained and the public score margin. After a bingo the immediate leave gives no information.

I would prioritize positions with several tiles left, a close score and plausible replies whose values change with the rack. Define any screening rule from public information on development games and score it on untouched games. For those positions, a first affordable model is to sample feasible pre-play racks, generate the opponent's actual candidate set on each, and reweight the racks based on whether the observed move survived. The staged and two-ply results justify testing this mechanism. The top-100 results show why matching the search *within* a wide candidate set also matters. Compare each model on decision utility at equal wall time, then run paired 18-thread games with actual 180 s and 1,500 s player clocks, including the endgame and charging inference to the player. The original match, the reply replays and the sampler regression answer different parts of that question.

## Reproduction

Local branch `local/infmatch-handicap` at `aa1ec60a` plus working-tree edits. Results are in `data/benchmarks/softmax-20261115/reply-audit/`. From the checkout, the following commands reproduce the summaries using the dedicated audit binary.

```sh
d=data/benchmarks/softmax-20261115
b=bin/magpie_test_softmax_audit
python3 scripts/analyze_inference_decisions.py --complete-only \
  "$d/decide-15s/run.log" "$d/decide-15s-ext/run.log" \
  "$d/decide-15s-feat/run.log" "$d/reply-audit/decide-soft-snapshot.log"
python3 scripts/replay_peg_replies.py --binary "$b" "$d"/peg2x1-cache/staged-*.log
python3 scripts/replay_peg_replies.py --binary "$b" --sim2 --per-worker 32 "$d"/peg2x1-sweep/sim2-*.log
python3 scripts/replay_peg_replies.py --binary "$b" --control4 \
  "$d/decide-15s/run.log" "$d/decide-15s-ext/run.log" \
  "$d/decide-15s-feat/run.log" "$d/reply-audit/decide-soft-snapshot.log"
"$b" infrefill
```

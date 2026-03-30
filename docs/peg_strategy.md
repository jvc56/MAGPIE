# PEG Solving Strategies by Time Budget

## Context

A 1-in-the-bag PEG solve evaluates ~600 candidate moves across ~8 bag-tile
scenarios with a multi-stage pipeline: greedy playout, then 1-ply, 2-ply, ...
endgame refinement passes that progressively narrow the candidate set.

The core cost equation is:
```
total_time = candidates x scenarios x cost_per_endgame_solve x num_stages
```

Nigel Richards doesn't brute-force this. He prunes ruthlessly -- probably
evaluating <10 candidates across the scenarios that matter, with an
intuitive sense for which draws flip outcomes. The strategies below
aim to replicate this at different time budgets.

---

## 3-Minute Time Control (max 60s per PEG turn)

### Target: correct answer 90%+ of the time in <30s

At this budget, we can afford greedy + 1-ply endgame for a small candidate set.
On the test position (single-threaded), greedy takes ~1.5s and 1-ply takes
~5s for 32 candidates. With 8 threads, this is well under budget.

#### Near-term (pure engineering)

- **Aggressive greedy pruning**: Top-16 from greedy -> 1-ply, top-8 -> 2-ply.
  Selfplay calibration data suggests the winner is in the greedy top-8 about
  95% of the time for 1-bag positions.

- **First-win optimization**: Use narrow-window search (a=-1, b=+1) for
  endgame stages. Only determines win/loss, not spread. ~3-5x faster per
  endgame solve. Compute spread only for tied-win% candidates on the final
  stage.

- **Early cutoff pruning**: Stop evaluating a candidate's remaining scenarios
  once it mathematically can't reach the K-th best win%. With killer-draw
  ordering (evaluate historically losing draws first), cutoffs fire after
  3-4 of 8 scenarios for weak candidates.

- **Parallel scenario evaluation**: With 8 threads and 8 scenarios, evaluate
  all scenarios for a candidate simultaneously. The shared TT means threads
  benefit from each other's search.

#### Big brain

- **Skip-greedy mode**: Skip the greedy stage entirely. Sort candidates by
  tiles played (longest first -- bag-emptying plays are strongest in endgame),
  break ties by static equity. Promote top-K directly to 1-ply endgame.
  Saves 1-2s and the greedy rankings aren't always reliable anyway.

- **Adaptive depth termination**: If the top candidate leads by >15% win
  after 1-ply, don't bother with 2-ply. Most positions have a clear winner
  after 1-ply; deep search only matters for close decisions.

- **Speculative next-stage evaluation**: When endgame threads finish their
  current-stage work items early, speculatively evaluate candidates at the
  next stage's depth. Cache results so the next stage can skip already-
  evaluated items. This overlaps stages and hides latency.

---

## 25-Minute Time Control (max 15 minutes per PEG turn)

### Target: correct answer 99%+ of the time, optimal spread tiebreaking

With 15 minutes and 8+ threads, we can afford 4+ endgame stages with
moderate candidate sets. The challenge shifts from "find the winner" to
"correctly rank tied candidates by spread."

#### Near-term

- **Full pipeline**: Greedy -> 1-ply (top-64) -> 2-ply (top-32) -> 3-ply
  (top-16) -> 4-ply (top-8). Each stage refines win% and spread estimates.
  Total endgame solves: ~120 x 8 scenarios x 4 stages = ~3840, but
  cutoff pruning eliminates ~60% of these.

- **Margin-based scenario skipping**: After 1-ply, record per-scenario
  spreads. In later stages, skip deep evaluation of scenarios with
  |spread| > 30 (clearly decided). Only re-evaluate close scenarios.
  Cuts endgame solves by ~50%.

- **Shared transposition table with 50%+ of RAM**: At 4-ply, TT hit rates
  are high because different candidates' endgames share subtrees. A large
  TT (4-8 GB) amortizes computation across candidates and scenarios.

- **Per-stage aspiration windows**: Use the previous stage's value as the
  center of a narrow window (+/-15) for the next stage. Falls back to full
  width on fail-high/low. Typically 2-3x faster than full-width search.

#### Big brain

- **Incremental stage promotion**: Don't re-evaluate scenarios whose outcome
  is stable across ply depths. If a scenario gave +45 at 1-ply and +42 at
  2-ply, it's clearly won -- skip 3-ply for that scenario. Only re-evaluate
  scenarios within +/-10 of zero. Combined with margin-skipping, this can
  reduce 3-ply and 4-ply work by 70-80%.

- **Move cap for interior endgame nodes**: At 3-ply and 4-ply, interior
  nodes generate all legal moves. Cap to top-15 by score (with TT move
  always included). TT-safe: store capped entries at depth=0 so later
  uncapped searches re-evaluate but still get the best-move hint.

- **Candidate partitioning by tiles played**: Evaluate bag-emptying
  candidates (play >=2 tiles) separately from non-emptying (play 1 tile).
  Bag-emptying plays are simpler (direct endgame) and usually stronger.
  Evaluate them first to set a high cutoff bar that prunes weak
  non-emptying candidates before their expensive recursive evaluation.

#### Galaxy brain

- **Neural network static evaluator for scenario triage**: Train a small
  CNN or transformer on (board, racks, scores) -> P(win) using endgame
  solve results as training data. Use it to classify scenarios into
  "clearly won" / "close" / "clearly lost" before any search. Only run
  endgame solves on "close" scenarios. If the NN is 95% accurate on
  clear outcomes, this eliminates ~70% of endgame solves.

- **NN-guided move ordering at interior endgame nodes**: Replace the
  current score-based move ordering in negamax with an NN that predicts
  "probability this move is the best response." Better ordering means
  earlier cutoffs, dramatically reducing nodes searched.

---

## Post-Mortem: 1-Hour Off-Clock Analysis

### Target: provably optimal for 1-bag, near-optimal for 2-bag

With an hour and 8 threads, we can afford exhaustive 6-8 ply search on the
final candidates and extend to 2-bag positions.

#### Near-term

- **Deep pipeline**: 6-8 endgame stages on 1-bag with generous candidate
  limits (128 -> 64 -> 32 -> 16 -> 8 -> 4 -> 2). At this depth the endgame
  solver finds the mathematically optimal line.

- **2-bag support**: Extend PEG to handle 2-tile-in-bag positions. This
  multiplies scenario count from ~8 to ~80, but with an hour budget and
  aggressive pruning, it's feasible.

- **Pass evaluation with full recursive solve**: Pass is the most
  expensive candidate (requires inner PEG from opponent's perspective).
  With 1-hour budget, run full multi-stage inner solves instead of the
  greedy approximation used in timed play.

#### Big brain

- **Proof number search for endgame verification**: After the standard
  pipeline finds a candidate, verify it with proof-number search -- a
  best-first algorithm that proves win/loss with minimal node expansions.

- **Monte Carlo endgame sampling for 2-bag triage**: For 2-bag with ~80
  scenarios, sample 20 random scenarios first. Candidates with <25% sample
  win rate are probably losers -- prune before exhaustive evaluation.

- **Retrograde analysis of stuck-tile positions**: Pre-compute a database
  of "stuck tile" endgame outcomes for table lookup.

---

## Summary: Search Depth by Budget

| Budget | Greedy | Endgame stages | Candidates | Key enablers |
|--------|--------|----------------|------------|--------------|
| 60s | Skip or fast | 1-2 ply | 8-16 | First-win, cutoff pruning, 8 threads |
| 15 min | Full | 3-4 ply | 32-64 | Margin skipping, aspiration windows, incremental promotion |
| 1 hr | Full | 6-8 ply (1-bag), 3-4 ply (2-bag) | 64-128 | Proof search, MCTS triage, retrograde tables |

## The Nigel Richards Principle

Richards likely evaluates <10 candidate moves, spending most of his time
on the 2-3 scenarios that could flip the outcome. His "intuition" is
pattern matching trained over millions of games -- effectively a neural
network running on biological hardware.

The engineering path to replicating this:
1. **Learn which candidates matter**: A policy network that assigns
   probability mass to the top-5 candidates
2. **Learn which scenarios matter**: A "criticality" predictor that
   identifies the 2-3 draws where the outcome is closest to flipping
3. **Learn position value without search**: A value network that evaluates
   endgame positions without alpha-beta, accurate to within +/-5 points

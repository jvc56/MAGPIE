# PEG Solving Strategies by Time Budget

## Context

A 1-in-the-bag PEG solve evaluates ~600 candidate moves across ~8 bag-tile
scenarios with a multi-stage pipeline: greedy playout, then 1-ply, 2-ply, ...
endgame refinement passes that progressively narrow the candidate set.

The core cost equation is:
```
total_time ≈ candidates × scenarios × cost_per_endgame_solve × num_stages
```

Nigel Richards doesn't brute-force this. He prunes ruthlessly — probably
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

- **Aggressive greedy pruning**: Top-16 from greedy → 1-ply, top-8 → 2-ply.
  Selfplay calibration data suggests the winner is in the greedy top-8 about
  95% of the time for 1-bag positions.

- **First-win optimization**: Use narrow-window search (α=-1, β=+1) for
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
  tiles played (longest first — bag-emptying plays are strongest in endgame),
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

- **Full pipeline**: Greedy → 1-ply (top-64) → 2-ply (top-32) → 3-ply
  (top-16) → 4-ply (top-8). Each stage refines win% and spread estimates.
  Total endgame solves: ~120 × 8 scenarios × 4 stages = ~3840, but
  cutoff pruning eliminates ~60% of these.

- **Margin-based scenario skipping**: After 1-ply, record per-scenario
  spreads. In later stages, skip deep evaluation of scenarios with
  |spread| > 30 (clearly decided). Only re-evaluate close scenarios.
  Cuts endgame solves by ~50%.

- **Shared transposition table with 50%+ of RAM**: At 4-ply, TT hit rates
  are high because different candidates' endgames share subtrees. A large
  TT (4-8 GB) amortizes computation across candidates and scenarios.

- **Per-stage aspiration windows**: Use the previous stage's value as the
  center of a narrow window (±15) for the next stage. Falls back to full
  width on fail-high/low. Typically 2-3x faster than full-width search.

#### Big brain

- **Incremental stage promotion**: Don't re-evaluate scenarios whose outcome
  is stable across ply depths. If a scenario gave +45 at 1-ply and +42 at
  2-ply, it's clearly won — skip 3-ply for that scenario. Only re-evaluate
  scenarios within ±10 of zero. Combined with margin-skipping, this can
  reduce 3-ply and 4-ply work by 70-80%.

- **Move cap for interior endgame nodes**: At 3-ply and 4-ply, interior
  nodes generate all legal moves. Cap to top-15 by score (with TT move
  always included). TT-safe: store capped entries at depth=0 so later
  uncapped searches re-evaluate but still get the best-move hint.

- **Candidate partitioning by tiles played**: Evaluate bag-emptying
  candidates (play ≥2 tiles) separately from non-emptying (play 1 tile).
  Bag-emptying plays are simpler (direct endgame) and usually stronger.
  Evaluate them first to set a high cutoff bar that prunes weak
  non-emptying candidates before their expensive recursive evaluation.

#### Galaxy brain

- **Neural network static evaluator for scenario triage**: Train a small
  CNN or transformer on (board, racks, scores) → P(win) using endgame
  solve results as training data. Use it to classify scenarios into
  "clearly won" / "close" / "clearly lost" before any search. Only run
  endgame solves on "close" scenarios. If the NN is 95% accurate on
  clear outcomes, this eliminates ~70% of endgame solves.

  Architecture: The input is compact (15×15 board + 2 racks + 2 scores +
  who's on turn ≈ 250 features). A 3-layer MLP with 256 hidden units
  could handle this in <1ms per evaluation. Training data: 100K positions
  from selfplay with ground-truth outcomes.

- **NN-guided move ordering at interior endgame nodes**: Replace the
  current score-based move ordering in negamax with an NN that predicts
  "probability this move is the best response." Better ordering means
  earlier cutoffs, dramatically reducing nodes searched. The TT best-move
  hint already does this for revisited positions; the NN extends it to
  novel positions.

---

## Post-Mortem: 1-Hour Off-Clock Analysis

### Target: provably optimal for 1-bag, near-optimal for 2-bag

With an hour and 8 threads, we can afford exhaustive 6-8 ply search on the
final candidates and extend to 2-bag positions.

#### Near-term

- **Deep pipeline**: 6-8 endgame stages on 1-bag with generous candidate
  limits (128 → 64 → 32 → 16 → 8 → 4 → 2). At this depth the endgame
  solver finds the mathematically optimal line.

- **2-bag support**: Extend PEG to handle 2-tile-in-bag positions. This
  multiplies scenario count from ~8 to ~80, but with an hour budget and
  aggressive pruning, it's feasible. Key challenge: non-bag-emptying
  candidates require recursive 1-PEG sub-solves, each taking 5-30s.

- **Pass evaluation with full recursive solve**: Pass is the most
  expensive candidate (requires inner PEG from opponent's perspective).
  With 1-hour budget, run full multi-stage inner solves instead of the
  greedy approximation used in timed play.

#### Big brain

- **Proof number search for endgame verification**: After the standard
  pipeline finds a candidate, verify it with proof-number search — a
  best-first algorithm that proves win/loss with minimal node expansions.
  Proof numbers naturally focus on the critical variations. If the proof
  completes, the result is mathematically certain.

- **Monte Carlo endgame sampling for 2-bag triage**: For 2-bag with ~80
  scenarios, sample 20 random scenarios first. Candidates with <25% sample
  win rate are probably losers — prune before exhaustive evaluation.
  Re-evaluate survivors exhaustively with the time saved.

- **Retrograde analysis of stuck-tile positions**: Pre-compute a database
  of "stuck tile" endgame outcomes. When the opponent has tiles that can't
  be played (e.g., V, Q without U), the game tree simplifies dramatically.
  A retrograde database indexed by (stuck_tiles, rack_values, spread) could
  replace full search for these positions with a table lookup.

#### Galaxy brain

- **MCTS-guided candidate discovery**: Instead of generating all ~600 legal
  moves and evaluating them, use Monte Carlo Tree Search with endgame
  playouts to discover promising candidates. MCTS naturally focuses on
  moves that lead to good outcomes, effectively pruning the candidate set
  without ever generating the full move list. After MCTS identifies the
  top-20 candidates, switch to exact PEG for verification.

  The MCTS playout policy: play the highest-scoring move (greedy), with a
  small probability of playing the 2nd or 3rd best (exploration). Each
  playout takes ~0.1ms. 10,000 playouts per candidate × 600 candidates =
  60s — feasible as a pre-filter before the hour-long exact analysis.

- **Endgame tablebase for common rack configurations**: Pre-compute
  exact endgame values for the most common late-game rack pairs (e.g.,
  {A,E,I} vs {R,S,T} on various board states). The board component is
  too large to enumerate, but racks can be abstracted: group rack pairs
  by (total_tiles, total_value, has_blank, stuck_tile_count) and store
  the average endgame outcome. This gives an instant evaluation for
  common configurations, with full search only for unusual racks.

---

## Post-Mortem: 8-Hour Deep Analysis

### Target: provably optimal for 1-bag and 2-bag, exploratory 3-bag

This is the regime where we can throw everything at the problem and
potentially push into 3-bag territory.

#### Near-term

- **Exhaustive 2-bag with all optimizations**: Full 4-stage pipeline
  on all ~80 scenarios with 4+ ply endgame. With 8 hours and 8 threads
  (230,400 thread-seconds), even conservative estimates allow 80 scenarios
  × 16 candidates × 4 stages × 4-ply ≈ 5000 endgame solves, each
  averaging ~20s. Margin-based skipping and incremental promotion reduce
  this to ~1500 actual solves.

- **Multi-PV endgame for spread analysis**: Run endgame solver with
  num_top_moves=5 to get the top-5 lines for each scenario. This reveals
  not just the best move but the margin of error — how much worse the
  2nd-best line is. Useful for understanding position sensitivity.

- **Full pass evaluation tree**: Expand the pass evaluation recursion to
  full depth: mover passes → opponent's best PEG response → if opponent
  passes, game ends; if opponent plays, mover's PEG response → ...
  Continue until pass-back or convergence. Current implementation limits
  recursion to prevent infinite pass-back; with 8 hours, allow deeper
  exploration.

#### Big brain

- **3-bag exploratory solving**: With 3 tiles in the bag and ~700 ordered
  draw triples, exhaustive analysis is borderline. Strategy: use 2-PEG as
  a subroutine but with aggressive settings (skip-greedy, first-win-only,
  top-4 candidates). Target: find the 3-bag winner for positions where
  the answer is clear (one dominant candidate). Punt on close positions.

  Feasibility: 700 scenarios × 8 candidates × 1 stage (greedy + 1-ply) ≈
  5600 sub-solves. If each 2-PEG sub-solve averages 30s with all
  optimizations, total = 47 hours single-threaded → ~6 hours with 8
  threads. Tight but possible for a single position.

- **Opponent modeling via selfplay statistics**: Instead of assuming the
  opponent plays optimally (minimax), model the opponent as a strong
  but imperfect player based on selfplay data. For each opponent response
  in the recursive evaluation, weight by the probability that a strong
  engine would choose that response (estimated from thousands of selfplay
  games). This "soft minimax" can change the optimal candidate in positions
  where the minimax-optimal play is only best against a perfect opponent
  but loses to likely imperfect responses.

- **Endgame opening book**: For common late-game board patterns (e.g.,
  triple-word-score lanes, hook-heavy edges), pre-compute endgame strategy
  templates. When a position matches a known pattern, use the template to
  seed the search with strong initial move ordering and aspiration windows.
  This is analogous to chess opening books but for the endgame.

#### Galaxy brain

- **Neural network position evaluator replacing greedy playout**: Train
  a deep network on millions of endgame positions to predict the exact
  spread from a position without any search. Use this as the leaf
  evaluator in negamax instead of greedy playout.

  Current greedy playout: ~0.5ms per call, heuristic accuracy.
  NN evaluator: ~0.1ms per call (GPU batch inference), trained accuracy.

  Architecture: ResNet or transformer encoder on the 15×15 board with
  rack embeddings. Training data: 10M positions from deep endgame solves
  (spread as target). The NN sees patterns that greedy playout misses:
  tempo, tile synergy, blocking value, stuck-tile dynamics.

  Impact: With a strong NN leaf evaluator, 2-ply search with NN leaves
  could match 4-ply search with greedy leaves — cutting search time by
  10-50x while maintaining accuracy. This is the single highest-impact
  optimization possible but requires significant ML infrastructure.

- **AlphaZero-style self-play for PEG**: Combine MCTS with a value/policy
  neural network, trained via self-play on pre-endgame positions. The
  policy network learns which candidate moves to evaluate (replacing the
  brute-force "generate all legal moves" approach). The value network
  learns position evaluation without search. The MCTS component handles
  the stochastic element (unknown bag tile) naturally via chance nodes.

  Training loop:
  1. Generate 1-bag positions from selfplay
  2. Run MCTS+NN to select moves (initially random network)
  3. Play out games to get win/loss outcomes
  4. Train NN on (position, MCTS visit counts, game outcome)
  5. Repeat with the improved network

  After sufficient training, the MCTS+NN agent could solve 1-bag positions
  in milliseconds (a few hundred MCTS iterations), enabling real-time
  PEG during 3-minute games. The exact PEG solver becomes a verification
  tool rather than the primary decision-maker.

- **Transfer learning from endgame to pre-endgame**: An NN trained on
  bag-empty endgame positions can be fine-tuned for 1-bag positions by
  adding an expectation layer over the ~8 possible draws. The endgame
  NN provides the per-scenario evaluation; a thin aggregation layer
  computes the expected win% across draws. This requires far less
  training data than learning the full PEG problem from scratch.

---

## Summary: Search Depth by Budget

| Budget | Greedy | Endgame stages | Candidates | Key enablers |
|--------|--------|----------------|------------|--------------|
| 60s | Skip or fast | 1-2 ply | 8-16 | First-win, cutoff pruning, 8 threads |
| 15 min | Full | 3-4 ply | 32-64 | Margin skipping, aspiration windows, incremental promotion |
| 1 hr | Full | 6-8 ply (1-bag), 3-4 ply (2-bag) | 64-128 | Proof search, MCTS triage, retrograde tables |
| 8 hr | Full | 8+ ply (1-bag), 6 ply (2-bag), exploratory 3-bag | All | NN evaluation, opponent modeling, selfplay-trained policy |

## The Nigel Richards Principle

Richards likely evaluates <10 candidate moves, spending most of his time
on the 2-3 scenarios that could flip the outcome. His "intuition" is
pattern matching trained over millions of games — effectively a neural
network running on biological hardware.

The engineering path to replicating this:
1. **Learn which candidates matter**: A policy network that assigns
   probability mass to the top-5 candidates (bypassing 595 non-starters)
2. **Learn which scenarios matter**: A "criticality" predictor that
   identifies the 2-3 draws where the outcome is closest to flipping
3. **Learn position value without search**: A value network that evaluates
   endgame positions without alpha-beta, accurate to within ±5 points

Each of these can be trained on the exact PEG solver's output as ground
truth. The solver generates the training data; the NN distills it into
fast inference. This is the same paradigm that AlphaZero used to go from
"search everything" to "search almost nothing."

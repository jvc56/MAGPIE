# 1PEG move chooser with reply features, 2026-10-14

A 1PEG is a position with one tile in the bag: every placement draws it, and
the mover cannot see which of the 8 unseen tiles it is. The previous one-pass
1PEG network (2026-10-13, 2,816 depth-2-labelled boards) did not beat static
equity: 49.9% seat-swapped win score and −0.7 points per pair with a static
endgame continuation (50.4% and −0.9 with a two-ply one). This study found
why, rebuilt the chooser, and evaluated it on boards never used for training or
model choice.

## Summary
Every chooser was rescored with MAGPIE's exact endgame solver after the 1PEG
move (see [Continuation](#continuation)). Exact numbers are on the first 547
boards of a seeded order of fresh set 2, an unbiased sample. 1,742 of the
1,745 solves are proven; three were stopped at depth 9–11 after 2.9–12.5 hours,
and their depth-limited values are used (a W/T/L change would need a swing of
120 points or more).

- **Yes:** under exact play every network beats static per 1PEG decision, and
  the networks sit between static and MAGPIE's PEG in cost.
- **Recommended default (unchanged):** `cascade-u`, about 3 ms per decision:
  **+3.3% wins [+1.7, +4.9], +6.4 points [+4.8, +8.1]** over static.
- **Greedy seed:** the earlier qualification was an artifact of the static
  continuation. Under exact play greedy over all moves gains +0.0%
  [−2.0, +2.0], −0.9 points, and greedy top-64 +0.6% [−1.3, +2.6], +0.3 points.
  Every network beats both greedy variants on points (+5.6 to +8.2 per decision,
  CIs above zero) and beats greedy over all moves on wins (CIs above zero).
  `cascade-u` gains +3.3% [+1.5, +5.2] wins and +7.3 points per decision over
  greedy over all moves, at 7–84× less cost.
- **MAGPIE's default PEG** (first 85 boards): +2.4% [−2.4, +7.1] wins,
  +10.6 [+5.2, +16.8] points over static. `cascade-u` vs PEG: +1.8%
  [−2.4, +5.9] wins, −1.4 [−4.0, +1.3] points, no detectable difference, at
  3 ms against minutes per decision (82 s median, 359 s mean on one thread).
- **Full games** (20,000 seat-swapped pairs, 25.9% reach a 1PEG) were not rerun
  with the exact solver. Quote the two-ply result, `cascade-u` 50.31%
  [50.25, 50.38]: two-ply agreed with exact on 99.1% of W/T/L outcomes, 1.5
  points optimistic. The static-endgame 50.36% should not be quoted.
- **Utility vs margin:** under exact play choosing by utility instead of
  expected margin with the same networks makes no measurable difference
  (−0.2% to +0.5% wins, CIs include zero; −0.4 to −0.5 points, CIs include
  zero).

## Information rule
Every chooser ranks the mover's candidates from its own rack and public
information only. The unseen pool is the tile distribution minus the board minus
the mover's rack. Each possible last tile is enumerated, weighted by its unseen
count, and its reply features are aggregated *before* the single forward pass per
candidate. The actual last tile is consumed only when the chosen move is played.
Every fresh decision was rerun with the actual last tile swapped for a tile on the
opponent's rack; inputs had to be bitwise identical and the choice the same. All
checks passed (MAGPIE's greedy seed: on the first 1,024 boards; MAGPIE's
default PEG: 4 boards, same pick).

## Why the previous network failed
- **Objective.** 98.4% of the variance of the depth-2 candidate labels is between
  boards (is this position won?). Only 1.6% is between candidates of the same
  board, which is all a move chooser needs. The old loss (absolute margin Huber +
  aggregate WDL) learned board value. Its validation margin regret, 8.03 points,
  was worse than static's 7.94. Root-demeaned and listwise training on the same
  inputs reached 5.2–6.0.
- **Representation.** The 72 public scalars carry no board information, so the
  dominant 1PEG effect — the opponent's outplays and bingo lanes, and blocking
  them — was invisible. A zero-learning one-ply rule (spread after the move minus
  the expected opponent best reply, including twice the mover's rack for an
  outplay) alone cut static's regret roughly in half (3.89 vs 7.70 over all
  stored boards).
- **Leaves.** Ranking by score alone beats static score + KLV in 1PEGs
  (7.36 vs 7.94 validation margin regret).
- Labels, score perspective, multiplicity aggregation and the native/Python
  agreement of the old study were correct.

## Method
- **Labels** (from the 2026-10-13 study): 2,816 boards from static self-play
  (NWL23), the static top 64 placements of each (179,803 candidates), and a
  depth-2 endgame search for every candidate and possible last tile (1,226,998
  scenarios). Per-tile margin, W/T/L and multiplicity were kept.
- **Per-tile reply features**, computed after playing the candidate in each
  hypothetical world:
  - Opponent's best reply: score, plus twice the mover's rack when it goes out;
    the outplay flag, tiles played and pass flag. The highest-scoring reply is
    found incrementally: the opponent's three best plays on the pre-move board
    are generated once per world, a play in a lane the candidate does not touch
    keeps its score, and only touched lanes are searched (seeded with it).
    This matched a full search in all 28,386 benchmark cases and was 8× faster
    than a full greedy (BEST_SMALL) search with a warm cache.
  - Mover's greedy follow-up on the post-move board: score, plus twice the
    opponent's rack when going out; the outplay flag (strong tiers only).
  - Per candidate: multiplicity-weighted mean, standard deviation, minimum and
    maximum of each feature over the possible last tiles, the one-ply spread
    estimate, 68 public candidate scalars (the value-net scalars without the four
    history entries), static equity and the spread after the move.
- **Model:** a 1×128 ReLU MLP, one forward pass per candidate, 46–64 KB.
  Loss: root-demeaned Huber on the expected margin plus a listwise softmax
  cross-entropy (temperature 4). AdamW, 25 epochs, trained on all 2,816 boards
  and frozen before any fresh evaluation.
- **Choosers**
  - `fast`: reply features for the static top 32.
  - `strong`: reply + mover follow-up features for the static top 64.
  - `cascade`: `fast` ranks the top 32; `strong` rescores its top 4.
  - `-u` (utility mode): the network also predicts the win probability and the
    absolute expected spread. It chooses by MAGPIE's `sim_utility_blend`
    (w_win 1, w_spread 0.5, spread scale 100), so spread breaks win ties. Extra
    inputs: the weighted share of possible last tiles whose one-ply (and, with
    mover features, crude two-ply) spread estimate exceeds fixed thresholds.
  - Without a legal placement the mover passes. No pass policy was learned;
    static never ranked a pass first on 8,192 fresh boards, and no forced pass
    occurred in 5,174 full-game 1PEG decisions. The 8,192 boards checked are
    fresh set 1 plus the labelled set.

## Evaluation protocol
- Feature and model choices used 4-fold grouped CV and out-of-fold completed
  games on the 2,816 stored boards only.
- **Fresh boards** (none equal to any stored or earlier 1PEG board, all distinct),
  generated by the same static self-play procedure:

  | set | seed | boards | used for |
  |---|---|---:|---|
  | labelled | 202610141501 | 512 | depth-2 labels, prediction metrics, depth-4 teacher check |
  | fresh set 1 | 202610141502 | 7,680 | margin-mode choosers |
  | full games | 202610141503 | 20,000 games | full-game pairs |
  | fresh set 2 | 202610141504 | 8,192 | utility mode, greedy comparison, timing |

- **Completed games:** each chooser's move is played with the board's actual last
  tile, then a common continuation finishes the game (see below). Results are the
  per-decision change vs static from the mover's view; the seat-swapped pair win
  score is 50% plus half the win change. 95% CIs from 10,000 bootstrap resamples
  of boards.

## Continuation
- **Exact (primary).** After the move, one `endgame_solve` from the post-move
  position: 25 plies, one thread, no time limit, one transposition table per
  process reused across solves. The mover's final spread is the current spread
  minus the solver's value if the opponent is on turn, plus it if the mover is.
  A value is proven when the solve finishes below 25 plies. Solve times are
  heavy-tailed: median 0.7 s, p90 140 s, p99 103 min; the longest proven solve
  took 10.7 h. Three picks were stopped instead: board 2562 pick 0 at depth 11
  after 4.6 h, board 4328 pick 24 at depth 11 after 12.5 h, and board 4328 pick
  29 at depth 9 after 2.9 h. Each uses the value of its deepest completed
  iterative-deepening search, read from the running process, and is marked
  unproven. Their final spreads are −186, +120 and +122 for the mover, so W/T/L
  is not in doubt.
- **After a pass** the tile stays in the bag. The player then on turn plays
  MAGPIE's default PEG pick until the bag is empty, then the exact solve.
  Earlier continuations: greedy-seed scoring had the opponent play static top 1
  at its own 1PEG, with the two-ply variant resuming once the bag was empty;
  the network and static runs never continued after a pass (candidates were
  placements only and no forced pass occurred). 11 of 547 greedy-all picks are
  passes; no network or PEG pick is.
- **Static top 1 and fixed two-ply (secondary)** were used for the original
  results. Against exact play over 1,449 (board, move) pairs here, static was
  7.9 points and 4.6% wins optimistic and wrong on 6.4% of W/T/L outcomes;
  two-ply was 1.5 points and 0.8% wins optimistic and wrong on 0.9%. Because
  each chooser is compared with static on the same continuation, most of that
  bias cancels, except for the greedy seed, whose playouts simulate the static
  continuation.
- An independent cloud check with stand-in choosers reached the same
  conclusions:
  [notes/peg1-continuation-20261014](https://github.com/jvc56/MAGPIE/blob/claude/peg1-continuation-check/notes/peg1-continuation-20261014/REPORT.md).
  The two harnesses were not compared position by position.

## Results
### Cost per decision
Isolated single process, policy PGO build, 512 fresh boards, warm page cache, ms:

| chooser | mean | median | p90 | p99 |
|---|---:|---:|---:|---:|
| static top 1 | 0.042 | 0.034 | 0.068 | 0.157 |
| fast | 2.30 | 1.94 | 3.57 | 7.85 |
| fast-u | 2.31 | 1.96 | 3.60 | 7.95 |
| cascade | 3.11 | 2.57 | 5.12 | 10.1 |
| **cascade-u** | **3.15** | 2.63 | 5.20 | 10.4 |
| strong | 18.3 | 12.0 | 37.0 | 91.4 |
| strong-u | 18.4 | 12.0 | 37.2 | 92.1 |
| greedy seed, static top 64 | 23.4 | 18.7 | 41.2 | 71.1 |
| greedy seed, all moves | 264.5 | 108.8 | 598.6 | 2,365 |
| depth-2 search, 64 candidates × every last tile | ~32,000–35,000 | | | |

An earlier run on fresh set 1 broke down `fast` (2.27 ms): opponent replies
1.72, building the hypothetical worlds 0.27, move generation 0.19, network 0.07.

### 1PEG decisions under exact play
Fresh set 2, the first 547 boards of a seeded order (seed 202610141801). Change
per decision vs static [95% CI]:

| chooser | wins | points |
|---|---:|---:|
| fast | +2.65% [0.9, 4.4] | +6.63 [4.9, 8.4] |
| fast-u | +2.38% [0.7, 4.1] | +5.89 [4.2, 7.6] |
| cascade | +2.56% [0.7, 4.5] | +7.25 [5.5, 9.0] |
| **cascade-u** | **+3.29%** [1.7, 4.9] | **+6.39** [4.8, 8.1] |
| strong | +2.93% [1.1, 4.8] | +7.12 [5.3, 8.9] |
| strong-u | +3.11% [1.3, 4.9] | +6.28 [4.5, 8.1] |
| greedy seed, top 64 | +0.64% [−1.3, 2.6] | +0.27 [−1.7, 2.3] |
| greedy seed, all moves | +0.00% [−2.0, 2.0] | −0.91 [−3.1, 1.3] |

On the same boards the two-ply continuation gives `cascade-u` +3.47% / +6.39
and greedy over all moves +0.82% / −0.05; the static continuation gives
+5.03% / +7.58 and +6.86% / +8.29. Adding the 17 later boards whose picks were
also complete (564 boards, biased toward quick solves) changes no entry in this
table by more than 0.2% wins or 0.2 points.

Networks vs greedy seed under exact play, per decision:

| network | vs greedy top 64: wins | points | vs greedy all: wins | points |
|---|---:|---:|---:|---:|
| fast | +2.01% [0.1, 4.0] | +6.35 | +2.65% [0.6, 4.8] | +7.54 |
| fast-u | +1.74% [−0.1, 3.6] | +5.62 | +2.38% [0.5, 4.3] | +6.81 |
| cascade | +1.92% [0.0, 3.9] | +6.98 | +2.56% [0.6, 4.7] | +8.16 |
| **cascade-u** | +2.65% [1.0, 4.5] | +6.12 | +3.29% [1.5, 5.2] | +7.31 |
| strong | +2.29% [0.6, 4.1] | +6.85 | +2.93% [1.0, 4.9] | +8.04 |
| strong-u | +2.47% [0.9, 4.1] | +6.01 | +3.11% [1.4, 4.9] | +7.19 |

Every points CI in this table is above zero. The margin-mode utility networks
(`fast-u`, `cascade-u`, `strong-u` choosing by expected margin) are in
`SUMMARY.json`. On wins, all nine networks beat greedy over all moves with CIs
above zero; against greedy top 64, six do, `cascade` and margin-mode `fast-u`
have CIs starting at 0.0, and `fast-u`'s includes zero.

MAGPIE's default PEG schedule, first 85 boards of the same order (static and
PEG pick only; the networks' picks are from the fresh-set-2 run on the same
boards):

| | wins | points |
|---|---:|---:|
| PEG vs static | +2.35% [−2.4, 7.1] | +10.64 [5.2, 16.8] |
| `cascade-u` vs PEG | +1.76% [−2.4, 5.9] | −1.38 [−4.0, 1.3] |
| `strong-u` vs PEG | +1.76% [−2.4, 5.9] | −0.39 [−2.7, 2.1] |
| `cascade` vs PEG | −2.35% [−5.9, 0.0] | −0.62 [−3.6, 2.6] |
| greedy all vs PEG | −2.35% [−7.1, 2.4] | −5.92 [−9.0, −2.8] |

PEG differs from static on 61% of these boards and takes 359 s mean, 82 s
median, 663 s p90 and up to 7,002 s per decision on one thread (timed on a busy
machine, so indicative only).

### 1PEG decisions, earlier continuations (fresh set 2, 8,192 boards)
Secondary: the static-endgame columns are biased and the greedy seed's
static-endgame numbers are an artifact (see [Continuation](#continuation)).
Change per decision vs static [95% CI]:

| chooser | static endgame: wins | points | two-ply endgame: wins | points |
|---|---:|---:|---:|---:|
| fast | +2.37% [1.92, 2.82] | +6.10 | +2.41% [2.00, 2.84] | +5.97 |
| fast-u | +2.66% [2.21, 3.10] | +5.50 | +2.47% [2.05, 2.90] | +5.21 |
| cascade | +2.82% [2.36, 3.28] | +6.86 | +2.73% [2.31, 3.16] | +6.48 |
| **cascade-u** | **+3.34%** [2.88, 3.80] | +6.34 | **+2.93%** [2.50, 3.37] | +5.79 |
| strong | +3.01% [2.52, 3.49] | +7.61 | +2.58% [2.15, 3.02] | +6.53 |
| strong-u | +4.11% [3.60, 4.63] | +7.09 | +2.95% [2.50, 3.41] | +5.69 |
| greedy seed, top 64 | +5.25% [4.64, 5.85] | +6.75 | +1.54% [1.01, 2.08] | +0.26 |
| greedy seed, all moves | +6.04% [5.40, 6.69] | +6.62 | +1.23% [0.68, 1.79] | −1.56 |

Fresh set 1 (7,680 other boards, margin mode) agrees. Static endgame: fast
+2.57% / +5.82, cascade +2.91% / +6.62, strong +3.39% / +7.42. Two-ply endgame:
+2.19% / +5.60, +2.50% / +6.18, +2.69% / +6.34. All CIs exclude zero.

### Utility mode vs margin mode
Under exact play, fresh set 2 (547 boards), each utility network choosing by
utility vs the same network choosing by expected margin, per decision:

| network | wins | points |
|---|---:|---:|
| fast | −0.18% [−1.4, 1.0] | −0.49 [−1.3, 0.3] |
| cascade | +0.46% [−0.4, 1.4] | −0.39 [−1.1, 0.4] |
| strong | +0.00% [−1.0, 0.9] | −0.53 [−1.2, 0.2] |

Earlier continuations, all 8,192 boards:

| network | static endgame: wins | points | two-ply endgame: wins | points |
|---|---:|---:|---:|---:|
| fast | +0.17% [−0.13, 0.47] | −0.40 | +0.03% [−0.25, 0.31] | −0.41 |
| cascade | +0.52% [0.22, 0.83] | −0.47 | +0.32% [0.06, 0.59] | −0.51 |
| strong | +1.03% [0.70, 1.35] | −0.54 | +0.48% [0.19, 0.78] | −0.68 |

On stored boards even the depth-2 teacher shows why spread must break ties. Pure
win% ranking left 6.36 points of margin regret; a spread weight of 0.02 cut that
to 0.83 at no win cost. The utility heads predict the expected spread to within
4.6 points (strong) and 5.4 points (fast) mean absolute error, against 12.6 for
the old network.

### Full games
20,000 seat-swapped pairs from the opening, static play outside the 1PEG.
Network player's win score [95% CI] and points per game:

| chooser | static endgame | | two-ply endgame | |
|---|---:|---:|---:|---:|
| fast | 50.22% [50.15, 50.29] | +0.70 | 50.25% [50.19, 50.31] | +0.70 |
| fast-u | 50.29% [50.22, 50.36] | +0.67 | 50.30% [50.23, 50.36] | +0.64 |
| cascade | 50.29% [50.22, 50.36] | +0.82 | 50.29% [50.22, 50.35] | +0.78 |
| **cascade-u** | **50.36%** [50.29, 50.43] | +0.76 | **50.31%** [50.25, 50.38] | +0.72 |
| strong | 50.29% [50.21, 50.36] | +0.91 | 50.29% [50.23, 50.36] | +0.81 |
| strong-u | 50.42% [50.34, 50.50] | +0.86 | 50.33% [50.26, 50.41] | +0.74 |

The effect is diluted because only 25.9% of games pass through a 1PEG.

### Prediction on fresh labelled boards
512 boards; regret is against the depth-2 aggregate labels over all 64
candidates [95% CI]:

| policy | margin regret | win regret |
|---|---:|---:|
| static | 7.86 [6.80, 8.96] | 0.048 |
| score only | 7.14 [6.21, 8.11] | 0.039 |
| fast | 2.15 [1.82, 2.49] | 0.023 |
| strong | 1.61 [1.30, 1.95] | 0.020 |
| cascade | 1.42 [1.17, 1.68] | 0.018 |
| depth-2 teacher, margin first | 0 | 0.015 |
| depth-2 teacher, win first | 1.08 [0.66, 1.56] | 0 |

### Teacher quality
256 fresh labelled boards were relabelled with a 4-ply search for the static
top 6 plus every policy's choice.
- Depth-2 and depth-4 pick the same best move on 87.9% of boards; their
  within-board correlation is 0.991.
- Depth-2 is 2.0 points optimistic for the mover on average.
- Under depth-4 labels, margin regret is 7.72 for static, 1.33 for cascade,
  1.48 for strong, 1.90 for fast, and 0.27 for the depth-2 teacher's own picks.

The approximate teacher is adequate; the remaining gap is in the model, so no
deeper relabelling (25–100 CPU-hours) was done.

## What did not help, and headroom
- **More data:** the learning curve with reply features is flat. Margin regret
  went from 1.81 at 264 training boards to 1.64 at 2,112.
- **Other designs:** per-tile models matched the one-pass aggregate models. The
  old KLV/stuck extras improved margin regret by only 0.05–0.17. Full
  greedy-playout features improved it by about 0.25 at about 1.4× the cost. Win-aware second-stage heads alone
  gave no credible gain; the blended utility mode did.
- **Headroom left:** under exact play on the first 87 fresh labelled boards,
  the depth-2 teacher's win-first picks gain +5.75% [0.0, 12.6] wins,
  +3.68 points over static and margin-first picks +4.60% [1.1, 9.2], +3.87.
  `cascade` gains +4.60% [1.1, 9.2], +4.22 on the same boards: it trails
  win-first picks by 1.15% [−5.8, +2.3] wins and leads by 0.54 points, so any
  remaining headroom is not measurable at this size. The earlier out-of-fold
  figures (+4.1% win-first vs +3.0% margin-first) used the static endgame.

## Caveats
- Exact play covers 547 of the 2,048 planned fresh-set-2 boards (1,745 picks),
  85 of 256 PEG boards and 87 of 512 labelled boards; the PEG and labelled runs
  are paused and can resume. Three of the 1,745 fresh-set-2 values are
  depth-limited (see [Continuation](#continuation)). Solve and PEG timings were
  taken on a shared, oversubscribed machine.
- Labels are depth-2 searches. Hidden tiles are weighted uniformly by public
  count, with no inference about the opponent's rack.
- Full games use static play outside the 1PEG. Sims and production were not
  changed or tested. Inside static-rollout sims the endgame after a 1PEG is
  played greedily, which is where the greedy seed is strongest, so measure in
  sims before choosing a rollout policy.
- The greedy seed ran with one thread, full draw enumeration, the rational
  opponent and no nested lookahead (pass candidates take one greedy rollout).
- The chooser code is research-harness code (`test/value_net_test.c` modes
  `peg1feat2`, `peg1net2`, `peg1cascade`, `peg1util`, `peg1games`,
  `peg1games3`, `peg1play`, `peg1pos`, `peg1greedy`, `peg1deep`, `peg1bench`,
  `peg1exact`) in
  the experiment's source copy. It is not merged.

## Reproduction
- Everything is in
  `/Volumes/T5 EVO/magpie-distill/inference-weights-20261011/peg1-player-20261014/`:
  - `results.txt`; `scripts/` (all analysis); `analysis/` (every JSON output;
    the exact results are `analysis/exact-20261015-547/`, the earlier
    interim snapshot `analysis/exact-interim-20261015/`);
  - `exact/` (exact-continuation plans, per-chunk outputs, binaries and
    hashes; `scripts/peg1exact_block.c` is the harness mode `peg1exact`,
    `scripts/exact_plan.py`, `run_exact.py`, `exact_analyze.py`,
    `exact_aux.py` plan, run and analyze it; `side_exact.py`,
    `harvest_main.py` and the lldb readers `eg_peek.py`, `eg_best.py`,
    `eg_rows.py` with offsets from `eg_offsets.c` handled the long solves;
    `exact/fresh2/DEPTH_LIMITED.json` records the three stopped picks);
    `fresh*/` (boards, choices, games); `native/` (parity and timing);
    `teacher/` (depth-4 labels).
  - `reproduction/harness-vs-full64-source.patch` is the harness diff against
    the 2026-10-13 study's source.
- Checks:
  - Native vs Python: bitwise-identical inputs, scores within 5e-5, no choice
    disagreements.
  - PGO and optimized builds made identical choices and game outcomes.
  - The greedy seed's outcomes matched `peg1play` on all 7,306 shared moves.
- Files in this directory:
  - `SUMMARY.json`: every headline number.
  - `PROTOCOL.json`: seeds, data, information rule and hashes.
  - `models/*.json`: frozen network manifests with SHA-256. The network files
    are in the experiment's `final/`.

# Late-game sims with neural rollouts and endgame search

Branch `claude/peg-nets-main`, 2026-10-21 to 2026-11-01. The experiment
harness is in `test/` (the `pegnet:` modes of `magpie_test`); the `src/` hooks
it needs are listed at the end. The branch is for experiments and is not meant
to be merged into main as it stands.

## Summary

At 7 tiles in the bag, a sim whose rollouts choose their plays with the 1PEG
and 2PEG nets at 1 and 2 tiles in the bag, and with a 2-ply endgame search
once the bag is empty, picks better moves than MAGPIE's static sim:

- **+0.78 judged win%** [+0.43, +1.16] with the same 16,000 rollouts, and
  **+0.63** [+0.26, +1.04] when the static sim gets the same wall time
  (512 boards each, board-bootstrap 95% CIs).
- **The endgame search is what helps.** The nets alone, without it, were not
  significantly better than static (+0.23). A 1-ply search was worse than
  2-ply, and the solver's greedy playout move added nothing over static.
- **Static sims saturate.** Running about 500 times as many rollouts didn't
  measurably improve them (+0.13 [-0.08, +0.32]).
- **Racing makes it affordable.** A new BAI mode, run on the static sim's best
  6 candidates, keeps the quality at about a tenth of the cost: 5.5 s instead
  of 57 s per decision on 10 threads.

A check that plays the rest of the game without the nets is running (see
*Pending*).

## Setup

- **Positions.** Static self-play games (NWL23), each stopped at a turn with
  exactly 7 tiles in the bag (`pegnet:pegnpos`, `PEG_POSITIONS_BAG=7`). The
  main runs use 512 boards.
- **The sim** (`pegnet:pegsim`):
  - Candidates: the static top 16 tile placements.
  - Rollouts: 1,000 per candidate by round robin, to the end of the game
    (25 plies).
  - Pairing: the same seed in every arm, so rollout k of a candidate is the
    same world in every arm.
  - Settings: no inference, no bag cycling; utility weights win% 1, spread 0.5
    at scale 100.
- **Arms.**

  | Arm | Rollout plays |
  |---|---|
  | A (0) | static |
  | B (1) | nets at 1-2 in the bag |
  | C (2) | static, for B's wall time |
  | D (3) | B + endgame search at bag 0 (`PEG_SIM_EG_PLIES`, default 2) |
  | E (4) | static + endgame search |
  | F (5) | static, for D's wall time |

  The rollouts' win and spread always come from the rollout's own world. The
  nets only choose plays; they see neither the bag nor the opponent's rack.
  `PEG_SIM_CHECK=1` checks this.
- **The judge** (`pegnet:peg3oracle` with `PEG_JUDGE_SAMPLES=32`):
  - Deals: 32 uniformly random deals of the 14 unseen tiles per board, shared
    by every pick of that board.
  - Playout: each pick is played out with static plays at 3+ tiles in the bag,
    the nets at 1-2, and an exact endgame (25 plies, 15 s cap).
  - Scoring: boards where two arms pick the same move count as 0 difference.

## Results (512 boards; judged win% and spread, 95% board-bootstrap CI)

| Comparison | Win% | Spread | Same pick | Study |
|---|---|---|---|---|
| D - A | +0.78 [+0.43, +1.16] | +1.04 [+0.58, +1.52] | | main |
| B - A | +0.23 (n.s.) | | | main |
| D - B | +0.54 [+0.24, +0.88] | | | main |
| D - F (equal wall time) | +0.63 [+0.26, +1.04] | +0.78 [+0.29, +1.27] | 342/512 | eqtime |
| F - A (~500x rollouts) | +0.13 [-0.08, +0.32] | +0.28 [-0.03, +0.60] | 441/512 | eqtime |
| 1-ply - 2-ply (P1 - D) | -0.25 [-0.49, -0.06] | -0.21 [-0.45, +0.02] | 437/512 | egply |
| greedy - B (P0 - B) | +0.02 [-0.19, +0.21] | +0.07 [-0.17, +0.33] | 450/512 | egply |

In the main run D was better on 66 boards and worse on 23.

### Cost and where the time goes

- **Per decision at 10 threads.** Full D took 129 s mean (69 s median) on the
  first binary. With a shared transposition table and main's speedups that
  came down to 74 s (41 s), and with a PGO build trained on these sims to
  57 s mean on the live-check boards. A takes 0.2 s.
- **The endgame search dominates.** It is about 87% of D's CPU time; 85% of
  that is the first bag-empty decision, when the racks hold about 14 tiles.
  The nets are the other 13%.
- **A shared transposition table pays off.** One lockless table shared by every
  rollout's endgame search (`PEG_SIM_EG_TT=shared`) cut endgame CPU to 0.70x
  and wall time to 0.74x. Picks and utilities were identical on all 8 test
  boards; a table per worker gave 0.81x.
- **Repetition is low at 7 in the bag.** 92% of rollout trajectories are
  distinct. Caching exact positions would save about 25%.

### Fewer rollouts: prefilters, fixed counts and racing

Every rollout of A and D on the 512 boards was recorded (`PEG_SIM_SAMPLES`).
Sampling schemes were then replayed on the recordings, offline in Python and
through MAGPIE's own BAI (`pegnet:pegsimbai` with `RANDOM_VARIABLES_TABLE`).
New picks were judged as above. Cost is total rollouts as a share of full D's
16,000.

| Scheme | Cost | Win% vs full D |
|---|---|---|
| race2: eliminate at 2 SE (paired), at most 1,000 per candidate | 0.19 | +0.01 [+0.00, +0.03] |
| pre6-race2: race2 on the static sim's best 6 | 0.12 | +0.01 [-0.04, +0.06] |
| pre4-race2: on the best 4 (selected on boards 0-255) | 0.095 | -0.05 [-0.24, +0.11] (boards 256-511, the pre-registered test) |
| MAGPIE BAI racing, prefilter 6 | 0.117 | -0.02 [-0.05, +0.01] |
| fix200: 200 rollouts per candidate | 0.20 | -0.22 [-0.39, -0.02] |

Further findings:
- **Fixed counts lose quality.** Many candidates are near-ties, and a fixed
  count can't tell them apart.
- **GK16 is too strict for this.** MAGPIE's BAI with the GK16 threshold keeps
  the quality at 0.6-0.8 of the cost, with or without elimination. Its
  threshold is about 3.7 SE at 99%, and it has no per-candidate cap, so
  near-ties run to the total cap.

**Live check** (prefilter 6, MAGPIE racing, boards 256-287):
- Cost: 0.097 of full D's wall time (5.5 s vs 56.8 s mean per board), or 0.100
  including the static prefilter sim.
- Picks: equal to full D's on 31 of 32 boards. On the other board the new pick
  judged better: 12.5% wins vs 6.25%.
- Rollouts: all 512,000 static rollouts, and all but 2 of the 54,025 D
  rollouts, are identical to the recorded run's. The 2 exceptions come from
  the shared table: it makes a 2-ply result depend on which searches ran
  earlier.
- Replay: replayed in the order the live sim draws its seeds, the replay gives
  the same picks on 32 of 32 boards. Per-candidate counts are identical on 30;
  the other 2 boards have similar plays (same square and tiles), which the sim
  never eliminates against each other and the replay doesn't model.

The live check also found three harness bugs, all fixed before the final run:
- **Missing rack.** pegsim's candidate list had no rack, so the sim's
  similar-play check treated any two plays of one length on one square as
  similar.
- **Candidate order.** The prefiltered list was in the move list's heap order
  rather than the static sim's ranking.
- **Replay order.** The replays drew rollouts in seed-value order, not the live
  sim's stream order.

The similarity bug affects only BAI-stopped sims, so the earlier
fixed-iteration studies stand.

## Pending: autoplay from 7-in-bag positions

Every verdict above comes from the judge, whose playouts use the nets at 1-2 in
the bag, so the nets judged continuations like their own. The current run
removes the nets from the continuation:
- **Positions:** 1,024 fresh 7-in-bag positions.
- **Picks:** racing D (prefilter 6) vs a static sim given D's whole decision
  time.
- **Playouts:** where the picks differ, both moves are played out in the same
  32 deals: the actual one plus 31 random ones.
- **Continuation:** every later drawing turn of either player is chosen by a
  one-thread static sim (`PEG_JUDGE_SIM=16,1000,25`), which sees neither rack
  nor bag; endgames are exact.
- **Primary test:** D - F win%, lower 95% bound > 0.

Whole-game autoplay would be a weak test of this change. Only about 1 game in
4 reaches exactly 7 tiles in the bag, and the two sims pick differently on a
third of those, so it would take tens of thousands of game pairs.

Not tested yet:
- 5 or 6 tiles in the bag.
- PEG at 4 in the bag against a sim at equal time.
- Real game-pair autoplay.

Every result is for NWL23 at 7 in the bag. Between 17% and 30% of judged deals
hit the 15 s endgame cap; such a solve keeps its last completed depth.

## Reproducing

`dataset/` holds the 512 boards at 7 in the bag with every arm's per-candidate
values and the judge results, the 2-in-bag label set, and the nets (see
`dataset/README.md`).

The 1PEG/2PEG model files are not in the repo. `NETS` is
`peg1-first:peg1-second:4:0.5:100:peg2-model:peg2-first:16:0`.

```sh
# positions: 512 roots at 7 in the bag
PEG_POSITIONS_BAG=7 bin/magpie_test "pegnet:pegnpos:512:0:SEED:pos"
# a racing D decision with its static prefilter (arms 0 and 3) and equal-time static F (arm 5)
PEG_SIM_THREADS=10 PEG_SIM_SEED=SEED PEG_SIM_ARMS=0,3,5 PEG_SIM_EG_PLIES=2 PEG_SIM_EG_TT=shared \
  PEG_SIM_PREFILTER=6 PEG_SIM_BAI=rr,99,100,elim,z2,cap1000 \
  bin/magpie_test "pegnet:pegsim:pos:out:16:1000:25:$NETS"
# judge picks (root,code,"move" rows): nets playouts, or static-sim playouts with PEG_JUDGE_SIM
PEG_JUDGE_BAG=7 PEG_JUDGE_SAMPLES=31 PEG_JUDGE_ACTUAL_WEIGHT=1 PEG_JUDGE_SEED=SEED \
  PEG_JUDGE_SIM=16,1000,25 PEG_JUDGE_THREADS=10 PEG_EXACT_CAP_S=15 \
  bin/magpie_test "pegnet:peg3oracle:pos:picks.csv:judge:25:0.03:$NETS"
```

The env vars and output columns of each mode are documented in
`test/peg_net_eval_test.h` and `test/peg_judge.h`. `PEG_SIM_SAMPLES` records
every rollout, and `pegnet:pegsimbai` replays BAI schemes on such a recording.

## Source changes the harness relies on

cppcheck and clang-tidy have not been run on these.

- **`src/ent/sim_args.h`, `src/impl/random_variable.c`:** `SimArgs.rollout_move_fn`
  and `rollout_done_fn`. They have their own sample entry points, so a plain
  sim pays nothing for them. `RANDOM_VARIABLES_TABLE` replays recorded samples.
- **`src/impl/bai.h`, `src/def/bai_defs.h`:** new `BAIOptions` fields.
  - `eliminate` drops arms by the GK16 threshold. The best arm stays the argmax
    of all arms, so the sim's pick is BAI's.
  - `elimination_z` sets a fixed z in place of GK16's threshold.
  - `arm_sample_limit` caps each arm's samples under round robin.
  - All three together are racing. Decisions happen only at folds, so results
    don't depend on the thread count.
- **`src/impl/peg.c`, `src/impl/peg.h`, `src/def/peg_defs.h`:** callbacks for
  nested 1PEG/2PEG choosers, and a root ranker that puts a net's ranking first
  (H2).
- **`src/impl/endgame.c`:** depth 0 returns the first move of the solver's
  greedy playout, including the stuck-tile logic.
- **`src/impl/move_gen.c`, `src/impl/move_gen.h`:** `MoveGenArgs.lane_mask` and
  `initial_best_move`.
- **`src/impl/value_net_features.*`, `src/ent/value_net_history.h`,
  `src/def/value_net_defs.h`:** the nets' features.
- **`src/compat/endian_io.h`:** `fwrite_le_floats`.
- Call sites updated for the new fields: `autoplay_solvers.c`, `config.c`,
  `play_chooser.c`.

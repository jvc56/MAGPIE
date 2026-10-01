# Blocking/setup PR work — status and handoff

Started 2026-09-30. Research branch: `claude/pat-sim-vs-threat` (preserved; audit
committed as `458de4bb`). This file is the running handoff record.

## Code map (as found)

- PAT itself is unmerged: draft PR #713 (`claude/pat-main`). The research branch
  is #713 + 8 commits. All PRs here stack on `claude/pat-main`.
- PlayChooser four-ply / 30-warmup defaults live in the research commits AND in
  draft PR #716 (`claude/playchooser-sim-args`). They are excluded from these PRs.
- Research-only movegen controls (lane_mask, lane_cover_masks, tiles_played_mask,
  use_best_floor, best_slack, skip_exchanges) add hot-path branches. Not ported
  unless measured free; the pass-relative teacher needs none of them except,
  possibly, skip_exchanges (measured before inclusion).
- `sim_candidates_fn` PlayChooser callback exists only on the research branch.
- PAT already separates root candidates (`patcand`) from rollouts (`patrollout`,
  `SimArgs.pat_rollout_disabled`).
- Teacher (harness `pcd_pass_relative_checks`, PCD_PASS_RELATIVE=1,
  PCD_CONDITION_DRAWS=1): universe = top 60 no-PAT static non-pass moves + up to
  5 exchanges within 35 static points; 64 opponent racks dealt disjointly from
  the on-turn player's unseen pool; per rack one follow-up draw order taken from
  the pool minus that rack. Reply/follow-up = best placement by score (0 if
  none), exchanges excluded. Setup gives our rack = candidate leave + the same
  refill tiles in both branches. Terminal opponent replies give a 0 follow-up.
  Harness stores 0.7*delta / 0.25*delta and the explore arm doubles/triples them:
  wide = 1.4*blocking, 0.75*setup (points). Samples had bag >= 7.

## Plan (PR stack)

1. `blocking-setup-teacher` (base claude/pat-main): production pass-relative
   teacher module (sampling separated from measurement), per-lexicon versioned
   parameter file format + loader, label-generation harness in test/, stdlib
   Python fitter, reproduction test against archived teacher values.
2. `sim-nomination` (base 1): configurable static/PAT/blocking/setup/exchange
   sources with dedup + provenance, PlayChooser candidate-source hook,
   explicit no-PAT rollout separation, disabled-path benchmarks.
3. `static-ish-eval-bench` (base 2): adjusted static chooser for benchmarking,
   paired-game and position harness, volatility diagnostics.

## Compute plan

Machine: Apple M4 (4P+6E), 16 GB, ~6 GB free disk. Bounded pilots only;
each recorded below with command, seeds, wall time, outputs.

## Log
- 2026-09-30 audit preserved (`458de4bb`, pushed). Large inputs in
  `notes/pat-seeded-pool-audit-20260930/archives/audit-inputs.tar.gz`.
- Teacher port (`src/impl/blocking_setup.c`, worktree `~/sources/magpie-bst`,
  branch `claude/blocking-setup-teacher`): on-demand
  `blockingsetupreplay:<cgp>:<checks.csv>:<PCD_SEED>` replays the harness's
  exact sampling. overnight-w0 (150 positions, 18,237 rows, seed
  202609300702) and blocking-w0 (50 positions, 6,071 rows, seed 202609301002):
  0 mismatches, max abs error 0.
- Compute plan, pilot P1 (bounded, ~20 min each): fresh CSW24 and NWL23 data,
  360 independent no-PAT static games each (120 per phase), one position per
  game, teacher at 16/64/256 racks, PAT (lexicon .pat) nominees 25, two
  independent 10 s 4-ply round-robin references, 8 workers. Seeds CSW24
  20261001.., NWL23 20261101... Purpose: pipeline validation, teacher
  precision vs rack count, cost, rough parameter direction. Not enough
  precision for production defaults; nothing is enabled from it.
- P1 CSW24 done 18:41-18:58 (data `~/sources/bs-data/p1-CSW24`, binary sha
  a7ac5227..., commit 58d513bf content). 360 positions, universe ~62.
  Teacher per position (8 processes busy): 16 racks ~125 ms, 64 ~525 ms,
  256 ~2.1 s. Teacher precision vs 256 racks: median Spearman blocking
  0.835 (16) / 0.949 (64); setup 0.629 (16) / 0.855 (64); top-25 admission
  lists differ from the 256-rack list by ~1.8-2.0 moves at 64 racks.
  Split (game hash, seed 20261001): 210 train / 78 validation / 72 test
  (test untouched). static_choice fit: best train grid (0.5, 0.25) ->
  validation +0.001 +- 0.056 pp (n=78, 13 changed). sim_admission oracle at
  equal count: validation gain exactly 0 (degenerate at this scale).
  Volatility diagnostics (train+val, exploratory, noise-dominated): no
  feature shows a reliable relation; late-bag quartile has the largest mean.
- Choice-set references (bs_fit.py choices + bsgen refs cands=): grid picks
  average 2.55 moves per position, 90/360 positions need none.
- Pilot P2 (planned, bounded ~12 min): pool comparison A/B/Bp/C vs equal-count
  static, 96 fresh CSW24 positions (games seed 20261201), research_v0
  coefficients (1.4/0.75, not fitted), 5 s selection / 20 s reference,
  no-PAT rollouts. Then P3 games pilots.
- Branch split (from final tree a2f673b2 minus notes; WIP history kept on
  local `claude/blocking-setup-wip`): draft PRs
  #744 `claude/blocking-setup-teacher` (base claude/pat-main #713),
  #745 `claude/sim-nomination` (base #744),
  #746 `claude/blocking-setup-studies` (base #745; pilot evidence in
  notes/blocking-setup-pilots-20260930/REPORT.md). PlayChooser 4-ply/30-warmup
  defaults are NOT in any of them (they remain #716 and this research branch).
- P2 pools (96 positions, 5 s/20 s, research weights): B-A +0.006+-0.006 pp
  (1/96 choices differ; pools differ 50/96); C-A identical choices.
- P3 games vs static (CSW24, 64 racks): research 1.4/0.75 53.6%+-2.1% (200
  pairs, p=0.077, 40% decisions changed); P1-fit 0.5/0.25 51.5%+-2.1%; PAT
  50.75%+-1.3% (500 pairs). Adjusted decision p50 ~460 ms vs static 0.18 ms.
- Checks: optimized build, full default suite (BOARD_DIM 15, 267 s) pass;
  format, circ-deps clean; cppcheck 2.17.1 + clang-tidy (LLVM 20) clean on
  new files. Not run: BOARD_DIM 21, wasm, whole-tree CI cppcheck/tidy.
- Awaiting go-ahead: (1) static-ish confirmation 2000 fresh pairs ~50 min;
  (2) per-lexicon Track 1 data w/ choice-set refs ~40 min/lexicon;
  (3) decisive A/B/C pool study ~6-7 h (enrichment design in REPORT.md).
- Open questions: whether to publish .bsp files via MAGPIE-DATA; whether the
  static-ish evaluator should become a PlayChooser mode (currently test-only);
  a cheap feature-model approximation of the teacher (not started).
- 2026-09-30 ~20:00: P4 2000-pair confirmation (seed 20261401) STOPPED by
  user request at 273/4000 games to speed up the evaluator first; marked
  incomplete, not analyzed.
- Teacher speed (one M4 core, 48 positions, 64 racks): exact path 311 ->
  152 ms/position (byte-identical; replay still exact); movegen additions
  free when off (10k static games 6.881 s both). Best-move race
  (blocking_setup_checker_choose): z=3 99.4% agreement with full argmax,
  48 ms/position; z=2 97.8%, 35 ms. In games: check 274 ms -> 126 (exact,
  identical games) -> 37 (z=3) -> 28 (z=2) median. Pushed to #744
  (22792a7a), merged into #745 and #746; evidence in #746 REPORT.md.
- Not yet tried: 16 racks + race, threads for evaluate-all, follow-up
  reconstruction when the reply changes. Confirmation run: not restarted;
  needs a decision on which evaluator mode (exact / z=3 / z=2) to confirm.
- P5 prespecified confirmation (z=3 race, research 1.4/0.75, 64 racks, CSW24,
  2,000 fresh pairs, seed 20261501, binary ea6d5b6a): 52.24% +- 0.65% vs
  no-PAT static, 95% CI [50.97, 53.51], z=3.46, p=0.0005; spread +8.0 +- 1.15
  per game; 39% of checked decisions changed; check median 71 ms with 8
  workers. Equal-work vs static only. games_study.sh now takes Z=/BATCH=.
- Rack sweep at z=3 (2,000 fresh pairs each vs no-PAT static, research
  1.4/0.75): 32 racks 51.29% +- 0.65 (p=0.048); 64 52.24% +- 0.65; 96
  53.55% +- 0.64; 128 53.21% +- 0.65. Unpaired across rack counts
  (difference SE ~0.9 pp): rises to ~96 then flat within noise. Check
  median 57/71/80/86 ms with 8 workers. Data ~/sources/bs-data/p6-*, p7-*;
  notes committed locally on claude/blocking-setup-studies (04200dd6), not
  pushed yet: that branch also has the unreviewed WIP rollout-policy commit
  d827ce1d (SimArgs.rollout_blocking_setup, -rbs/-rbsz/-rbsracks).

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
- Overlap research (scratch branch claude/bs-overlap-research, local):
  pool-as-rack enumeration 4-25x slower; word-map repetition 13x but caching
  it gains 0-3%. Not pursued.
- teacher_value .bsp row (score | equity_score | equity). Head-to-heads
  (2,000 pairs each, 64 racks, z=3, weights 1.4/0.75 unrefit, b = score
  mode): equity 51.76% +- 0.66 (p=0.007); equity_score 49.61% +- 0.65
  (p=0.55). Local WIP commits on claude/blocking-setup-studies, unpushed.
- 2026-09-30 22:41 PDT: user authorized useful testing until 07:30 PDT.
  Queue (sequential, each prespecified, fresh seeds): P9 equity-weight grid
  (running) -> P10 confirm winner (2,000 pairs) -> rollout wiring tests +
  off-cost check + sim it/s (no-PAT/PAT/static-ish) -> NWL23 transfer
  -> paired 64 vs 96 racks -> PlayChooser sim pilot (static vs static-ish
  rollouts) if time allows.
- P9 equity-weight grid (800 pairs/point vs equity 1.4/0.75, seed
  20262101): no point beat the reference; best (1.4,0.4) 49.94% +- 1.0;
  blocking 2.1 or setup 1.1 2-3 SE worse. P10 confirmation SKIPPED
  (deviation recorded in ~/sources/bs-data/p10-confirm-tuned/SKIPPED):
  the selected point did not beat the reference even in tuning.
- Benchmarks (quiet machine): rollout wiring off-cost within noise
  (11.97-12.19 s base vs 11.97-12.30 s new, 120k iterations). Sim it/s
  (4-ply, 15 cands, 1 thread): no-PAT 10,457; PAT 6,911 (0.66x);
  static-ish score 16 racks 10.1, 64 racks 6.1, equity 64 racks 4.2.
- Code reorganized: #746 got notes-only commits (rack sweep, teacher
  value); new draft PR #747 (claude/static-ish-rollouts, base #746): equity
  teacher mode, BlockingSetupPolicy, SimArgs.rollout_blocking_setup,
  -rbs/-rbsz/-rbsracks, params_b, tune_weights.sh, sim player in games.
  Backup of the pre-split tip: local claude/static-ish-wip.
- Queued: P11 NWL23 equity vs static, P13 CSW24 equity vs static, P12
  equity 96 vs 64 racks, P14 equity vs PAT static, P15 equity vs 100 ms
  2-ply sim, P16 gate (checks only bag<60) vs ungated.
- Overnight results so far (all 2,000 pairs, details in #747
  notes/static-ish-studies-20261001/REPORT.md): NWL23 equity vs static
  52.25% (p=0.0003); CSW24 equity vs static 51.42% (p=0.028); equity vs
  PAT static 51.12% (p=0.08); equity vs 100 ms 2-ply sim 52.25%
  (p=0.0006); gate bag<60 vs ungated 50.54% (n.s., 26% cheaper); 96 vs 64
  racks equity 50.04%, score 50.82% (both n.s.); z=3 vs exact 49.10%
  (n.s., 3.4x cheaper); universe 30 vs 60 49.66% (n.s., 33% cheaper);
  NWL23 equity vs score 49.26% (n.s.; CSW24 equity advantage does not
  replicate). Running: P21 block, P24 nominated-candidate sims, P25 block.
- Overnight run finished 06:14 PDT (P9-P36). Summary at the top of #747
  notes/static-ish-studies-20261001/REPORT.md. Key: static-ish beats
  static in CSW24/NWL23/FRA20 (51.4-53.9%); beats 100 ms and 300 ms 2-ply
  sims; cheap config (bag<30, universe 30, 18 ms mean) 52.4% vs static,
  ~1 pp behind full (p=0.09), ties PAT static; 64 racks is the floor;
  z=3 fine; equity mode +0.87 +- 0.29 pp pooled but heterogeneous;
  nominated sim candidates no help at 300 ms (1,000 pairs).
- Open next steps: decide default config (full vs cheap; score vs equity);
  fit weights per lexicon with the bs_fit/tune pipeline; a sim test at
  realistic budgets (>= 1 s) for nomination; publishing .bsp files via
  MAGPIE-DATA; whether static-ish should be a PlayChooser mode.
- Branches: #744 teacher (+speedups), #745 nomination, #746 studies,
  #747 claude/static-ish-rollouts (all draft). Local only:
  claude/static-ish-wip (backup), claude/bs-overlap-research (scratch).
- 2026-10-01 morning (#747, commits 190a69c1..be4b8d07):
  - teacher_value equity_reply: replies by equity, follow-ups by score.
    P37 grid best 0.7/0.4; P38 confirm vs score 1.4/0.75 = 51.39%
    (p=0.031, 2,000 pairs, fresh seed), and cheaper (50 vs 65 ms p50).
    CSW24 only so far.
  - Win-chance racing (race.win_pcts; bsstudy winpct=1; sim -rbswp true):
    P39 vs points = 49.56% (p=0.51), spread -8.6. Null with points-tuned
    weights.
  - Fixed: sim workers kept a stale rollout policy (pointer compare), so
    -rbsz/-rbsracks changes between sims were ignored and a reloaded -rbs
    left freed params. Now compared by value.
  - Next: replicate equity_reply 0.7/0.4 in NWL23 and FRA20; equity_reply
    vs equity mode head-to-head.
- 2026-10-01 afternoon (#747 notes through 5acbe717 and the P44-P48 commit):
  - P40 lower equity_reply grid: nothing beats 0.7/0.4; blocking matters,
    setup flat in CSW24. P41 skipped.
  - P42 NWL23 hybrid vs score 49.88% (n.s.); P43 FRA20 51.25% (p=0.048);
    pooled with P38 +0.85 +- 0.37 pp.
  - P44-P46 hybrid vs equity mode: +0.48 +- 0.37 pp pooled (n.s.), at
    ~60% of the time. Hybrid supersedes equity mode.
  - P47 NWL23 grid picked 1.4/0.4; P48 vs score 49.46% (n.s.). NWL23 gains
    nothing from equity variants or refit.
  - Candidate default: equity_reply 0.7/0.4 (better or equal everywhere,
    ~20% cheaper than score mode). Not switched; user's call.

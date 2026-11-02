# Late-game search bridge, PlayChooser, solver scaling, and strategy pipeline: plan

Source: late-game study on `origin/claude/peg-nets-main`
(`notes/pegsim-late-game-20261101/REPORT.md`), `docs/better-defaults-plan.md`
(`#897`), `docs/peg-vs-macondo.md`, `docs/peg-status-plan.md`, `#747`
(`origin/claude/static-ish-rollouts`), `#697`
(`origin/claude/pat-utility-correction`), `#930`/`#931` (`pegfit` /
`pegadjust-table`), and full audit of `src/impl/play_chooser.c`,
`src/impl/endgame.c`, and `src/impl/peg.c` at `v0.1.4` (`57c8f810`).

Companion roadmap documents:
- `docs/inference-parity-and-softmax-plan.md` — `inference.c` parity fixes
  (PR 1) and standalone static/PAT softmax inference (PR 2).
- `docs/fast-inference-and-movegen-plan.md` — `census_play_scores` inference,
  RIT exchanges, batched subtrees, GADDAG playthrough pre-filter, and table
  memory optimizations.

---

## 1. Net-free late-game search bridge (`bag` 5–8)

### Findings from `origin/claude/peg-nets-main`
In the 7-in-bag late-game study (`notes/pegsim-late-game-20261101/REPORT.md`),
comparing standard midgame static rollouts against late-game rollouts at
**matched wall-clock time** produced a **`+0.63` win% gain**. Decomposing that
gain showed that most of the equal-time improvement comes from three **net-free**
mechanisms working together:
1. **Fast static-sim prefilter to top 6 candidates:** eliminates clearly
   dominated plays cheaply before spending deep late-game rollouts.
2. **2-ply exact endgame search (`endgame_solve` at `plies = 2`) once a rollout
   empties the bag (`bag == 0`):** standard static rollouts play greedy 1-ply
   `standard_endgame_adjustment` moves at `bag == 0`, missing basic 2-ply
   outplay/block inversions that decide close late games. With a **shared
   lockless transposition table** across rollout threads, 2-ply bag-0 solves hit
   cached positions heavily and cost only a fraction of a full rollout.
3. **BAI elimination racing (`elim`, `z = 2`, iteration cap):** stops allocating
   expensive late-game rollouts to candidates once statistically separated.

### Productization plan (zero neural-net file dependencies)
1. **Option in `sim` (`src/impl/simmer.c`, `src/ent/sim_args.h`,
   `src/impl/config.c`):**
   - Add `-sendgameplies <int>` (default `0`; e.g., `2` enables a 2-ply
     `endgame_solve` tail once a rollout reaches `bag == 0` within the rollout's
     ply horizon, or extends the rollout to finish the 2-ply bag-0 decision when
     a rollout empties the bag).
   - Share a single `EndgameTranspositionTable` across the `SimCtx`'s worker
     threads (sized modestly, e.g., `64–256 MB` or reusing `PlayChooser`'s
     existing `endgame_tt` — never exceeding the 50% system RAM cap in
     `AGENTS.md`).
   - Ensure each sim worker owns a lightweight `EndgameCtx` reused across
     iterations (with `lazy-sim-backups` already merged, workers pay zero
     per-iteration heap allocation).
2. **Two-stage prefilter + late-game race for `5 <= bag <= 8`:**
   - Stage 1 (~15%–20% of budget): standard fast static 2-ply/4-ply `sim` over
     the initial candidate set (including `-stempo` candidates once `#917`
     lands) to select the top `K = 6` plays by win%.
   - Stage 2 (~80%–85% of budget): race the 6 survivors with `-sendgameplies 2`
     (plus PR 2's softmax inference, since at `bag` 5–8 a 3–5 tile opponent play
     leaves only 2–4 tiles, so exact softmax inference takes `< 5 ms`!) under
     BAI elimination (`z = 2.0`).
3. **Validation (`AGENTS.md` quality methodology):**
   - Register an on-demand test in `on_demand_test_table[]` playing equal-time
     clocked game pairs (`-gp true -wmp true -threads 8`) comparing `PlayChooser`
     with vs without the `bag` 5–8 late-game bridge (and standalone fixed-budget
     7-in-bag decision panels against `peg_judge` static-sim oracle).

---

## 2. Four high-leverage `PlayChooser` upgrades (`src/impl/play_chooser.c`)

Auditing `src/impl/play_chooser.c` after `#897` (`better-defaults`) turned up
four places where `PlayChooser` has not yet inherited recent engine improvements
or leaves substantial clocked strength on the table:

### 2.1 Pass `PlayChooser`'s `endgame_tt` into `play_chooser_run_peg`
- **Current code:** `PlayChooser` lazily allocates a persistent 20%-of-RAM
  `endgame_tt` (`play_chooser_get_endgame_tt`, `src/impl/play_chooser.c:318-324`),
  yet `play_chooser_run_peg` (`src/impl/play_chooser.c:758`) passes
  `shared_endgame_tt = NULL`!
- **Consequences today:**
  1. Every PEG turn allocates, zeroes, and frees a brand-new multi-GB
     transposition table inside `peg_solve`.
  2. When two `PlayChooser` instances play game pairs, `0.20 + 0.20` persistent
     endgame TTs plus a fresh PEG TT can exceed the 50% system RAM rule in
     `AGENTS.md`.
  3. Positions solved during `bag = 1` or `bag = 2` PEG are thrown away instead
     of pre-warming `PlayChooser`'s `endgame_tt` for the very next `bag = 0`
     endgame turn!
- **Fix:** Pass `play_chooser_get_endgame_tt(chooser)` as `shared_endgame_tt` in
  `play_chooser_run_peg` (verifying Zobrist seed/hash compatibility between
  `peg`'s leaf endgame calls and `endgame_solve`).

### 2.2 Bring `#897` timed-cascade speedups to `play_chooser_run_peg`
- **Current code:** `play_chooser_run_peg` (`src/impl/play_chooser.c:746`) still
  hardcodes `first_win_intermediate = false` and fixed stride defaults.
- **Fact (`docs/better-defaults-plan.md`, Phase 8):** Enabling
  `first_win_intermediate = true` in intermediate cascade stages gave a ~`2x`
  speedup on timed PEG cascades in `config.c` with no loss of final ranking
  accuracy, and bag-sized `-pegstride` (`5` at bag 3, `7` at bag 4;
  `docs/better-defaults-plan.md:619-622`) avoids resampling aliasing across
  draw strides.
- **Fix:** Set `first_win_intermediate = true` in `play_chooser_run_peg` and
  benchmark bag-sized strides (`5` for `bag == 3`, `7` for `bag == 4`) in timed
  game pairs.

### 2.3 Adaptive BAI early stopping & phase-weighted clock management
- **Current code:**
  - `play_chooser_run_sim` (`src/impl/play_chooser.c:501`) passes
    `BAI_THRESHOLD_NONE`, forcing `sim` to burn 100% of `budget_seconds` even on
    obvious bingos or +30%-win-rate blowouts where Candidate 1 separates after
    200 rollouts.
  - `play_chooser_get_seconds_for_move` (`src/impl/play_chooser.c:386-423`)
    divides remaining clock nearly uniformly across estimated remaining turns.
- **Fix:**
  1. Enable a conservative BAI stopping threshold (e.g. `BAI_THRESHOLD_99` or
     `99.5%`) in `play_chooser_run_sim` so clear midgame decisions finish in
     10–30 ms and **bank their unused clock** automatically for contested turns.
  2. Weight per-turn time allocation toward critical late-game turns (`bag` 1–8,
     where exact PEG and the 2-ply endgame bridge convert extra milliseconds
     into large win% gains) while reserving a clean safety buffer for `bag == 0`.
  3. Pair with PR `#716` (4-ply sim default where budget permits).

### 2.4 Budget-capped inference in `play_chooser_run_sim`
- **Current code:** `play_chooser_run_sim` (`src/impl/play_chooser.c:494-495`)
  hardcodes `use_inference = false`, so clocked `PlayChooser` games never use
  opponent-rack inference during midgame simulations!
- **Why it was off:** On a tight clock, an opponent 7-tile exchange or 1-tile
  play could spend seconds in `infer()`, starving `sim` of rollout time.
- **Fix:**
  1. Pass the opponent's previous move (`GameHistory` or `previous_move`) into
     `PlayChooser` and add an inference wall-time / `MovegenBudget` cap (e.g.,
     at most `15%–20%` of `budget_seconds`; if `infer()` hits the cap before
     finishing all leaves, fall back cleanly to uniform random unseen draws for
     that turn!).
  2. Combined with `census_play_scores` inference
     (`docs/fast-inference-and-movegen-plan.md` Section 1.1) and batched subtree
     claiming (Section 1.3), even 7-tile exchanges finish in a few milliseconds,
     making inference practical on almost every clocked turn.

---

## 3. Endgame and PEG solver scaling

### 3.1 Fix root TT bound cutoffs returning an empty PV (Issue `#698`)
- **Bug (`src/impl/endgame.c:5051-5058`):** In `iterative_deepening` (especially
  under fixed/narrow aspiration windows `[alpha, beta]` or when a shared TT is
  already warm from a prior PEG / `analyze` solve), if `negamax` hits a
  sufficient `TT_LOWER` (`>= beta`) or `TT_UPPER` (`<= alpha`) bound at the
  root on every depth `1 .. plies` without searching children, neither `pv` nor
  `worker->best_pv` ever records a move (`num_moves == 0`). This returns an
  empty move list (and forces `src/impl/analyze.c` to fall back to an expensive
  full-window re-solve).
- **Fix:**
  1. At the root node (`ply_from_root == 0`), either require at least one legal
     best move to be materialized (if `ttentry_valid(entry)` has a stored
     `SmallMove`, unpack and validate it into `pv.moves[0]`; if a bound entry has
     no stored move, do **not** take a move-less TT cutoff at the root!).
  2. Add a deterministic regression test in `test/endgame_test.c`: solve a
     position at depth 6 into a shared TT, then re-solve the same root with a
     narrow fixed window `[v + 1, v + 2]` (fail-low) and `[v - 2, v - 1]`
     (fail-high) without resetting the TT, asserting a valid legal root `Move`
     is always returned.

### 3.2 Eliminate PEG's tail bottleneck via dynamic draw / worker lending
- **Bottleneck (`docs/peg-vs-macondo.md:187-200`, `docs/peg-status-plan.md:127-128`):**
  In `peg`'s complete solve (`src/impl/peg.c`), assigning one candidate per
  worker thread is optimal while `remaining_candidates >= active_workers`
  because losing candidates refute sequentially after 1–2 cheap draws. Once the
  queue drains to the last `1 .. active_workers - 1` surviving candidates (the
  exact regime dominating the remaining `> 900 s` 3–4-in-bag deficit outliers),
  idle threads sit unused while 1–3 candidates grind through dozens of hard
  draws and deep leaf endgames.
- **Two-level tail lending design:**
  1. **Draw-level stealing within a surviving candidate:** Once the outer
     candidate job queue is empty, switch any in-flight candidate from single-thread
     sequential draw iteration to an atomic draw cursor `next_draw_idx` over its
     remaining draws so idle PEG workers pull `(candidate_idx, draw_idx)` tasks
     from the active survivors, checking the candidate's atomic `wins / losses`
     early-cutoff condition before and after each draw!
  2. **Endgame-level ABDADA lending (`endgame_add_worker`):** When even
     `remaining_draws == 0` and 1–3 long leaf endgames remain running, idle
     workers can attach to the longest-running leaf `EndgameCtx` sharing its TT
     (ABDADA) until it finishes.

### 3.3 Autoplay bot endgame caps & `RACK_SIZE=8` completeness
1. **Per-move endgame time cap for autoplay bots
   (`docs/better-defaults-plan.md:623-625`):**
   - A rare pathological 6+ ply stuck endgame can stall an untimed `autoplay`
     thread for hours. Support `-etlim` / a default bot safety cap (or node
     budget) per endgame move in `autoplay` so iterative deepening returns its
     last completed depth cleanly (`241f10ee` already made `-etlim` stop an
     in-progress depth promptly).
2. **`RACK_SIZE=8` solver completeness (PR `#663` + `#661` + `#662`):**
   - Pair `#663` (`bit_rack_defs.h` fixes and `RACK_SIZE=8` CI job) with fixing
     `SmallMove` bit-packing dropping an 8th played tile (`#661`,
     `src/ent/move.h:42-77`) and `rack_encode` width bounds (`#662`) so 8-tile
     variants work end-to-end in `endgame` and `peg`.

---

## 4. Strategy calibration pipeline, fast analysis tier, and JSON API

### 4.1 One-command lexicon strategy calibration pipeline
Magpie now implements every major static-evaluation component natively in C:
1. `census` + `leaveblend` (`#921`–`#924`): exact zero-self-play board-play
   lattice leaves blended with self-play `leavegen` KLVs.
2. `patgen` v5 + `#697` `utility_adjust`: premium defense, created-hook scoring,
   stage scaling, and score-differential win-curve utility correction.
3. `winpct` recording & smoothing: empirical spread-by-bag win-percentage table.
4. `pegfit` + `-pegadjust` (`#930`, `#931`): empirical pre-endgame bag-emptying
   adjustments fit to static play outcomes.

**Plan:** Add a reproducible driver script (`tools/build_lexicon_strategy.sh` or
`scripts/calibrate_lexicon.py`) documenting and automating the dependency order:
`census -> leavegen -> leaveblend -> klvwmp2rit -> winpct -> patgen (with utility_adjust) -> pegfit`
so non-English and super lexica (`NWL23`, `FRA24`, `DSW25`, `OSPS49`,
`CSW24_super21`) can each receive a matched `{.klv2, .pat, pegadjust.csv, winpct.csv}`
release bundle in `MAGPIE-DATA`.

### 4.2 "Instant+" ~30 ms kibitz tier (`equity_reply` race from `#747`)
- While `origin/claude/static-ish-rollouts` (`#747`) showed `equity_reply` is
  too slow to use *inside* 2-ply Monte Carlo rollouts (`P49`–`P51`), standalone
  **1-ply `equity_reply` racing** (`bag < 30`, ~18–50 ms per decision) beat both
  100 ms and 300 ms 2-ply sims by **`+2.3` win%** across lexica!
- Expose `-s1 equity_reply` / a `generate -reply true` (or `sim -plies 1` fast
  BAI race) tier for instant post-game `analyze` pre-scans, mobile/WASM
  battery-saver kibitzing, and candidate nomination (`origin/claude/sim-nomination`).

### 4.3 Uniform structured JSON output across `cmd_api.h`
- `-pegjson` gave WASM clean structured progress and final rankings for `peg`,
  whereas `generate`, `sim`, `infer`, `endgame`, and `analyze` still primarily
  emit human-formatted ASCII tables over `cmd_api.h` (`src/impl/cmd_api.h:82-139`).
- Adding an opt-in `-json true` output serializer for `generate`, `sim`,
  `infer`, `endgame`, and `analyze` (plus fixing `auto`'s table-load report
  omitting forced tables noted in `docs/better-defaults-plan.md:626-628`) will
  eliminate brittle text scraping in `wasmentry`, `magpie_tui`, and external
  `libmagpie` clients.

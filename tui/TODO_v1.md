# MAGPIE TUI — v1 TODO

v1 bar: a stranger can play a complete, timed game against the computer,
annotate or watch a game with live analysis through every phase
(midgame sim → pre-endgame → endgame), and walk away with a GCG —
without hitting a dead end or a stall.

Explicitly punted past v1: **inferences**, board-editing mode (the
`/board` analysis power tool), Woogles live play / bot API
(NOTES_woogles_integration.md — bot flow first when we get there),
and the GameSession/Config extraction (NOTES_game_session.md) unless
it falls out of test work.

---

## P0 — v1 blockers (functionality)

### 1. Pre-endgame (PEG) support
The engine work has landed on main (`peg_solve` in src/impl/peg.h).
The solver has a sim-style live poll (`PegPoll`), which is exactly
the shape the TUI streams.
- [x] Land/track the engine PRs; pick the canonical entry point
      (`peg_solve` + `PegPoll`, both upstreamed to main).
- [x] bot_worker: third solve mode between sim and endgame
      (`run_peg`); triggers on the solver's own bag range (effective
      bag 1–4 tiles); budget from the same clock logic; falls back to
      sim if PEG is interrupted before ranking anything.
- [x] Analysis panel: PEG row source (live poll → rows at the same
      ~10Hz throttle as sim — see NOTES_render_performance.md rule 3;
      current stage's candidates merged over the previous stage's
      baseline so stage transitions never blank the leaderboard) +
      per-turn snapshot + saved leaderboard moves + /resume, like
      sim/endgame.
- [ ] TT memory budgeting: PEG shares/partitions with the endgame TT
      (CLAUDE.md: total ≤ 50% RAM; two TTs = 0.25 each). PEG's leaf
      endgames currently size their own ctx inside peg_solve.
- [ ] Status bar: PEG progress (solved/total scenarios) where nps
      shows. (The panel title already shows cands done / field size
      per stage.)

### 2. Play-vs-computer completeness
- [ ] **Pass and exchange for the human.** Placement-only today. The
      cell editor already parses `pass` / `ex ABC` (annotation), so:
      extend `tui_pvc_commit_preview_move` beyond PLACEMENT, add a
      board-entry gesture (e.g. typed `pass`/`-ABC` in the cell, plus
      command-bar `/pass`, `/exchange ABC`), keep exchanged tiles
      concealed in history (already done for the bot's).
- [ ] **Game-over flow.** End banner with final scores/bonus, then a
      clear "what now": new game / copy CGP / browse analysis. Today
      the game just stops and the analysis panel reappears.
- [x] **Clock enforcement.** Play-setup now offers three overtime
      rules (flag at 0:00 / max overtime with a minute cap /
      unlimited) and two penalty rates (10 pts per started minute /
      1 pt per started second). Flagging and cap-exceeded forfeits
      end the game with a "lost on time" history event; overtime
      penalties post as engine-style time-penalty history entries at
      game end and adjust the live scores. Overtime clocks render
      negative ("-1:23") in the error color.
- [ ] Six-zero / consecutive-scoreless end condition surfaced properly.

### 3. GCG export / session safety
- [ ] Export the current game as GCG (engine has the writer via the
      console `export` command — wire `/export [path]`).
- [ ] Autosave each committed turn to a session GCG so a crash or
      accidental quit can't lose a game (quit-confirm exists; this
      covers everything else).
- [ ] Resume: load that autosave back into the matching mode (PvC
      games must restore racks/bag — needs more than the GCG; CGP +
      seed sidecar or punt resume-PvC to v1.1 and resume watch/annotate
      only).

## P1 — strongly wanted for v1 (UX polish)

From the Woogles gap audit (NOTES_input_design.md):
- [ ] Recall-all gesture (return all placed tiles, keep anchor) —
      Esc cancels the whole entry today, which is blunter than wanted
      mid-edit.
- [ ] Right-click a placed (uncommitted) tile → retract that tile
      (Backspace is last-tile-first only).
- [ ] Click a rack tile → place at the cursor (rack is display-only).
- [ ] PvC setup: remember last-used settings (names, time, lexicon,
      strength) across sessions via config.
- [ ] Bot strength presets in plain words ("beginner … max") mapping
      to sim plies/candidates, instead of raw numbers.

## P2 — nice for v1, fine to slip
- [ ] "Add custom candidate" from the analysis panel (design doc).
- [ ] In-place rack editing (design doc; slash `/rack` exists as the
      fallback design).
- [ ] History panel: jump-to-turn by clicking the board snapshot
      while scrubbing; smoother long-game scrolling.
- [ ] Exchange-tile picker UI (vs typed `ex ABC`) — Woogles-style
      click-to-select modal.

---

## Code quality — where the TUI is weakest

Ranked by (bug risk × cost to work around). The first three are where
this month's real bugs actually came from.

### Q1. Two parallel move-entry implementations
The board builder and the history-cell editor implement the same
semantics (rack gating, blank fallback, playthrough absorption,
commit) as two separate code paths that share helpers by convention.
Nearly every input bug this cycle (autofill gluing letters onto the
coord, landing-square vs rack-gating conflicts, blur-commit wiping
PvC racks, the unreachable direction toggle) was a divergence between
them. **Unify on one move-builder state machine** (origin/anchor/dir/
placed-tiles) that both surfaces drive; the buffer becomes a render
of that state instead of the source of truth.

### Q2. main.c input dispatch: one ~4,900-line function
`main()` contains the entire event loop: every modal's key handling,
the game-screen dispatch, mouse routing, and 80 hand-paired mutex
lock/unlock sites, as one if/else cascade. Adding any handler means
scrolling a 5k-line function and getting the lock discipline right by
eye. **Extract per-modal handlers and the game-screen handler into
functions (`tui_input_*.c`)**, each taking (state, key, input) and
returning a small action enum; centralize lock acquisition at the
dispatch boundary.

### Q3. Mode behavior scattered as conditionals
24 `app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER` checks across four
files gate editor commits, rack sync, seeking, concealment, analysis
visibility, blur semantics. Each new gate was discovered by a bug
(rack wipe, bag desync, panel leak). **Invert into a mode-policy
struct** (`commit_draws_tiles`, `racks_are_real`, `conceal_opponent`,
`analysis_visible`, `cell_editor_writes_entries`, …) consulted at the
behavior sites, so a new mode (or rule change) is one table row.

### Q4. game_render.c is a 9,850-line monolith with 132 statics
Layout, per-panel renderers, all modals, pixel composition, glyph
caches, tile caches, and debug counters in one translation unit.
The file-scope cache globals make the renderer untestable in
isolation and single-instance by construction. **Split by concern**
(board_pixel.c, panels.c, modals.c, caches.c) and move the caches
into a context struct. Mechanical, big, safe to do incrementally.

### Q5. Testing is end-to-end-only and CI-blind
- The shell suites (settings / annotate / cgp_copy / play_vs_computer)
  are genuinely useful but timing-sensitive, macOS-only (pbcopy), and
  not run in CI. CI doesn't even **build** the TUI — a tui build job
  plus cppcheck/clang-tidy over tui/ is a cheap, high-value win.
- The pure logic (edit-buffer parser, move builder, rack gating,
  playthrough absorption) has no C-level tests despite being the
  most bug-dense code. After Q1's state machine extraction, table-
  driven tests of it become trivial (and CLAUDE.md-conformant:
  test-only code in test/).

### Q6. Concurrency by convention
One big `game_state.mutex` shared by render, input, and bot threads;
the analysis-row stall (NOTES_render_performance.md incident #2) came
from reading live engine objects under contention. Direction:
worker→UI handoff via published snapshots instead of shared-object
reads under the global lock. Tactical near-term: keep new per-frame
reads behind the same throttle pattern.

### Q7. Small but real debt
- [ ] Inline `extern` declarations for the `tui_debug_*` counters →
      a proper `tui_debug.h`.
- [ ] The `MAGPIE_FPS_DEBUG` fprintf block in main.c → move into a
      `tui_perf_trace.c` helper (keeps the no-printf rule auditable).
- [ ] `tui_cell_word_landing_square` duplicates coord parsing that
      game_state.c's parser already does — share it.
- [ ] Multi-char letters (e.g. Catalan L·L) are handled in some
      builder paths and assumed single-char in others (toggle
      extraction documents the simplification) — decide: support or
      assert English-only in the TUI for v1.
- [ ] PvC "Random" first-move seat uses `time(NULL) & 1` — fold into
      the game seed for reproducibility.

---

## Suggested order of attack

1. **PEG plumbing** (P0.1) — biggest user-visible v1 feature; the
   bot_worker/analysis shape already exists for sim+endgame, so this
   is mostly a third instance of a known pattern.
2. **PvC pass/exchange + game-over + clock** (P0.2) — closes "can't
   finish a real game" holes; touches the commit path, so do it
   before/alongside Q1 rather than after.
3. **Q1 unify move entry + Q5 C-level tests for it** — pays down the
   highest-bug-density code while P0.2 has it open anyway.
4. **GCG export/autosave** (P0.3) — small, independent.
5. **CI: build + lint tui/** (Q5a) — one workflow edit.
6. **Q2 input-dispatch extraction, then Q3 mode-policy table** —
   mechanical refactors, best done after the feature dust settles.
7. P1 polish items as palate cleansers between the above.

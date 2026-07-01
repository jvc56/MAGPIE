# MAGPIE TUI Plan — Causeway 2026 + Streaming

Two-phase plan with nightly check-ins.

- **Phase 1 — Causeway commentary TUI.** Deadline: **Fri May 29, 2026**.
- **Phase 2 — Human-player mode + streaming readiness.** Deadline: **Fri Jul 3, 2026** (pre-Japan travel).

Today: Sun May 17, 2026.

---

## Phase 1: Causeway commentary TUI

### Required features

- Real OTB game watched over a video stream; commentator enters racks + plays manually.
- Sim, preendgame, endgame analysis.
- Add extra plays to candidate move lists.
- Challenges (record outcome, adjust score).
- Simmed inferences (use inferred opp rack distribution in sim).
- Click-on-board + keyboard tile entry for plays.

### Branch merges required

- `peg-*` branch — preendgame solver. Candidates (most-recent first): `peg-pass-support` (27 commits), `jvc56/peg-solver` (20), `peg-cli` (16), `peg-bai-solver` (15), `peg-1-ply-1-itb` (11). **TBD which one.**
- "Nested sims" branch — **TBD which branch.** `autoplay_sim_2` / `autoplay_sims_3` exist but unclear if these are it.

### Multi-machine pattern

- Each home machine runs an independent Claude Code session on its own git worktree / branch.
- Phone is mission control: reviews PRs via `gh pr view` / `gh pr diff`, merges, kicks off new sessions over SSH.
- Cross-machine coordination is via git push/pull, not via the SendMessage tool (which only continues an agent within one session).
- Each task brief must be **self-contained** — the remote agent starts cold.

### Open design decisions

- Click-to-place behavior:
  1. Should typed tiles auto-extend through existing tiles on the board (so typing `RUN` across an `E` produces `RUNE`)?
  2. Enter auto-commits, or land as a "proposed move" awaiting confirm?
  3. Click-to-place for exchanges, or keep exchanges slash-only (`/exch ABC`)?
- Sim-inference UX:
  1. Always-on (just enable in sim args)? Or `/infer on|off` toggle?
  2. Title indicator: `Sim+infer (4p/110K)`?
- Challenge UX: post-move modal, or slash (`/chal succ` / `/chal fail`)?

### Cut list (if behind on Sat May 23)

In order of what to drop first:

1. Sim-inference UX polish — hardcode `use_inference=true`, no toggle, no title indicator.
2. Challenge UI niceties — slash commands only, no modal.
3. Click-to-place keyboard-only fallback — mouse + slash still works.

### Day by day

#### Sun May 17 — home (tonight)

- [ ] Identify the peg branch and nested-sims branch to merge.
- [ ] Spawn 2 background merge-feasibility agents in worktrees (peg→main and nested→main) so we know the conflict landscape by Monday morning.
- [ ] Write a one-page spec for click-to-place + challenge UX + sim-inference toggle.
- [ ] Pick the click-to-place open decisions above so Wednesday's remote agent has a clean brief.

#### Mon May 18 — phone-only

- [ ] Review overnight merge-attempt reports.
- [ ] For each conflict-laden merge, kick off a focused agent on a dedicated machine to resolve. Different agent per branch.
- [ ] Background agent: investigate inference engine + RIT state; confirm `sim_args->use_inference` end-to-end path works in current `main` simmer.

#### Tue May 19 — home eve

- [ ] Prototype manual rack entry: `/rack p1 ABCDEFG` and `/rack p2 …`, marks sim/endgame stale, kicks off fresh analysis.
- [ ] First cut of click-to-place: hit-test board cell clicks, store anchor + direction.
- [ ] Direction toggle (Tab or click-anchor-again).
- [ ] Confirm whichever peg/nested merge agent shipped.

#### Wed May 20 — phone-only

- [ ] Agent A: typed-tile placement + validation + ghost-tile rendering.
- [ ] Agent B: "add custom candidate" engine plumbing — sim accepts a `force_include` move list.
- [ ] Agent C: continue branch integration if not done.
- [ ] Agent D: sim-inference TUI wiring (set `use_inference=true`, feed last-opp-move into inference args).
- [ ] Review PRs from phone as they land.

#### Thu May 21 — phone-only

- [ ] Merge whatever feature branches are ready.
- [ ] Agent: game-pair regression test on the merged commentary branch.
- [ ] Agent: stress-test sim/endgame under repeated `/rack` swaps (find any reset bugs).
- [ ] Agent: scaffold challenge state (TuiHistoryEntry field, slash command stub).

#### Fri May 22 — home eve

- [ ] End-to-end run: play a recorded game through the commentary TUI.
- [ ] Implement challenge UI + score adjustment.
- [ ] Note every UX rough edge. Triage into a bug list.

#### Sat May 23 — home, full day

- [ ] **Big push.** Sim-inference integration: enable, verify numbers differ from random-rack sim.
- [ ] Click-to-place hands-on testing across every edge case (blanks, played-through, exchanges, passes).
- [ ] Challenge flow end-to-end including history rendering of the challenge outcome.
- [ ] Tournament-realistic features: time controls + clock entry matching Causeway format, going-out / pass-the-bag correctness.

#### Sun May 24 — home, full day

- [ ] Polish all features: undo last entered move, mid-game restore from saved position/GCG.
- [ ] All-features hands-on dress rehearsal — simulate commentating a full game at tournament pace.
- [ ] Tag `v0.9-commentary`.

#### Mon May 25 — phone-only

- [ ] Agents on home machines: bake-in testing. 100 game-pair regressions, autoplay sim_2 vs main, sim-with-inference vs without.
- [ ] Triage any regressions.

#### Tue May 26 — home eve

- [ ] Final UX pass: walk through commentator workflow minute by minute.
- [ ] Worst-case test: commentator falling behind by 2 moves on a fast game — make sure catch-up entry is fast.

#### Wed May 27 — phone-only

- [ ] Tag `v1.0-commentary`. Feature freeze.
- [ ] Agent: write a one-pager "how to operate the TUI" cheatsheet (keep on screen during broadcast).

#### Thu May 28 — phone-only

- [ ] Smoke test from phone via remote Claude Code: have an agent play a saved GCG through the TUI and confirm display.
- [ ] Last-minute bug fixes only. **No new features.**

#### Fri May 29 — Tournament begins.

---

## Phase 2: Human-player mode + streaming readiness

### Required features

- Play a game against the computer (human side selectable: P1 / P2 / random).
- Conceal opponent rack during their turn.
- Click-to-place entry (already shipped in Phase 1).
- End-of-game flow (rack penalty, going-out bonus, final spread, "play again?").
- Stream-friendly layout (clean, legible at OBS capture resolutions).

### Open design decisions

- Concealment style: `???????` placeholder / count-only (`7 tiles`) / `· · · · · · ·`?
- Opponent rack reveal: momentary during their play animation, or hidden until end of game?
- Challenges in human-vs-computer v1: skip / engine-validate only / full challenge protocol with held-result?
- After bot plays: auto-advance to human's turn, or wait for an "OK" key (gives commentary/stream a beat)?

### Day by day

#### Week of May 30 — first playable

- [ ] Player-mode state + side selection.
- [ ] Opponent-rack concealment rendering.
- [ ] Bot worker fires only on its own side.
- [ ] End-to-end: pick side, bot plays first turn, human enters reply, bot plays again.

#### Week of Jun 6 — game loop polish

- [ ] Per-side clock UX (only on-turn player's clock ticks).
- [ ] End-of-game scoring + outcome screen.
- [ ] Exchange / pass flows from human side.
- [ ] Game-over: "play again?" reset cleanliness.

#### Week of Jun 13 — streaming readiness

- [ ] Optional "stream mode" layout: fewer panels, larger glyphs, higher contrast.
- [ ] Verify OBS capture: legibility at 1080p / 720p, color fidelity.
- [ ] Hide debug/FPS/slash palette during gameplay so they don't flash on stream.
- [ ] Subtitle / overlay hooks if needed.

#### Week of Jun 20 — polish + edge cases

- [ ] Undo last entered move.
- [ ] Replay-from-saved games.
- [ ] Challenge protocol (if not deferred).
- [ ] "New game" reset cleanliness across all modes.

#### Week of Jun 27 — rehearsals + final UX

- [ ] Play several games yourself, recording.
- [ ] Watch the recordings back. Note what feels off on stream.
- [ ] Final UX fixes.

#### Jul 1–3 — buffer

- [ ] Last-minute fixes. No new features.

#### Jul 4 weekend — Japan travel.

---

## Already-shipped foundation (Phase 0)

Visible in `git log notcurses-tui ^main` — completed before this plan was written:

- TUI with board / rack / bag / history / sim+endgame analysis panels.
- 2x pixel-graphics board with subscripts + grid overlay.
- Per-square tile ownership colors (P1 green / P2 amber).
- Live endgame progress with iterative deepening + polled title.
- Sim analysis with per-ply averages, best-row bolding.
- Lexicon + RIT settings, FPS gating, NPS readout.
- Menu / settings / time-picker / quit-confirm modals.
- Panel focus model with `[n>]` selection chips, Tab/Shift-Tab cycle.
- Command palette with `/new`, `/quit`, `/exit`, `/settings`.
- Mouse clicks for panel focus.
- History cursor with arrow nav + click-to-select, persistent across focus changes.
- Per-turn board snapshots + analysis snapshots (replay any committed turn's position + leaderboard).
- Per-turn deep-cloned `SimResults` for future resume.

---

## Nightly check-in protocol

Each evening, mark off completed items above. If a day's items slip, note what's blocking and either:

- Pull from the cut list, or
- Push the slipped item to the next available home-evening slot, or
- Re-spec it as a self-contained agent task for the next phone-only day.

Don't silently let items slide — the schedule has buffer days (Mon 25 / Wed 27 / Thu 28 / Jul 1–3) but they fill fast.

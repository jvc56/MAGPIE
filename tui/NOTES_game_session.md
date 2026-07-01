# Notes: extract GameSession from Config / TuiGameState

Design notes, not yet implemented. Captured here so the architecture
work can resume without re-deriving it.

## Goal

Stop treating `TuiGameState` and `Config` as two parallel-ish holders
of "the same engine objects + some extra knobs." Pull the genuinely
shared, UI-agnostic core into a `GameSession` that:

- Headless evals use directly (no Config, no UI).
- Config wraps for CLI usage (Config owns argv parsing + forwards to a
  GameSession it holds).
- UIs (TUI today, Qt/web later) wrap with their own platform/display
  state.

The end state is "TuiGameState is genuinely UI-only and small; the
parts that aren't UI live in GameSession; the CLI parser doesn't bleed
into UI code."

## Target layout

```
┌──────────────────────────────────────────────────┐
│ GameSession (UI-agnostic, src/ent/game_session.h)│
│                                                  │
│ • Game *                                         │
│ • LetterDistribution *                           │
│ • BoardLayout *                                  │
│ • PlayersData *                                  │
│ • WinPct *                                       │
│ • GameHistory *           (engine canonical)     │
│ • SimResults *                                   │
│ • EndgameResults *                               │
│ • EndgameCtx *            (reused across solves) │
│ • ThreadControl *                                │
│ • clock state (time_per_side, seconds_used[2],   │
│                turn_started)                     │
│ • ComputerPlayer config (see other notes file)   │
│ • EndgameSnapshot (last solve, board_duplicate'd)│
└─────────┬─────────────────┬──────────────────────┘
          │                 │
   ┌──────▼─────┐    ┌──────▼─────────────────────┐
   │ Config     │    │ UI layer                   │
   │ (CLI only) │    │ (TuiGameState/Qt/web/...)  │
   │            │    │                            │
   │ • pargs[]  │    │ • visual settings          │
   │ • parser   │    │ • render caches            │
   │ • exec_mode│    │ • display-formatted history│
   │ • mutates  │    │ • spinner / pending state  │
   │   session  │    │ • platform plumbing        │
   └────────────┘    └────────────────────────────┘
```

## What moves where

### Moves DOWN into GameSession (from TuiGameState)

- `game`, `ld`, `players_data`, `board_layout` — already shared.
- `win_pcts` — engine config, not UI.
- `sim_results`, `endgame_results`, `endgame_ctx` — engine objects.
- `endgame_initial_spread` — engine bookkeeping.
- Clock: `time_per_side_seconds`, `seconds_used[2]`, `turn_started`.
  Time control is a game rule, headless evals can use timed games.
- `endgame_snapshot` (the saved board + Move array + values from the
  last solve). Useful for any client that wants the analysis to outlive
  `play_move`. Once `ComputerPlayer` exists as a core abstraction, this
  becomes part of `ComputerPlayerResult` lifecycle.

### Stays in TuiGameState (UI-only)

- `border_thickness`, `blank_uppercase`, `premium_labels`, `board_scale`,
  `antialias`, `score_subscripts` — visual settings.
- `glyph_cache`, `glyph_cache_sub` — FreeType caches, 2x pixel mode.
- `render_version` (atomic) — pixel-composite invalidation signal; a
  Qt/web client would have its own equivalent (signal/slot, postMessage).
- `history[TUI_HISTORY_MAX]` and `history_count` — these are
  *display-rendered* entries (`move_str`, `rack_str`, `pending`,
  `clock_at_start`, etc.). Should be derived lazily from the canonical
  `GameHistory` in GameSession, but the cached rendered strings + the
  "pending → spinner" UX are UI concerns.
- `bot_thread`, `bot_started`, `bot_stop` — UI clients pick their own
  concurrency primitive. Headless eval doesn't need a worker thread;
  it calls the player synchronously.
- `mutex` — protects the UI-side state. GameSession will have its own
  mutex (or document its threading contract) independently.

### Stays in Config (CLI-only)

- `pargs[]`, `shortest_unambiguous_name`, `exec_parg_token`, all the
  arg-token machinery.
- `exec_mode` (sync/async/etc).
- `human_readable`, autoplay-specific flags, sim-CLI-specific flags
  (sampling_rule, threshold, time_limit, etc) — these are *commands*
  applied to a GameSession via parser, not session state.

Config gets a `GameSession *session` member and forwards engine
accessors to it.

## Migration order (safe, incremental)

Each step preserves behavior; no big-bang rewrite.

1. **Extract GameSession skeleton.** Move just the engine pointers
   (game, ld, players_data, board_layout, win_pcts, sim_results,
   endgame_results, endgame_ctx, thread_control) into a new
   `src/ent/game_session.{h,c}`. Config keeps its current API; its
   internals delegate to a `GameSession *`. Build / tests / TUI
   unchanged.

2. **Game-rule state moves down.** Clock fields (`time_per_side_seconds`,
   `seconds_used[]`, `turn_started`) leave `TuiGameState`, enter
   `GameSession`. `TuiGameState` keeps a `GameSession *session` and
   reaches through it for clock reads/writes. Same for `win_pcts` —
   TuiGameState stops owning it directly.

3. **Computer player extraction.** Move the orchestration currently in
   `tui/bot_worker.c` into core (see the player-extraction notes). The
   bot worker thread stays in TUI; it just calls
   `computer_player_compute(session, ...)` instead of inlining the sim
   / endgame plumbing.

4. **EndgameSnapshot moves down.** Snapshot becomes part of GameSession
   (or `ComputerPlayerResult`). UI reads it; the dedupe + sort logic
   in `endgame_snapshot_from_pvs` moves to core too. The
   "engine-side duplicate PV" defensive dedupe goes away once #530's
   `topk_dup_repro` is fixed upstream — keep the workaround until then.

5. **History rationalization.** Decide: does GameSession's
   `GameHistory` get formatted-string fields appended, or does each UI
   render strings on demand from canonical history? My lean is the
   latter — UI owns its rendered cache, indexed by event index. The
   "pending / spinner" entry is purely UI display state; no need for
   the engine to know about it.

After step 5, `TuiGameState` is roughly: GameSession pointer + visual
settings + glyph caches + render version + UI history cache + bot
thread handle + bot_stop atomic. Maybe ~10 fields, no engine pointers.

## Open questions

- **GameSession threading contract.** Single-threaded ownership with
  caller-managed locking? Or own a mutex? TuiGameState today uses its
  mutex to protect both UI state and engine state simultaneously,
  which is convenient. If GameSession owns its own mutex, UIs would
  hold both — annoying. Probably right: document GameSession as
  "caller owns synchronization," and let TUI's mutex cover both.

- **Mutability of GameSession from CLI vs UI**. Config mutates state
  via parser commands (`set -lex CSW21`). If a UI is running and the
  user changes a setting, both need consistent state. For the TUI this
  is moot (no CLI on top), but for a future API that exposes both
  CLI-style commands and direct mutation, we'll want clear ownership.

- **WinPct lifetime**. Currently TUI loads on init, frees on destroy.
  Should GameSession lazily load on first use (like Config does via
  `config_load_win_pcts`)? My lean: eager load if a session knows it
  wants ComputerPlayer; lazy if it doesn't. Could be a session flag.

- **Snapshot ownership across sessions.** If a single GameSession is
  reused across games (e.g., autoplay loop, "new game" without
  process restart), the snapshot from a prior game becomes invalid the
  moment `game_reset` runs. Today the bot worker clears the snapshot
  before each populate; need to make sure GameSession resets snapshot
  on game_reset.

## Explicitly out of scope for now

- Renaming Config. It's a fine name for "the CLI parser layer"; the
  problem is just that it also holds engine objects today.
- Migrating Qt/web clients. They don't exist yet; just don't *prevent*
  their existence by leaving engine state UI-coupled.
- Changing the engine's `GameHistory` shape. UI just reads it.
- Threading-model overhaul. Keep the current thread topology (UI
  thread + bot worker thread + solver internal threads) until there's
  reason to change it.

## When to start

After the etopk dedupe is fixed upstream and #530 merges, this is a
good moment to do steps 1-2. Steps 3-5 can follow when there's appetite
for the bigger refactor. Each step ships independently.

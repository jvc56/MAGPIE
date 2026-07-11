# Move entry, rack editing, and board editing — input design

Design doc for mouse + keyboard input across three overlapping
workflows in MAGPIE TUI:

1. **Move entry** — adding a move to a game (live human play,
   commentator annotation, or "add this candidate" in analysis).
2. **Rack editing** — setting or correcting either player's rack
   (commentator entering a real OTB rack; analyst trying "what if I
   had this rack instead?").
3. **Board editing** — free-form tile placement / erasure for
   analysis ("CGP editor" mode), distinct from in-game move entry.

These are different mental models and shouldn't be conflated, but
they should share keystrokes wherever the user's intent is obvious.

Reference UIs surveyed: **Woogles (liwords)** for in-game move
input; **Quackle** for analyst-driven board + rack editing; **qtpie**
(Cesar's modernized Quackle) for the candidate-list workflow.

---

## Prior art summary

### Woogles (web client) — in-game move entry

- **Click an empty board cell** to drop an anchor cursor. A small
  glyph (▶ or ▼) at the anchor shows the direction.
- **Click the same cell again** to cycle direction (across → down
  → hidden → across). Mouse-only — there's no keyboard equivalent
  for direction toggle. `Space` in their key handler instead
  advances the cursor one cell in the current direction without
  placing a tile (a "skip this square" semantic, distinct from
  what we use space for). See `liwords-ui/src/utils/cwgame/tile_placement.ts`.
- **Type a letter** → that tile (if in your rack) places at the
  anchor and advances. Played-through squares auto-skip — typing
  "RUN" across an existing "E" produces "RUNE" automatically
  (the "E" is skipped over, not consumed).
- **Shift+letter** designates a blank. The web client renders blanks
  as outlined / lowercase.
- **Backspace** retracts the most recent tile. The cursor steps back
  to where it was placed.
- **Enter** commits the move.
- **Esc** cancels the in-progress move.
- **Drag-and-drop** tiles from the rack panel to specific squares
  (mouse only). Reorder the rack by dragging within it.
- **Shuffle / sort / recall buttons** sit alongside the rack panel.
  Shuffle randomizes display order; sort restores configured
  ordering; recall pulls placed-but-uncommitted tiles back to the
  rack.
- **Pass / Exchange / Resign** are explicit buttons; exchange opens
  a sub-modal to select tiles by clicking.
- **Challenge** is post-opponent-move with a per-clock timer.

### Quackle — analyst editing

- **Annotation mode** workflow:
  - Pick which player is on turn (radio buttons).
  - Type their rack in a textbox (`AEINRST`).
  - Pick a move by selecting from the auto-generated candidate
    list (Quackle's solver fills this in), OR type a move in
    coordinate form (`8H QI`) into a textbox.
  - Click "Commit". The game state advances.
- **Manual board edit** lives in a separate dialog. You click a
  square, type a letter, blanks are entered with lowercase or via a
  pop-up. There's no in-place anchor + type flow — it's
  cell-at-a-time.
- **Rack editing** is always a textbox (one per player). No
  drag/drop.
- Quackle's strength is the candidate list — most analysts pick
  from generated plays rather than entering arbitrary ones.

### qtpie — modernized analyst tool

(Cesar's Qt-based redo. Caveat: this section is from less-precise
memory; treat as "general direction" not gospel.)

- Tighter version of Quackle's flow: click → type → enter.
- Candidate list updates live as you type a partial move.
- Sim and inference are panel-side, not modal-driven.
- Rack reorder via drag (we **don't** adopt this — see "Why no
  drag" below).
- Plays are committed to a history pane; navigating history
  rewinds the board (same as our current behavior).

### What we already have

- Per-panel focus with `[N>]` selection chips, Tab/Shift-Tab cycle.
- History cursor (arrow keys, click) with per-turn board rewind.
- Analysis cursor with rank vs. move anchoring.
- Click-to-focus and click-to-select-row.
- Slash command palette (`/new`, `/quit`, `/settings`).
- Mouse-wheel scroll on the Analysis panel.
- Readline-style text input bindings (Ctrl-A/E/B/F/D/K/U/W) in the
  Load Position / Load Game modals.

What we're missing for full input parity:

- Click-to-anchor on the board.
- Type-to-place tiles on the board.
- Rack editing (in-place, not just via slash).
- Free-form board edit mode (for analysis).

---

## Implementation vs Woogles — gap audit (June 2026)

Status of the shipped board move-entry against the Woogles survey
above. ✅ = matches Woogles (or deliberately better), ◐ = partial /
deliberate divergence, ✗ = not implemented.

| Interaction | Woogles | Ours | Status |
| --- | --- | --- | --- |
| Click empty cell → arrow | always across | always across (was a longest-run heuristic — removed; same click anchoring differently read as random) | ✅ |
| Same-cell click | cycles across → down → hidden | toggles across ↔ down (Esc hides; toggle re-derives leading playthrough for the new direction) | ◐ two-state + Esc instead of three-state cycle |
| Keyboard direction toggle | none (mouse only); arrows Left/Right flip the arrow | Space / Tab | ◐ ours is keyboard-first; arrows move the origin instead |
| Typing over board tiles | cursor auto-SKIPS occupied squares (tiles never enter an input string) | playthrough letters ABSORB into the move text (canonical "8D W(OR)D" form builds itself, incl. a leading run absorbed right after the coord) | ✅ same felt behavior, different mechanism |
| Blanks | Shift+letter = explicit blank; typed letter falls back to blank if no real tile | identical in play-vs-computer (board + cell paths); annotation requires Shift (racks are unknown there — deliberate) | ✅ |
| Tile you don't hold | keystroke ignored | keystroke ignored (rack-aware gating, playthrough-aware) | ✅ |
| Backspace | retract last tile, cursor steps back | same (skips back over playthrough); with nothing placed, steps the origin back | ✅ |
| Enter / Esc | commit / cancel | commit / cancel | ✅ |
| Space | advance cursor one square without placing | direction toggle (deliberate — see "Why not Woogles' spacebar behavior?") | ◐ |
| Click rack tile → place at cursor | yes | no (rack panel is display-only) | ✗ |
| Drag & drop tiles | yes | no (deliberate — see "Why no drag") | ✗ deliberate |
| Right-click placed tile → return to rack | yes | no (Backspace only, last-tile-first) | ✗ |
| Click blank on board → designation modal | yes | n/a (designation happens at type time via case) | ◐ |
| Recall-all / shuffle / sort | buttons + Up/Down arrows | none | ✗ |
| Pass / exchange / challenge | buttons + number hotkeys (2/3/4) | not implemented (placement-only scope) | ✗ known gap |
| Click-into-panel | single click acts | single click acts everywhere (History/Analysis previously needed focus-then-click; removed) | ✅ |

The biggest UX gaps to close next, in rough order of expected value:
pass/exchange entry (game-completeness, not just UX), recall-all
(Esc covers it but a dedicated gesture is friendlier mid-edit),
right-click-to-retract a specific tile, and click-rack-tile-to-place.

## Hotkey discipline

A rule that constrains every shortcut decision in this doc:

> **In any mode where the user might be typing tile letters
> (move entry, rack editing, board editing), every hotkey must
> use a modifier — Ctrl, Alt, etc. Bare letters are always tile
> input there. Shift-letter is reserved for blanks.**

Practical consequences:

- Action shortcuts use `Ctrl-…` (or `Alt-…`) as the prefix.
- Control keys (Tab, Enter, Esc, Backspace, arrows) are fine —
  they're not letters.
- Digits are **tentatively fine** today (panels use `0..5`), but
  see the open decision below: if we ever support typing
  coordinates (`8H POND` → anchor + play), digits collide and
  panel-focus needs to move.
- Spacebar is safe in tile-typing modes (you don't type spaces as
  letters). In move-entry mode it toggles direction. In rack /
  board edit modes it's currently unbound; if we add a meaning
  there it should be analogous (e.g. cursor direction in board
  edit).
- Even in modes where the user is NOT actively typing (e.g. Rack
  panel focused but not in edit mode), prefer modifier-prefixed
  hotkeys anyway — users shouldn't have to remember that a key's
  meaning depends on whether they've clicked into an edit yet.

---

## Click-to-focus policy

> **A click both focuses the panel AND acts on the element under the
> cursor, in one click.** No "first click focuses, second click
> selects" two-step.

Every panel with inner elements (board cells, history entries,
analysis rows) routes a click directly to the element and sets panel
focus as a side effect. The two-step pattern shipped first for
History/Analysis cursor moves and consistently read as "the click
didn't work"; the board and the pending-history-cell click never had
it, and the inconsistency made it worse. Clicks on panel chrome
(title, borders) still just focus.

---

## Modes

The same user input means different things depending on mode. Three
explicit modes:

| Mode             | Source            | Validation         | Where it ends   |
| ---------------- | ----------------- | ------------------ | --------------- |
| **Move entry**   | Live play / annotation | Full (must use rack tiles, must form valid words, must connect) | Commit → played, or cancel |
| **Rack editing** | User wants to set a rack | Distribution check (no more letters than exist; not subtracting tiles on the board) | Commit → racks now contain those tiles |
| **Board editing** | Free analysis | None (any tile, anywhere) | Commit → CGP changes, racks/bag re-derived |

The TUI surfaces the active mode via the **status bar** and a
**board cursor color**:

- Move entry: cursor in the on-turn player's accent color (green
  for P1, amber for P2). Status bar: `▶ Entering move for P1`.
- Rack editing: cursor in the rack panel, board not editable.
  Status bar: `Editing P1 rack`.
- Board editing: cursor in a neutral gray on the board. Status bar:
  `Editing board · /done to commit, Esc to cancel`.

Mode is entered via:

- Move entry: click an empty board cell (when no modal is open),
  or slash `/move` (also keyboard discoverable).
- Rack editing: click the rack panel of either player, or slash
  `/rack` (own player by default; `/rack 2` for opponent).
- Board editing: slash `/board` (or `/edit`). Mouse alone won't
  enter this mode — it's an explicit "analysis power tool" gesture.

---

## Move entry — primary flow

Triggered by clicking an empty board square OR pressing Enter while
the Board panel is focused.

### Anchor + direction

- A board cursor lands on the clicked cell.
- The cursor glyph is a directional arrow on the cursor cell:
  `▶` (across) or `▼` (down). Default direction is **always across**
  (Woogles convention). We originally shipped a "whichever direction
  has more open space" heuristic; it made the same click anchor
  differently depending on nearby tiles, which played as random —
  predictability won.
- The builder tracks two cells: the **origin** (the cell the user
  clicked — where typing starts) and the **anchor** (the true word
  start, derived by walking back through leading played-through
  tiles in the current direction). All user-facing operations —
  same-cell toggle detection, arrow-key moves, backspace past the
  word start — work on the ORIGIN; the anchor is recomputed from it.
  The first implementation operated on the walked-back anchor, which
  made the direction toggle unreachable after a playthrough walk-back
  (re-clicking your cell no longer matched the anchor) and pinned
  arrow keys against playthrough runs.
- **Toggle direction**:
  - Click the same (origin) cell again. (Woogles convention.) A
    toggle re-derives the leading playthrough for the new direction
    and re-places the user's tiles along it.
  - **Space** — keyboard equivalent. Reads as natural: "flip the
    arrow." See "Why not Woogles' spacebar behavior?" below.
  - Tab — also accepted, for consistency with the rest of the
    TUI where Tab cycles state.
- **Move the cursor**:
  - Click a different empty cell. (Cursor + anchor jump; any
    in-progress tile placements are dropped first, with a
    confirmation if non-trivial — see "Cancel" below.)
  - Arrow keys: ←↑→↓ on a focused board with the cursor visible.
  - Backspace — see below.

#### Why not Woogles' spacebar behavior?

Woogles binds spacebar to "advance the cursor one cell in the
current direction without placing a tile" (a kind of "skip this
square" gesture). We **don't** do that. Reasons:

- Auto-playthrough already eats existing tiles, so the only time
  the user would want to skip is to leave an intentional gap —
  but that creates a disconnected play that won't commit anyway.
- Spacebar = toggle direction is a stronger user instinct in a
  keyboard-first TUI.

If we ever need the skip semantic later we can promote it to a
modifier-prefixed binding.

### Typing tiles

- Pressing an uppercase letter consumes the matching rack tile,
  draws it ghosted on the board cursor cell, and advances the
  cursor by one square in the current direction.
- **Played-through** tiles are AUTO-SKIPPED: typing "RUN" across an
  existing "E" produces "R-U-N-E"-on-board where the cursor
  hopped over the "E". This matches Woogles and is the dominant
  expectation. The played-through tile is rendered in its existing
  style (not dimmed/highlighted) so the user can see what they're
  building off of.
- **Lowercase letter** (or shift+letter, see "Blanks") designates
  a blank played as that letter.
- **Tile not in rack** → soft-bell the keystroke; do nothing.
  Status bar briefly flashes `R not in rack`.
- **Cursor walks off the board** → drop the new tile, soft-bell.

### Blanks

Two acceptable schemes; pick **Shift+letter**:

- **Shift+letter**: holds the shift modifier while pressing a
  letter to mark it as a blank played as that letter. Matches
  Woogles and aligns with the on-screen rack rendering convention
  (caps = real, lowercase = blank).
- **(Alternative)** typing `?` first, then a letter, would make it
  a blank — but it adds a keystroke and is less discoverable.

Visually, the blank tile in-progress is rendered in lowercase with
a subtle underline or the blank_tile_fg color.

### Backspace / Delete / Undo

- **Backspace** — two stacked behaviors:
  1. If there's an in-progress tile *behind* the cursor in the
     current direction, **retract** it: tile returns to the rack;
     cursor steps back one cell to where that tile sat.
  2. If there's nothing to retract (no tiles in progress, or the
     cursor is already at or behind every placement), **navigate**
     instead: move the cursor one cell backward (left for across,
     up for down), even if the cell behind is empty or holds a
     pre-existing board tile.

  This lets backspace serve as a one-key "step back" — the user
  doesn't have to remember to switch from Backspace to ← / ↑ once
  they've cleared their placements. Empty + played-through cells
  are passed over freely; the cursor never refuses to move.

- **Ctrl-U / Ctrl-R**: clear the entire in-progress move; cursor
  returns to the anchor.

### Commit

- **Enter**: commit the move if it validates (uses rack tiles,
  connects to the board, forms valid words per current lexicon).
  - Live play: sends to opponent / advances turn.
  - Annotation: appends to history.
  - Analysis: drops the move into the candidate list as a
    "user-added" play (see "Add custom candidate" below).
- Validation failure: status bar shows the engine's error
  (`Phony: ZQX*`, `Disconnected from board`, etc.). Tiles stay in
  place; user can fix or cancel.

### Cancel

- **Esc**: discard the in-progress move; cursor stays, rack
  refilled.
- Clicking outside the board area cancels.
- Clicking a different empty square while tiles are in progress
  asks `Discard in-progress move?` only if 2+ tiles have been
  placed (1-tile moves cancel silently — too cheap to confirm).

### "Add custom candidate" (analysis path)

When the active panel is Analysis and the user clicks the board to
start an entry, the committed move enters the candidate list rather
than being played. Re-sim picks up the new candidate the next
iteration. UX hint in status bar: `Building custom candidate`.

### Exchange / Pass / Resign

These don't fit the "click + type" flow. Surface as slash commands
AND as a small overlay menu reachable via a dedicated key (e.g.
the spacebar in Board-focused state) so the mouse user has a path:

- `/exch ABCD` — exchange those tiles.
- `/pass` — pass.
- `/resign` — resign (live play only; double-confirms).
- Exchange UI: a sub-modal that highlights rack tiles and lets you
  click them to toggle "exchanged" status. Enter commits.

### Shuffle / sort / recall

Three "rack manipulation" affordances borrowed from Woogles. None of
these change game state — they're purely about getting the rack
into a configuration that helps the user see plays.

| Action  | What it does                                                  |
| ------- | ------------------------------------------------------------- |
| Shuffle | Randomize rack display order. Doesn't touch the engine rack — only the on-screen ordering, so the user can break out of "same five letters staring at me" mental ruts. Re-applying re-randomizes. |
| Sort    | Apply the user's configured rack sort (alphabetic, vowels-first, etc. — already a Settings option). Resets after any shuffle. |
| Recall  | Pull all in-progress tiles back from the board into the rack. Equivalent to clearing the move builder. Only meaningful while a move is being entered; a no-op otherwise. |

#### Where the affordances live

Render small clickable labels inside the Rack panel below the
tiles, with the active letter shortcut bracketed. Underline (or
bold) the shortcut letter so the mouse user sees it and the
keyboard user can find it by reading:

```
┌─[2] Rack (7) ─────────────────────────────────┐
│  A  E  I  N  R  S  T                          │
│  [s]huffle  s[o]rt  [r]ecall                  │
└───────────────────────────────────────────────┘
```

A click on any of the three labels fires the action — same as
the keyboard shortcut.

When a move is being built, the rack panel's tile area dims tiles
that are currently placed on the board (ghosted) and the `recall`
label brightens to indicate it's now useful; otherwise `recall`
renders dim/disabled.

#### Hotkeys

The shortcuts have to be modifier-prefixed because plain letters
mean "type a tile" during move entry. Use Ctrl-modified letters:

| Key      | Action  | Available in                              |
| -------- | ------- | ----------------------------------------- |
| `Ctrl-S` | Shuffle | Always (idle, move entry, rack focused)   |
| `Ctrl-O` | Sort    | Always                                    |
| `Ctrl-R` | Recall  | Move entry only                           |

Bare `s` / `o` / `r` are NOT bound — see "Hotkey discipline"
above. Even when the Rack panel is focused, the user might be a
keystroke away from entering a tile letter, and a bare-letter
binding would intercept that.

#### Why not bind to the digit row?

Two reasons:

- `0..5` are already panel-focus shortcuts.
- Digits are a candidate for *future* keyboard coordinate entry
  (typing `8H` to anchor at row 8, col H — Quackle / Macondo
  shell style). Reserving them now keeps that path open.

#### Why not autosort after every play?

The current behavior already re-sorts the rack at the start of
each turn. The Sort button is for the case where the user shuffled
mid-turn and wants to get back to sorted view.

---

## Rack editing

### Why two flows

- **In-place edit** (mouse-driven): click a rack panel → edit
  inline.
- **Slash command** (keyboard-driven): `/rack ABCDEFG`.

Both end in the same state. Both validate the same way (can't have
more of a letter than the distribution has).

### In-place edit

- Click the rack panel (matching the focused player; click P2 pill
  to edit P2). Cursor lands on the first rack tile; status bar:
  `Editing P1 rack · type letters · Enter to commit · Esc to cancel`.
- Existing tiles render normally; the editor surfaces an INSERT
  caret between tiles.
- **Type a letter**: appends that tile to the rack.
- **Backspace**: removes the last tile.
- **Ctrl-U**: clears the rack.
- **`?`**: appends a blank.
- **Enter**: commits. If the tiles aren't available (the bag +
  other player don't have enough to satisfy this rack), error
  flashes; rack reverts to prior contents.
- **Esc**: revert.

### Behavior when sim/endgame is mid-flight

Rack edits invalidate any active sim. Show a confirmation
(`Discard ongoing sim?`) only if the sim has > N iterations of
real work. Otherwise just cancel-and-rerun silently.

### Two-player edit

The rack panel shows P1's rack normally. Editing P2's rack from a
TUI focused on P1 should be possible without context-switching:

- Click the P2 score pill to focus + edit P2's rack.
- Or `/rack 2 ABCDEFG`.

---

## Board editing (analysis power tool)

Distinct from move entry. Enables corrections and "what if the
position were different" exploration.

### Entry

- `/board` (or `/edit`) toggles board-edit mode.
- Status bar makes the mode obvious: `Editing board · arrows + type
  · /done to commit, Esc to cancel`.

### Cursor

- A neutral-gray cursor cell, navigable with arrows / mouse click.
- Direction is set the same way as move entry, but here it just
  controls which way typing advances — there's no "play" being
  built.

### Typing

- Type a letter: places that tile (regardless of rack). Cursor
  advances.
- Lowercase / Shift+letter: blank.
- **Backspace**: erase the tile under the cursor; cursor stays.
- **Delete**: erase the tile and advance.

### What's mutated

- The board itself.
- The bag is automatically rebalanced: any tile placed comes out
  of the bag; any tile erased goes back. Racks are not touched.
- If the placement leaves the bag with negative counts (you placed
  more A's than exist), `/done` rejects the commit and surfaces
  the imbalance.

### Commit

- `/done` (or Enter on an empty cell when no tile is being typed):
  validate the resulting position is legal CGP; if so, snap the
  TUI into the new state. The history gets a `[manual board edit]`
  marker entry.
- `Esc`: revert to the pre-edit board.

### Why not just `/cgp <string>`?

We already support that (Load Position modal). Board-edit mode is
the interactive equivalent — same destination, mouse-friendly
input.

---

## Mouse interaction reference

| Gesture                          | Where           | Action                                                    |
| -------------------------------- | --------------- | --------------------------------------------------------- |
| Click empty board cell           | Game / Analysis | Anchor cursor, enter move entry                           |
| Click same cell again            | Move entry      | Toggle direction                                          |
| Click different empty cell       | Move entry      | Move anchor (confirm if 2+ tiles already placed)          |
| Click occupied board cell        | Game / Analysis | Anchor before the start of the word that cell is part of (so you can extend a word) |
| Click occupied cell              | Board edit      | Move cursor onto that cell (for replacement / erase)      |
| Click rack tile                  | Game / Analysis | "Press" that letter — same as typing it; tile slides to cursor |
| Click `[s]huffle` label          | Rack panel      | Shuffle rack display order                                |
| Click `s[o]rt` label             | Rack panel      | Sort rack (apply configured sort)                         |
| Click `[r]ecall` label           | Rack panel (move entry) | Pull all in-progress tiles back to rack            |
| Click P1 / P2 pill               | Any             | Focus that player's rack; click again to enter edit       |
| Click history entry              | History focused | Rewind to that turn (existing behavior)                   |
| Click analysis row               | Analysis focused | Move analysis cursor (existing behavior)                 |
| Right-click a board cell         | Anywhere        | Context menu: "Set anchor here", "Edit this cell" (cells with tiles), "Erase" (in board-edit mode) |
| Wheel scroll                     | Analysis        | Scroll candidate list (existing)                          |
| Wheel scroll                     | History         | Scroll history list (new, low priority)                   |

### Why no drag (anywhere)

Drag-based gestures don't translate to a terminal. Character cells
are a coarse grid — a "drag" updates one cell at a time, so the
follower never moves smoothly under the cursor. What feels natural
on a web client (Woogles' tile-from-rack-to-board) reads as jittery
and broken in a terminal.

Everywhere prior art uses drag, we bridge with click-then-click:

- **Drag rack tile to a board cell** → **click rack tile** (which
  picks the letter up — see "Click rack tile" above) **+ click
  board cell** (places it at the cursor).
- **Drag tiles to reorder the rack** → out of scope entirely. Rack
  ordering is cosmetic; the existing sort settings + alphagram
  modes already cover it.

This is a general TUI constraint, not specific to move entry.

---

## Keyboard reference

### Mode-agnostic shortcuts (always work)

| Key            | Action                                                |
| -------------- | ----------------------------------------------------- |
| `Tab` / `Shift+Tab` | Cycle panel focus (existing)                     |
| `1`..`5`       | Jump to panel N (existing)                            |
| `0`            | Open slash command palette (existing)                 |
| `Esc`          | Cancel current mode / close modal                     |
| `Ctrl-C`       | Quit (with confirm, existing)                         |
| `Ctrl-S`       | Shuffle rack display order                            |
| `Ctrl-O`       | Sort rack (apply current sort setting)                |

(All shortcuts are modifier-prefixed — see "Hotkey discipline"
above. There are no bare-letter bindings, even when a non-typing
panel has focus, because the user might enter a typing mode with
one keystroke and a bare-letter binding would surprise them.)

### Move entry

| Key            | Action                                                |
| -------------- | ----------------------------------------------------- |
| `A`..`Z`       | Place that letter (consumes rack tile)                |
| `Shift`+letter | Place blank designated as that letter                 |
| `?`            | Place blank, undesignated (deferred — Shift is preferred) |
| `Backspace`    | Retract last tile if any; else navigate cursor backward one cell |
| `Ctrl-U` / `Ctrl-R` | Recall — pull all in-progress tiles back to rack |
| `Space`        | Toggle across/down (also: `Tab`)                      |
| `←↑→↓`         | Move the cursor (drops any in-progress tiles)         |
| `Ctrl-S`       | Shuffle rack (cosmetic; doesn't drop in-progress tiles) |
| `Ctrl-O`       | Sort rack (cosmetic)                                  |
| `Enter`        | Commit                                                |
| `Esc`          | Cancel                                                |

### Rack editing

| Key            | Action                                                |
| -------------- | ----------------------------------------------------- |
| `A`..`Z`       | Append tile                                           |
| `?`            | Append blank                                          |
| `Backspace`    | Remove last tile                                      |
| `Ctrl-U`       | Clear rack                                            |
| `Enter`        | Commit                                                |
| `Esc`          | Revert                                                |

### Board editing

| Key            | Action                                                |
| -------------- | ----------------------------------------------------- |
| `A`..`Z`       | Place tile at cursor; advance                         |
| Lowercase or `Shift`+letter | Place blank-as-that-letter; advance      |
| `Backspace`    | Erase under cursor; navigate cursor backward one cell |
| `Delete`       | Erase under cursor; advance                           |
| `Space`        | Toggle across/down (also: `Tab`)                      |
| `←↑→↓`         | Move cursor                                           |
| `Enter` / `/done` | Commit                                             |
| `Esc`          | Revert                                                |

---

## Status-bar grammar

The status bar always tells the user what mode they're in and the
two or three most useful keys. Examples:

- Idle: `[Tab] focus · [0] cmd · [Esc] menu`
- Move entry (across, 0 tiles placed):
  `▶ Move entry · type tiles · [Tab] direction · [Enter] commit · [Esc] cancel`
- Move entry (3 tiles placed, valid):
  `▶ Move entry · 8H POND · [Enter] commit · [Ctrl-R] recall · [Esc] cancel`
- Move entry (invalid):
  `▶ Move entry · 8H PNDO · ⚠ phony · [Backspace] fix · [Esc] cancel`
- Rack edit:
  `Editing P1 rack · ABCDEFG · [Enter] commit · [Esc] cancel`
- Board edit:
  `Editing board · [Tab] direction · [/done] commit · [Esc] cancel`

---

## Open decisions

These are real branching choices to make before implementation:

### 1. Auto-extend through existing letters?

Woogles auto-skips played-through tiles ("RUN" across "E" → "RUNE").
The alternative is to require explicit playthroughs (typing `.` for
each played-through cell). Auto is friendlier; explicit is more
predictable for the analyst.

**Recommendation:** auto-skip. Match Woogles. Power users who want
explicit playthroughs can type letters one at a time and use arrow
keys to step.

### 2. Enter commits, or "stage as proposed"?

For analysis, committing every Enter clutters the candidate list
fast. Alternative: Enter stages the move as a "proposed move"
shown highlighted in the Analysis panel; a second Enter commits;
Esc unstages.

**Recommendation:** stage-then-commit in **analysis** mode; direct
commit in **live play** mode (the staging step would just slow the
player down under clock). Annotation mode = direct commit since
the user has already decided what was played.

### 3. Right-click as an action button?

A native-feeling app has a right-click context menu (Set anchor,
Edit cell, Erase…). Notcurses supports BUTTON3 events but the
context-menu rendering is non-trivial. Without it, all those
actions still exist via slash or keyboard.

**Recommendation:** ship without right-click in v1. Revisit when
we have a real reason to add a context-menu widget.

### 4. Click rack tile semantics?

Two interpretations:
- (a) Acts like typing the letter — sends it to the current cursor.
- (b) Picks the tile up; next click on the board places it.

(a) is closer to Woogles' final outcome but loses the "pick up
THIS specific copy of A" precision. (b) matches the drag model
without the drag.

**Recommendation:** (a). Cursor model is unambiguous enough that
specifying a particular rack copy is rarely useful.

### 5. Concealment in human-vs-computer mode

Phase 2 already has this open. For move entry, the issue is:
when the bot is on turn, should the user be able to anchor the
cursor and "preview" plays? Yes, but those previews should be
clearly NOT-going-to-be-committed — a different cursor color and
no Enter binding.

**Recommendation:** during opponent turn, allow move-entry as a
"what if I had to play" exploration but Enter is a no-op. Useful
for studying mid-game.

### 6. Tile selector for exchange

Modal vs. inline. A modal that lists rack tiles with checkboxes
is the most discoverable; inline (hold a modifier, click a tile
to toggle) is faster for power users.

**Recommendation:** modal in v1; revisit if it feels slow.

### 7. Discoverability of slash commands

The slash palette already exists. Should mode names be tab-
completable? Should `/help` list every binding?

**Recommendation:** `/help` lists every binding grouped by mode.
Tab completion in the slash palette is a separate small win.

### 8. Keyboard coordinate entry?

Quackle's textbox and Macondo's shell let the user type a full
move as text — `8H POND` — without clicking. This is fast for
keyboard-first users and matches workflows imported from existing
analysis tools.

If we add this, **digits become unsafe as bare hotkeys**, including
the existing `0..5` panel-focus shortcuts. We'd need to:

- Move panel-focus to a modifier (`Ctrl+1..5` or `Alt+1..5`), and
- Either gate coordinate entry on a mode flag (slash command,
  e.g. `/move 8H POND`) or detect the typed prefix unambiguously
  (a leading digit on the Board panel always means "starting a
  coordinate").

**Recommendation:** defer coordinate entry past v1; the click +
type flow covers the common case. But **don't introduce any new
bare-digit hotkeys** before this decision — leave the namespace
open.

---

## Suggested ship sequence

Smallest useful slice first. Each step is a real upgrade.

1. **Board anchor + arrow nav (no typing yet).** Click an empty cell,
   anchor lands, arrows move it, Tab toggles direction. No tiles
   yet — proves the cursor + status-bar plumbing.
2. **Typing tiles in move-entry mode.** Letters consume rack and
   advance. Backspace works. Enter validates and (annotation
   mode) commits. Sim/analysis paths come later.
3. **Blanks via Shift+letter.**
4. **Rack edit in place** (replaces or augments the existing
   `/rack` slash; both fine).
5. **Analysis "stage as candidate" path.** Sim picks it up.
6. **Exchange modal + pass / resign keystrokes.**
7. **Board edit mode** (the analyst power tool). `/board` to
   enter, free-form placement, `/done` to commit.
8. **Mouse refinements:** right-click context menu, mouse-wheel
   history scroll. (No drag — see "Why no drag" above.)

Steps 1-4 cover the Causeway commentary case; steps 5-6 cover
analysis power users; steps 7-8 are polish.

---

## What we explicitly take from each prior-art system

| From       | What we adopt                                                                 |
| ---------- | ----------------------------------------------------------------------------- |
| **Woogles** | Click-to-anchor, click-again-to-toggle, type-to-place, auto-skip playthroughs, Shift+letter for blanks, Backspace to retract, Enter to commit, Esc to cancel. The whole click-and-type idiom. The shuffle / sort / recall affordances by the rack. |
| **Quackle** | The "annotation mode" mental model: rack + move per turn, no notion of clocks. The candidate-list-first analyst flow. Textbox-style rack entry as a fallback. |
| **qtpie**   | Live update of the candidate list as a partial move is typed. The cleaner separation of analyst vs. player flows. (We skip qtpie's rack drag-reorder — terminals can't render drag smoothly.) |
| **(ours)**  | Slash command palette as a power-user shortcut, status bar grammar, panel focus model, history-cursor rewind. These already work and shouldn't change. |

---

## Non-goals for v1

To keep the work tractable:

- **Any drag gesture.** Terminals are character-cell grids, so drag
  feedback jitters cell-by-cell instead of moving smoothly under
  the cursor. Drag-to-place is bridged with click-rack + click-
  board; drag-to-reorder-rack is dropped entirely (the alphagram
  and rack-sort settings already cover the underlying need).
- Animations / tile-fly-in effects on commit.
- Sound effects on tile placement.
- Touch input (irrelevant for terminals).
- Custom keybindings (`.config` driven). Use the defaults; revisit
  if users complain.
- Bidirectional language layouts (RTL, vertical scripts).
- Tile-fonts beyond the bundled freetype rasterizer.

---

## Implementation breadcrumbs

(For the future implementer — not to be read as a commitment.)

- Move-entry state goes on `TuiGameState` next to the existing
  cursor fields. Two new fields probably suffice: a `MoveBuilder`
  (anchor row/col, dir, an array of `(row, col, ml, is_blank)`
  placements) and an `input_mode` enum.
- Render goes through `pick_render_board`-style indirection: when a
  move is being built, the board renderer overlays the in-progress
  tiles on top of the underlying board. Existing playthrough
  detection already does the bookkeeping needed to skip filled
  squares.
- Validation reuses `validated_moves_create` (the GCG parser
  already calls into it).
- Rack editing requires mutating both the engine `Rack` and the
  bag. There's an existing helper for the inverse (`return_rack
  _to_bag`); we'll need its counterpart.
- Board editing similarly uses the existing tile-set + bag-balance
  helpers.
- Status-bar grammar: extend the current `tui_status_*` plumbing
  with a mode-aware variant that takes a structured "tips" list and
  truncates as needed.
- Mouse coordinates → cell coordinates: `tui_game_panel_at` already
  hit-tests panels; we'll need a board-cell hit-test too. The
  existing layout's `board_top_row` / `board_width` make this a
  one-liner per axis.

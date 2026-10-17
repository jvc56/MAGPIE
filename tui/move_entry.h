#ifndef TUI_MOVE_ENTRY_H
#define TUI_MOVE_ENTRY_H

#include "../src/ent/board.h"
#include "../src/ent/move.h"
#include "game_state.h"
#include <stdbool.h>
#include <stddef.h>

// ── Move entry — the common core behind both input surfaces ─────────
//
// The TUI has two surfaces for entering a move:
//
//   1. BOARD ENTRY — click (or keyboard-begin) an empty cell, type
//      tiles, Space/Tab to toggle direction, arrows to relocate,
//      Enter to submit. Driven by the builder state below
//      (origin/anchor/direction) with the buffer always cursor-at-end.
//   2. CELL ENTRY — free-text editing of the history entry's MOVE
//      field ("8D WORD", "ex ABC", "pass"). The annotation workflow
//      needs arbitrary text editing (revising committed turns,
//      entering exchanges), so this surface keeps its own cursor and
//      insert mechanics.
//
// Both share ONE semantic core defined here: the same letter
// resolution (rack gating, blank fallback, played-through
// pass-through — mode-aware), the same playthrough absorption, the
// same landing-square geometry, and the same commit paths. The two
// surfaces differ only in buffer mechanics; everything that decides
// WHAT a keystroke means lives in this module. (History: nearly every
// input bug came from these semantics being implemented twice and
// drifting — see NOTES_input_design.md.)
//
// All functions that take a non-const TuiGameState require the caller
// to hold gs->mutex.

// What a typed letter resolves to, given the landing square and the
// input mode.
typedef enum {
  TUI_TYPED_LETTER_REJECT = 0,
  // Place a tile: out_glyph is 'A'-'Z' for a real rack tile or
  // 'a'-'z' for a blank designated as that letter.
  TUI_TYPED_LETTER_PLACE = 1,
  // The landing square already holds this letter: spell it as
  // playthrough. out_glyph carries the BOARD tile's notation case
  // (lowercase when the board tile is a designated blank).
  TUI_TYPED_LETTER_PLAYTHROUGH = 2,
} TuiTypedLetterAction;

// Resolve one typed letter. land_row/land_col is the board square the
// letter would land on (pass -1,-1 when unknown / not applicable).
// Mode policy:
//   - play-vs-computer: racks are real and known. A letter landing on
//     a matching occupied square is playthrough; on a mismatched
//     occupied square it's rejected (collision). On an empty square
//     it must be coverable by the human's remaining tiles — an
//     unshifted letter falls back to a blank when the real tile is
//     exhausted; Shift explicitly requests a blank; neither available
//     rejects the keystroke.
//   - watch/annotate: free text (the annotator owns the racks).
//     Shift+letter = blank (lowercase), otherwise uppercase. A letter
//     matching an occupied landing square still resolves as
//     playthrough so the buffer picks up the canonical notation case.
TuiTypedLetterAction tui_move_entry_resolve_letter(const TuiGameState *gs,
                                                   char typed, bool shift,
                                                   int land_row, int land_col,
                                                   char *out_glyph);

// Board square the next typed WORD letter lands on, derived from the
// MOVE buffer: the coord token's square advanced along the play
// direction by the number of word letters before the cursor
// (played-through letters absorbed into the buffer count, which keeps
// the position board-aligned). Works for both surfaces — board entry
// is simply the cursor-at-end case. Returns false when the buffer has
// no parseable coord token yet.
bool tui_move_entry_landing_square(const TuiGameState *gs, int *out_row,
                                   int *out_col);

// Append one typed letter at the end of the MOVE buffer: resolves it
// (landing square + mode policy), inserts the coord/word space when
// needed, re-parses, and absorbs any played-through tiles the word now
// runs into. The shared whole-keystroke path for board entry and for
// cell entry when the cursor sits at the end. Returns true when a
// glyph was added.
bool tui_move_entry_append_letter(TuiGameState *gs, char ch, bool shift);

// Absorb contiguous played-through board tiles at the end of the
// current preview move into the MOVE buffer (with the coord/word space
// when the buffer is still just a coord). No-op without a valid
// placement preview.
void tui_autofill_playthrough(TuiGameState *gs);

// ── Board-entry builder state ────────────────────────────────────────

// Begin (or relocate) board move-entry at origin (row, col) with the
// given direction; walks the anchor back through leading playthrough
// and seeds the buffer with those letters. Drops previously-placed
// tiles.
void tui_board_builder_set_anchor(TuiGameState *gs, int row, int col, int dir);

// Direction for a fresh anchor: always across (Woogles convention).
int tui_board_builder_default_dir(const TuiGameState *gs, int row, int col);

// Toggle across/down, re-anchoring from the origin and re-placing the
// user's tiles along the new direction.
void tui_board_builder_toggle_dir(TuiGameState *gs);

// Cancel board entry: return placed tiles to the rack, exit edit mode.
void tui_board_builder_cancel(TuiGameState *gs);

// Begin board entry from the keyboard (no mouse): anchors at the last
// origin used this game when it's still an empty cell, else the first
// empty cell scanning from the board center. No-op in watch mode, with
// a modal up (caller's job), or when the game is over.
void tui_board_entry_begin_keyboard(TuiGameState *gs);

// Retract the last placed tile (skipping trailing playthrough); with
// nothing placed, step the origin back one cell.
void tui_board_entry_backspace(TuiGameState *gs);

// Submit the in-progress board move: play-vs-computer plays with
// drawing and hands the turn to the bot; annotation plays without
// drawing and seeds the next pending turn.
void tui_board_entry_submit(TuiGameState *gs);

// ── Commits ──────────────────────────────────────────────────────────

// Commit the current edit-buffer preview as the human's play in a live
// play-vs-computer game (draw tiles, finalize the pending entry,
// charge the clock, hand off to the bot). Shared by board-entry Enter
// and cell-entry Enter. Returns true when a move was committed.
bool tui_pvc_commit_preview_move(TuiGameState *gs);

// Tag a placed move's squares with the player index so the renderer
// colors them.
void tui_tag_move_owners(Board *board, const Move *move, int player_idx);

#endif

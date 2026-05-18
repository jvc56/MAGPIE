#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include "game_state.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

struct Game;
struct LetterDistribution;

// Which modal (if any) is currently open. Drives both rendering of the
// modal itself and what control hints the status bar advertises.
typedef enum {
  TUI_MODAL_NONE = 0,
  TUI_MODAL_MAIN_MENU = 1,
  TUI_MODAL_SETTINGS = 2,
  TUI_MODAL_TIME_PICKER = 3,
  TUI_MODAL_LEXICON_PICKER = 4,
  TUI_MODAL_QUIT_CONFIRM = 5,
} TuiModalState;

// Which panel currently has keyboard focus. NONE means no panel is
// focused — pressing 1..5 selects the matching panel; 0 unfocuses.
// Panel-specific behaviors (history scrollback, analysis candidate
// preview, etc.) consult this enum to decide whether to intercept
// keys; render code uses it to bold/highlight the focused panel's
// [N] indicator.
typedef enum {
  TUI_FOCUS_NONE = 0,
  TUI_FOCUS_BOARD = 1,
  TUI_FOCUS_RACK = 2,
  TUI_FOCUS_BAG = 3,
  TUI_FOCUS_HISTORY = 4,
  TUI_FOCUS_ANALYSIS = 5,
} TuiPanelFocus;

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal);

// Main menu modal — rendered on top of the game frame when the user
// presses Esc.
typedef enum {
  TUI_MENU_NEW_GAME = 0,
  TUI_MENU_SETTINGS = 1,
  TUI_MENU_BACK = 2,
  TUI_MENU_QUIT = 3,
  TUI_MENU_ITEM_COUNT = 4,
} TuiMenuItem;

void tui_game_render_menu(struct ncplane *plane, const Theme *theme, int focus);

// Time-picker modal — opened from "New game" in the main menu.
// Items come from time_picker.h's preset accessors.
void tui_game_render_time_picker(struct ncplane *plane, const Theme *theme,
                                 int focus);

// Quit-confirmation modal. Two items: No (focus 0) / Yes (focus 1).
// Default focus is 0 (No) so the safer option is Enter-confirmable;
// Y / N shortcuts trigger their action regardless of current focus.
void tui_game_render_quit_confirm(struct ncplane *plane, const Theme *theme,
                                  int focus);

// Settings modal — opened from the main menu. Left/Right arrows on the
// focused row adjust that setting's value.
typedef enum {
  TUI_SETTINGS_SCALE = 0,
  TUI_SETTINGS_AA = 1,
  TUI_SETTINGS_SUBSCRIPTS = 2,
  TUI_SETTINGS_BORDER = 3,
  TUI_SETTINGS_PREMIUM = 4,
  TUI_SETTINGS_BLANKS = 5,
  TUI_SETTINGS_LEXICON = 6,
  TUI_SETTINGS_RIT = 7,
  TUI_SETTINGS_BACK = 8,
  TUI_SETTINGS_ITEM_COUNT = 9,
} TuiSettingsItem;

// `board_scale` is 1 or 2; the scale row is grayed out when 2x is
// unavailable (no pixel support or font load failed). `antialias`
// applies to the 2x render only and is grayed at 1x. `score_subscripts`
// is also 2x-only. `border_thickness` is the current pixel-grid
// thickness (0..6). `pixel_supported` is true when the host terminal
// can render pixel graphics. `font_available` is true when the bundled
// TTF loaded. `premium_labels` selects the TW/tw/none labeling style
// for premium squares. `blank_uppercase` controls whether played blanks
// render uppercase (with blank_tile_fg) or lowercase (with tile_fg).
// Shared modal-plane accessor (used by render_modal and the lexicon
// picker). Creates the plane on first use; resizes/repositions on
// subsequent calls. The plane is destroyed when tui_game_render sees
// modal == TUI_MODAL_NONE, so callers don't manage its lifetime.
struct ncplane *
tui_game_render_get_or_create_modal_plane(struct ncplane *parent, int top,
                                          int left, int rows, int cols);

// Hit-test a (y, x) cell coordinate against the panel layout that
// tui_game_render would draw for the given state on the given plane.
// Returns the TuiPanelFocus index of the panel containing the click,
// or -1 if the click misses every panel.
int tui_game_panel_at(struct ncplane *plane, const TuiGameState *state, int y,
                      int x);

// Hit-test (y, x) against the History panel's last-rendered entry
// rectangles. Returns:
//   -2 — the point is outside the History panel entirely
//   -1 — inside the panel but not on a specific entry row (title,
//        chrome, empty space below the entries) — caller should
//        snap history_cursor to -1 (the [4>] label)
//   0..N-1 — the history entry index that was clicked
//
// Reads a per-frame cache the renderer populates each tick, so this
// is only meaningful right after tui_game_render has run on the
// same plane geometry.
int tui_history_cursor_at(int y, int x);

// Analysis cursor column. The Analysis cursor remembers not just
// which row it's on but which "column" — rank-anchored (cursor
// pins to the row index, ignoring sim reorderings) or move-
// anchored (cursor pins to a specific move and follows it as the
// leaderboard reorders). Left/Right arrows toggle the column.
typedef enum {
  TUI_ANALYSIS_COLUMN_RANK = 0,
  TUI_ANALYSIS_COLUMN_MOVE = 1,
} TuiAnalysisColumn;

// Hit-test (y, x) against the Analysis panel's last-rendered row
// rectangles. Returns:
//   -2 — point is outside the Analysis panel
//   -1 — inside the panel but not on a specific row (title /
//        column-header strip / blank space below the list); caller
//        should snap analysis_cursor back to -1 (the [5>] label)
//   0..N-1 — the row index that was clicked
int tui_analysis_cursor_at(int y, int x);

// Like tui_analysis_cursor_at, but also reports which column of the
// row was clicked via *out_column. Returns the same row-index code
// as tui_analysis_cursor_at. *out_column is set to the appropriate
// TuiAnalysisColumn value when the click hits a row, and is left
// unchanged otherwise.
int tui_analysis_cursor_column_at(int y, int x, TuiAnalysisColumn *out_column);

// Populate a snapshot of the Analysis-panel contents for the
// currently-active sim or endgame solve. Called by the bot worker
// at finalize time (just after a move is chosen, just before
// play_move advances the board) so each history entry can
// preserve the analysis the user was looking at when the bot
// committed. Picks sim vs endgame the same way the live render
// does: endgame when the bag is empty and the endgame snapshot
// is valid, sim otherwise. Safe to call from any thread that
// already holds state->mutex.
void tui_capture_analysis_snapshot(const TuiGameState *state,
                                   TuiAnalysisSnapshot *out);

void tui_game_render_settings(
    struct ncplane *plane, const Theme *theme, int focus, int board_scale,
    bool antialias, TuiScoreSubscripts score_subscripts, int border_thickness,
    bool pixel_supported, bool font_available, TuiPremiumLabels premium_labels,
    bool blank_uppercase, const char *lexicon, bool load_rit);

// Render just the board cells (no row/col labels) at (top, left). Each
// cell is 2 columns wide; the rendered region is BOARD_DIM rows tall and
// BOARD_DIM*2 columns wide. When the host terminal supports pixel
// graphics and `border_thickness` is positive, a grid overlay is drawn
// on top using a private cached child plane.
void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const struct Game *game,
                         const struct LetterDistribution *ld,
                         bool blank_uppercase, TuiPremiumLabels premium_labels,
                         int border_thickness);

// Destroy any cached pixel-grid child planes (board, rack, both pills,
// preview). Call after the theme picker exits so the preview's grid
// doesn't linger under the in-game UI, and on resize so a font-size
// change rebuilds them at the new cell-to-pixel ratio.
void tui_game_render_reset_grids(void);

#endif

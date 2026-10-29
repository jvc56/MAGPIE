#ifndef TUI_RENDER_HIT_TEST_H
#define TUI_RENDER_HIT_TEST_H

#include "game_state.h"
#include "render_layout.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

// Per-entry hit-test rectangles, populated by render_history_panel
// on every frame so mouse clicks can locate which turn the user
// tapped. Sized to TUI_HISTORY_MAX so a fully-populated history
// panel can still be hit-tested without truncation.
typedef struct {
  int top_row;    // first screen row of entry, inclusive
  int bottom_row; // last screen row of entry, inclusive
  int left_col;   // first column, inclusive
  int right_col;  // last column, inclusive
  int idx;        // history entry index
  // Leave-column hit-test rect. The leave glyph sits on the move
  // row (top_row), right-aligned just inside the score column.
  // Click X in [leave_left, leave_right] on top_row → leave field.
  // -1 in leave_left means "no leave column rendered for this
  // entry" (e.g., spinner / divider rows).
  int leave_left;
  int leave_right;
} HistoryRowMap;
// Per-row hit-test rectangles for the Analysis panel. The cap
// matches ANALYSIS_ROW_CAP (max candidates shown). Same idea as
// the history rect map: filled during the analysis render so
// mouse clicks can be mapped back to a candidate row.
typedef struct {
  int top_row;
  int bottom_row;
  int left_col;
  int right_col;
  // Screen column where the "move" text column begins. Clicks at
  // x < move_left_col land on the rank column; x >= move_left_col
  // land on the move column. Lets the click handler decide which
  // TuiAnalysisColumn to switch the cursor to.
  int move_left_col;
  int idx;
} AnalysisRowMap;
// Per-frame hit-test data for the currently-open modal (when one
// is open and routed through render_modal_ex). All coordinates
// are absolute screen rows/cols. invalidated when no modal
// renders this frame.
enum { MODAL_MAX_ITEMS = 16 };
typedef struct {
  int top;        // screen row of modal interior top (item rows)
  int left;       // screen col of modal interior left (clickable area)
  int right;      // screen col of modal interior right
  int item_count; // number of items rendered
  bool disabled[MODAL_MAX_ITEMS];
  // Per-row chevron screen columns: where the ◀ and ▶ glyphs sit
  // on rows that include them (focused settings/watch-setup
  // rows). -1 when the row has no chevron. Lets click handlers
  // detect "user clicked the left/right chevron to adjust" vs.
  // "user clicked the row to focus."
  int left_chev_col[MODAL_MAX_ITEMS];
  int right_chev_col[MODAL_MAX_ITEMS];
  // Modal outer bounding box, used to differentiate "click inside
  // modal but not on an item" from "click outside modal."
  int outer_top;
  int outer_bottom;
  int outer_left;
  int outer_right;
  bool valid;
} ModalHitMap;

// Hit-test geometry recorded by the panel renderers every frame and read
// by the tui_*_at functions below. UI-thread only.
typedef struct {
  // Per-entry History rectangles, filled by render_history_panel.
  HistoryRowMap history_row_map[TUI_HISTORY_MAX];
  int history_row_map_count;
  // Bounding box of the History panel as a whole — used by
  // tui_history_cursor_at to detect clicks on the title / chrome
  // (between entries) and snap the cursor back to -1 (the [4>]
  // label). Refreshed every frame alongside the row map.
  int history_panel_top;
  int history_panel_bottom;
  int history_panel_left;
  int history_panel_right;
  // Per-row Analysis rectangles, filled by render_analysis_rows.
  AnalysisRowMap analysis_row_map[ANALYSIS_ROW_CAP];
  int analysis_row_map_count;
  int analysis_panel_top;
  int analysis_panel_bottom;
  int analysis_panel_left;
  int analysis_panel_right;
  // The currently-open modal, filled by render_modal_ex.
  ModalHitMap modal_hit_map;
} TuiHitMaps;

// The renderers write through this pointer while drawing a frame.
TuiHitMaps *tui_hit_maps(void);

// Hit-test a (y, x) cell coordinate against the panel layout that
// tui_game_render would draw for the given state on the given plane.
// Returns the TuiPanelFocus index of the panel containing the click,
// or -1 if the click misses every panel.
int tui_game_panel_at(struct ncplane *plane, const TuiGameState *state, int y,
                      int x);

// Map a screen (y, x) cell coordinate to a board cell. Returns true and
// fills *out_row / *out_col (both 0-based, 0..BOARD_DIM-1) when the point
// lands inside the 15x15 grid; false otherwise. Uses the same layout /
// scale selection as tui_game_panel_at, so it handles both 1x text and
// 2x pixel board rendering. Only meaningful right after tui_game_render
// has run on the same plane geometry.
bool tui_board_cell_at(struct ncplane *plane, const TuiGameState *state, int y,
                       int x, int *out_row, int *out_col);

// Hit-test (y, x) against the most-recently-rendered modal (one
// of the modals routed through render_modal[_ex]: main menu,
// startup menu, settings, time picker, quit confirm, watch
// setup). Returns:
//   -2 — point is outside the modal entirely
//   -1 — point is inside the modal but on chrome (title, border,
//        or a disabled item). Caller should absorb the click
//        without activating anything.
//   0..N-1 — the item index that was clicked, ready for the
//        caller to treat as if the user pressed Enter on that
//        focused item.
int tui_modal_item_at(int y, int x);

TuiModalChevron tui_modal_chevron_at(int y, int x);

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

// Like tui_history_cursor_at, but also reports which row of the
// entry was clicked: 0 = row 1 (the move text); 1 = row 2 (the
// rack); >= 2 = end-bonus rows (treated as MOVE for now). Used
// by the annotation cell editor to route clicks to the right
// edit field. *out_field is set only when the return is >= 0.
int tui_history_cursor_field_at(int y, int x, int *out_field);

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

#endif

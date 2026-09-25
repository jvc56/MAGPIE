#ifndef TUI_RENDER_LAYOUT_H
#define TUI_RENDER_LAYOUT_H

#include "../src/def/board_defs.h"
#include "game_state.h"
#include <stdbool.h>

// ── Layout ────────────────────────────────────────────────────────────────
//
//   left column (cols 0..L.board_right_col):
//     row 0:                           column labels Ａ..Ｏ
//     rows 1..15:                      board cells (row label at col 0)
//     rows L.rack_top..L.rack_bottom:  rack panel (3 rows)
//     rows L.bag_top..L.bag_bottom:    bag panel  (fills remaining height)
//
//   right column (cols L.right_col_left..L.right_col_right):
//     rows 0..2:                       Player 1 pill
//     rows 3..5:                       Player 2 pill
//     rows L.history_top..L.history_bottom:
//                                      history panel (fills remaining height)
//
//   row L.status_row:                  status bar (full width)
//
// Board cells are 2 cols wide. Tiles render as fullwidth Ａ-Ｚ on tile_bg.
// Empty premium squares render as 2-char ASCII labels (TW/DW/TL/DL, or
// their lowercase forms, or blank) on the premium_*_bg tint. Empty non-
// premium cells render as an ideographic space on board_bg. Column labels
// are fullwidth Ａ-Ｏ. Rack tiles are fullwidth letters on tile_bg, no gap.
// Everything else is halfwidth ASCII; if a rack ever appears inline in a
// history row, render that one halfwidth too.

enum {
  CELL_WIDTH = 2, // 1x board tile width in cols (and rack tile width always)
  // The board widget gets its own 1-col border on all four sides, so
  // everything inside (labels + cells) shifts 1 col right and 1 row
  // down from where it'd sit without a border. The title "Board (N)"
  // lives on the top border row.
  ROW_LABEL_COL = 1,
  // Row labels are 2 cols wide ("%2d"); the board sits flush against
  // them so the column-label letters land directly above the cells they
  // describe instead of being floated one extra column off.
  CELL_COL_BASE = ROW_LABEL_COL + 2,
  COL_LABELS_ROW = 1,
  CELL_ROW_BASE = 2,
  // 1 col on the right edge of the board widget for the box border.
  BOARD_RIGHT_BORDER = 1,
  RACK_HEIGHT = 3,
  PILL_HEIGHT = 3,
  RIGHT_COL_LEFT_OFFSET = 0, // right column sits flush against the board
  RIGHT_COL_MIN_WIDTH = 32,  // narrowest the right column can be
  // Two-column history thresholds, by pill-rack rendering mode.
  // FULLWIDTH: each rack tile takes 2 cols (cleaner look) — needs
  // ~33 cols per pill (`▶ P1 ＡＢＣＤＥＦＧ 999 99:59`) × 2 + gutter.
  // HALFWIDTH: each rack tile takes 1 col — needs ~25 cols per pill
  // (`▶ P1 ABCDEFG 999 99:59`) × 2 + gutter. Below the halfwidth
  // threshold the history must drop to a single column.
  HISTORY_TWO_COL_THRESHOLD = 68,
  HISTORY_TWO_COL_HALFWIDTH_THRESHOLD = 52,
  // Analysis panel sizing.
  ANALYSIS_MIN_WIDTH = 30,    // narrower than this and we skip the panel
  ANALYSIS_DEFAULT_ROWS = 20, // height used in "below history" mode when
                              // history overflows
  ANALYSIS_GUTTER = 1,        // gap between history and right-side analysis
  // Right column has to hold a 2-col history AND an analysis panel
  // alongside it before we switch to the three-column layout. The
  // gutter sits between them.
  ANALYSIS_THREE_COL_THRESHOLD =
      HISTORY_TWO_COL_THRESHOLD + ANALYSIS_GUTTER + ANALYSIS_MIN_WIDTH,
  STATUS_BAR_HEIGHT = 1,
  MIN_ROWS_REQUIRED_1X = 24,
  // 1x layout: 33 cols for the board area + 1 gutter + 32 cols right column.
  // 2x bumps the board to 63 cols, so the floor rises to 96.
  MIN_COLS_REQUIRED_1X = CELL_COL_BASE + BOARD_DIM * CELL_WIDTH +
                         BOARD_RIGHT_BORDER + RIGHT_COL_LEFT_OFFSET +
                         RIGHT_COL_MIN_WIDTH,
};
typedef struct {
  unsigned plane_rows;
  unsigned plane_cols;

  int status_row;
  // Always-reserved command bar above the status bar. Hosts the [0]
  // focus indicator and (eventually) a "/ for cmd" Minecraft-style
  // prompt. Always present, so content panels lose at least one row
  // to it even when no pending changes are queued.
  int command_bar_row;
  // -1 when no pending settings; otherwise the row where the pending-
  // change banner renders (just above the command bar). When set,
  // panels shrink by another row so the banner has somewhere to live
  // without overlapping content.
  int pending_row;
  // Last row a content panel may render into. Use this everywhere the
  // old `status_row - 1` was used; it tracks whichever of the bottom
  // bars is highest.
  int content_bottom_row;

  // Scale-dependent board geometry. At scale=1: cell_w=2, cell_h=1, the
  // 33-col / 16-row classic layout. At scale=2: cell_w=4, cell_h=2 →
  // 63-col / 31-row board area driven by the FreeType composite path.
  int scale;
  int board_cell_w;
  int board_cell_h;
  int board_width; // total cols claimed by the board area (incl. row labels)
  int board_bottom_row; // last row occupied by the board's bottommost cells

  int board_right_col; // = board_width - 1
  int rack_top, rack_bottom;
  int bag_top, bag_bottom;

  int right_col_left, right_col_right;
  int right_col_width;
  bool two_col;
  // In two-col mode, true when the right column is narrow enough
  // that the player pills must render their racks as halfwidth
  // (1 col per tile) instead of fullwidth (2 cols).
  bool pills_halfwidth;
  // In two-col mode the pills + history sit inside one combined
  // box: pill content rows on top, a single horizontal divider, and
  // history rows below. Saves a row of pill bottom-border + history
  // top-border and visually ties the headers to their columns.
  // When true, render_player_pill and render_history_panel skip
  // their own box drawing — draw_combined_pills_history_frame draws
  // all borders, joints, and the vertical column divider.
  bool combined_pills_history;
  // Column at which the combined frame's vertical divider lives
  // (also the right edge of pill1's box and the left edge of pill2's
  // box). Only meaningful in two_col mode.
  int divider_col;

  // Pills. In two-col mode they sit side-by-side on the same row; in
  // one-col mode they stack vertically.
  int pill1_top, pill1_bottom, pill1_left, pill1_right;
  int pill2_top, pill2_bottom, pill2_left, pill2_right;

  int history_top, history_bottom;

  // Analysis panel. Placement varies with available space:
  //   ANALYSIS_RIGHT_OF_HISTORY — extra-wide terminals split the right
  //     column into history (left) + analysis (right), both spanning
  //     the full history vertical range.
  //   ANALYSIS_BELOW_HISTORY    — right column is wide enough for the
  //     two-column history layout. Analysis sits below the history
  //     box: if history fits without scrolling, analysis grabs all
  //     remaining vertical space; otherwise it takes a fixed 20 rows.
  //   ANALYSIS_BELOW_BAG        — narrow right column (history is
  //     single-column). Analysis lives in the left column under the
  //     bag; the bag itself is sized just tall enough for its content
  //     and the rest of the column goes to analysis.
  //   ANALYSIS_NONE             — there's no room.
  enum {
    ANALYSIS_NONE,
    ANALYSIS_BELOW_BAG,
    ANALYSIS_BELOW_HISTORY,
    ANALYSIS_RIGHT_OF_HISTORY,
  } analysis_placement;
  bool has_analysis;
  int analysis_top, analysis_bottom, analysis_left, analysis_right;
} Layout;

int compute_effective_scale(int user_pref, unsigned plane_cols,
                            unsigned plane_rows);
Layout compute_layout(struct ncplane *plane, int user_scale,
                      const TuiGameState *state);

#endif

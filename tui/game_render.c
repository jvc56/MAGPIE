#include "game_render.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/endgame.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "frame_dump.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "mach_compat.h"
#include "theme.h"
#include "time_picker.h"
#include "tui_resize.h"
#include <ctype.h>
#include <limits.h>
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Three tile scales:
//   0 = halfwidth (1 col × 1 row per cell, single ASCII glyph)
//   1 = fullwidth (2 cols × 1 row, fullwidth Unicode glyph)
//   2 = double    (4 cols × 2 rows, FreeType pixel composite)
// compute_effective_scale picks the largest scale ≤ user_pref that fits
// the current plane. Returns -1 when even halfwidth is too cramped.
// Forward-declared so the analysis-panel titles can use it; the body
// lives next to the other status-bar helpers further down.
static void format_count_compact(uint64_t n, char *buf, size_t bufsz);

static int compute_effective_scale(int user_pref, unsigned plane_cols,
                                   unsigned plane_rows) {
  static const int cell_w_for[3] = {1, 2, 4};
  static const int cell_h_for[3] = {1, 1, 2};
  if (user_pref < 0) {
    user_pref = 0;
  } else if (user_pref > 2) {
    user_pref = 2;
  }
  // Account for the chrome rows compute_layout reserves at the
  // bottom: command bar (always) + status bar (always). The pending
  // banner is ignored here — if it shows up later the panels just
  // get one row tighter, which is fine and visually unchanged.
  if (plane_rows >= 2) {
    plane_rows -= 1;
  }
  for (int s = user_pref; s >= 0; s--) {
    const int cols = CELL_COL_BASE + BOARD_DIM * cell_w_for[s] +
                     BOARD_RIGHT_BORDER + RIGHT_COL_LEFT_OFFSET +
                     RIGHT_COL_MIN_WIDTH;
    // Board widget = top border (1) + col-label row (1) + cells + bottom
    // border (1). Then rack box, bag min 3 rows, status 1. Rack sits
    // flush under the board's bottom border, no gap.
    const int rack_box_rows = (s == 2) ? 4 : 3;
    const int rows = BOARD_DIM * cell_h_for[s] + 3 + rack_box_rows + 3 + 1;
    if (plane_cols >= (unsigned)cols && plane_rows >= (unsigned)rows) {
      return s;
    }
  }
  return -1;
}

// Display-column count of the dense bag/unseen-tile string the bag
// panel renders. Mirrors render_bag_panel's content building; used by
// compute_layout to size the bag tightly when the analysis panel is
// taking its overflow.
static int bag_unseen_chars(const TuiGameState *state) {
  if (state == NULL || state->game == NULL) {
    return 0;
  }
  const Bag *bag = game_get_bag(state->game);
  const LetterDistribution *ld = state->ld;
  const int ld_size = ld_get_size(ld);
  int counts[64] = {0};
  for (int ml = 0; ml < ld_size && ml < (int)(sizeof(counts) / sizeof(int));
       ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  const int off_turn = 1 - game_get_player_on_turn_index(state->game);
  const Rack *off_rack =
      player_get_rack(game_get_player(state->game, off_turn));
  for (int ml = 0; ml < ld_size && ml < (int)(sizeof(counts) / sizeof(int));
       ml++) {
    counts[ml] += rack_get_letter(off_rack, (MachineLetter)ml);
  }
  // Each grouping is "<n copies of letter>"; groupings are separated
  // by a single space. Letters are ASCII (1 col each) for the lexica
  // we ship; non-ASCII would over-count visual width slightly but the
  // bag would just end up a row taller, which is fine.
  int total = 0;
  bool first = true;
  for (int ml = 0; ml < ld_size; ml++) {
    if (counts[ml] == 0) {
      continue;
    }
    if (!first) {
      total += 1; // separator space
    }
    first = false;
    total += counts[ml];
  }
  return total;
}

// Maximum row usage across the two history columns. Used to decide
// whether history fits without scrolling so we can give the surplus
// space to the analysis panel.
static int history_two_col_rows_needed(const TuiGameState *state) {
  if (state == NULL) {
    return 0;
  }
  int left = 0;
  int right = 0;
  for (int idx = 0; idx < state->history_count; idx++) {
    const int rows = state->history[idx].end_bonus != 0
                         ? 4
                         : 2; // mirrors history_entry_rows
    if (idx % 2 == 0) {
      left += rows;
    } else {
      right += rows;
    }
  }
  return left > right ? left : right;
}

// Forward declarations of the cursor-view pickers (defined later in
// the file alongside the rest of the cursor-aware accessors). Used
// by compute_layout below so the layout's bag_empty heuristic
// follows whatever board/rack the user is currently viewing
// instead of the live game state — otherwise scrolling back to
// a mid-game turn in a finished game leaves the bag panel
// collapsed to a divider even though the cursor view has plenty
// of tiles to show.
static const struct Board *pick_render_board(const TuiGameState *state);
static const TuiHistoryEntry *pick_history_view(const TuiGameState *state);

// True when the bag the user is currently looking at is empty.
// Mirrors render_bag_panel's formula (ld_total − on_board −
// on_turn_rack) so the layout's bag-collapse decision matches
// what the renderer will actually paint.
static bool cursor_view_bag_empty(const TuiGameState *state) {
  if (state == NULL || state->ld == NULL) {
    return true;
  }
  const Board *render_board = pick_render_board(state);
  const TuiHistoryEntry *hview = pick_history_view(state);
  const Rack *on_turn_rack = NULL;
  if (hview != NULL && hview->rack_before != NULL) {
    on_turn_rack = hview->rack_before;
  } else if (state->game != NULL) {
    on_turn_rack = player_get_rack(game_get_player(
        state->game, game_get_player_on_turn_index(state->game)));
  }
  int board_counts[64] = {0};
  if (render_board != NULL) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        MachineLetter ml = board_get_letter(render_board, row, col);
        if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
          continue;
        }
        if (ml & BLANK_MASK) {
          ml = 0;
        }
        if (ml < (int)(sizeof(board_counts) / sizeof(int))) {
          board_counts[ml]++;
        }
      }
    }
  }
  const int ld_size = ld_get_size(state->ld);
  for (int ml = 0;
       ml < ld_size && ml < (int)(sizeof(board_counts) / sizeof(int)); ml++) {
    const int dist = ld_get_dist(state->ld, (MachineLetter)ml);
    const int on_rack = on_turn_rack != NULL
                            ? rack_get_letter(on_turn_rack, (MachineLetter)ml)
                            : 0;
    int v = dist - board_counts[ml] - on_rack;
    if (v < 0) {
      v = 0;
    }
    if (v > 0) {
      return false;
    }
  }
  return true;
}

static Layout compute_layout(struct ncplane *plane, int user_scale,
                             const TuiGameState *state) {
  Layout L = {0};
  ncplane_dim_yx(plane, &L.plane_rows, &L.plane_cols);

  L.status_row = (int)L.plane_rows - 1;
  L.command_bar_row = L.status_row - 1;
  // Pending-change banner: rendered above the command bar when the
  // user's Settings have outpaced the live game's loaded tables.
  // Content panels treat content_bottom_row as their lowest available
  // row regardless of which path is active.
  const bool pending_changes =
      state != NULL &&
      (strcmp(state->pending_lexicon, state->active_lexicon) != 0 ||
       state->pending_load_rit != state->active_load_rit);
  L.pending_row = pending_changes ? L.command_bar_row - 1 : -1;
  L.content_bottom_row =
      (L.pending_row >= 0 ? L.pending_row : L.command_bar_row) - 1;

  L.scale = compute_effective_scale(user_scale, L.plane_cols, L.plane_rows);
  if (L.scale < 0) {
    // Caller checks L.scale < 0 and renders the too-small message;
    // the rest of the fields stay zeroed and unused.
    return L;
  }
  L.board_cell_w = (L.scale == 2) ? 4 : (L.scale == 1) ? 2 : 1;
  L.board_cell_h = (L.scale == 2) ? 2 : 1;
  // board_width spans cols [0, board_width-1], with col 0 the left
  // border and col board_width-1 the right border.
  L.board_width =
      CELL_COL_BASE + BOARD_DIM * L.board_cell_w + BOARD_RIGHT_BORDER;
  L.board_bottom_row = CELL_ROW_BASE + BOARD_DIM * L.board_cell_h - 1;

  L.board_right_col = L.board_width - 1;
  // Skip the bottom-border row of the board widget; rack starts on the
  // row after that.
  L.rack_top = L.board_bottom_row + 2;
  // Rack box gains a row when the board is double-size so the rack
  // tiles (also rendered at 4×2 cells) get the vertical room they need.
  const int rack_box_rows = (L.scale == 2) ? 4 : 3;
  L.rack_bottom = L.rack_top + rack_box_rows - 1;

  L.right_col_left = L.board_width + RIGHT_COL_LEFT_OFFSET;
  L.right_col_right = (int)L.plane_cols - 1;
  L.right_col_width = L.right_col_right - L.right_col_left + 1;

  // Decide where the analysis panel lives based on right-column width.
  // Two-col history is allowed as long as halfwidth pills fit; the
  // analysis-three-col split still gates on the fullwidth threshold
  // because the leftover slice still has to be wide enough to host a
  // useful analysis panel.
  L.analysis_placement = ANALYSIS_NONE;
  if (state != NULL) {
    if (L.right_col_width >= ANALYSIS_THREE_COL_THRESHOLD) {
      L.analysis_placement = ANALYSIS_RIGHT_OF_HISTORY;
    } else if (L.right_col_width >= HISTORY_TWO_COL_HALFWIDTH_THRESHOLD) {
      L.analysis_placement = ANALYSIS_BELOW_HISTORY;
    } else {
      L.analysis_placement = ANALYSIS_BELOW_BAG;
    }
  }

  // Three-column case: peel the analysis strip off the right of the
  // right column BEFORE computing pill geometry. Instead of giving
  // history a fixed slab and letting analysis have whatever's left,
  // size history just wide enough for its actual content (longest
  // history entry's move row) — wider analysis is much more useful
  // than wider history.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    // Minimum interior per history column: enough for a halfwidth
    // pill ("▶ P1 ABCDEFG 999 99:59" ≈ 22 cols). History entry
    // content (move row: "N. <move> +score") sometimes runs wider —
    // we walk the history and take the max so column-widths grow
    // (never shrink below the pill minimum) to fit the data.
    int col_interior = 22;
    if (state != NULL) {
      for (int idx = 0; idx < state->history_count; idx++) {
        const TuiHistoryEntry *e = &state->history[idx];
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%d. ", idx + 1);
        char delta[16];
        snprintf(delta, sizeof(delta), "+%d", e->score);
        // Row 1 width: "N. <move>" + 1-col gap + "+<score>".
        const int row1 = (int)strlen(prefix) + (int)strlen(e->move_str) + 1 +
                         (int)strlen(delta);
        if (row1 > col_interior) {
          col_interior = row1;
        }
        // Row 2 width: indent (== prefix len) + clock(4-5) + space +
        // rack(up to 7) + 1 gap + running total.
        char total[16];
        snprintf(total, sizeof(total), "%d", e->total_after);
        const int rack_len = e->rack_str[0] ? (int)strlen(e->rack_str) : 1;
        const int clock_len = 5; // worst case "99:59"
        const int row2 = (int)strlen(prefix) + clock_len + 1 + rack_len + 1 +
                         (int)strlen(total);
        if (row2 > col_interior) {
          col_interior = row2;
        }
      }
    }
    // History panel total: 2 columns + 1-col divider + 2 outer borders.
    const int hist_w = 2 * col_interior + 1 + 2;
    // Cap so analysis still gets at least ANALYSIS_MIN_WIDTH.
    const int max_hist =
        L.right_col_width - ANALYSIS_GUTTER - ANALYSIS_MIN_WIDTH;
    const int actual_hist = hist_w > max_hist ? max_hist
                            : hist_w < HISTORY_TWO_COL_HALFWIDTH_THRESHOLD
                                ? HISTORY_TWO_COL_HALFWIDTH_THRESHOLD
                                : hist_w;
    const int analysis_width =
        L.right_col_width - actual_hist - ANALYSIS_GUTTER;
    L.analysis_right = L.right_col_right;
    L.analysis_left = L.right_col_right - analysis_width + 1;
    L.right_col_right = L.analysis_left - 1 - ANALYSIS_GUTTER;
    L.right_col_width = L.right_col_right - L.right_col_left + 1;
  }

  L.two_col = L.right_col_width >= HISTORY_TWO_COL_HALFWIDTH_THRESHOLD;
  // Even at a width that nominally fits two columns, a single
  // entry's move + leave + delta combo can be longer than the
  // half-column interior. When that happens the move text runs
  // into the leave column and produces visible character overlap.
  // Downgrade to single-column mode in that case — each row gets
  // ~2× the width and the overlap clears. Zero-gap collisions are
  // fine (color styling differentiates them); we only flip when a
  // truly negative gap would be needed.
  if (L.two_col && state != NULL && state->history_count > 0) {
    const int gutter = 1;
    const int half_width = (L.right_col_width - 2 - gutter) / 2;
    const int per_col_interior_w = half_width - 2; // minus side borders
    int rank_digits = 1;
    for (int n = state->history_count; n >= 10; n /= 10) {
      rank_digits++;
    }
    const int prefix_w = rank_digits + 2; // digits + ". "
    // 4 cells reserved on the right for the delta column (a 3-char
    // "+NN" + a 1-char gap). Same reservation render_history_entry
    // uses when placing the leave.
    const int delta_room = 4;
    for (int i = 0; i < state->history_count; i++) {
      const TuiHistoryEntry *e = &state->history[i];
      if (e->pending) {
        continue;
      }
      int move_w = 0;
      for (const char *p = e->move_str; *p != '\0'; p++) {
        if (*p != '(' && *p != ')') {
          move_w++;
        }
      }
      const int leave_w =
          e->leave_str[0] != '\0' ? (int)strlen(e->leave_str) : 1;
      if (move_w + leave_w + delta_room + prefix_w > per_col_interior_w) {
        L.two_col = false;
        break;
      }
    }
  }
  L.pills_halfwidth =
      L.two_col && L.right_col_width < HISTORY_TWO_COL_THRESHOLD;

  if (L.two_col) {
    // Pills + history share one combined box. The vertical divider
    // between the two columns runs through both the pill row and
    // the history rows; the divider row between pill content and
    // history is the pills' bottom border (├─┼─┤). Geometry:
    //   row 0:   ┌─...─┬─...─┐   (combined top border)
    //   row 1:   │pill1 │pill2│   (pill content)
    //   row 2:   ├─...─┼─...─┤   (horizontal divider == pill bottom)
    //   rows 3+: │hist1 │hist2│
    //   bot:     └─...─┴─...─┘
    const int gutter = 1; // box-vertical, drawn by the combined frame
    const int half_width = (L.right_col_width - 2 - gutter) / 2;
    L.combined_pills_history = true;
    L.divider_col = L.right_col_left + 1 + half_width;
    L.pill1_top = 0;
    L.pill1_bottom = PILL_HEIGHT - 1; // == row 2, the divider row
    L.pill1_left = L.right_col_left;
    L.pill1_right = L.divider_col;
    L.pill2_top = L.pill1_top;
    L.pill2_bottom = L.pill1_bottom;
    L.pill2_left = L.divider_col;
    L.pill2_right = L.right_col_right;
    L.history_top = L.pill1_bottom; // shared divider row
  } else {
    L.pill1_top = 0;
    L.pill1_bottom = PILL_HEIGHT - 1;
    L.pill1_left = L.right_col_left;
    L.pill1_right = L.right_col_right;
    L.pill2_top = L.pill1_bottom + 1;
    L.pill2_bottom = L.pill2_top + PILL_HEIGHT - 1;
    L.pill2_left = L.right_col_left;
    L.pill2_right = L.right_col_right;
    L.history_top = L.pill2_bottom + 1;
  }
  L.history_bottom = L.content_bottom_row;

  // Default bag region: rack_bottom+1 to content_bottom_row. Tightened
  // below in the BELOW_BAG case so analysis can sit beneath the bag,
  // and also when the bag is fully empty — in that case
  // render_bag_panel collapses to a single divider line and we
  // shouldn't waste rows on a blank box.
  L.bag_top = L.rack_bottom + 1;
  L.bag_bottom = L.content_bottom_row;
  // Follow the cursor view so scrolling back to a mid-game turn
  // in a finished game restores the bag panel.
  const bool bag_empty = cursor_view_bag_empty(state);
  if (bag_empty) {
    L.bag_bottom = L.bag_top; // single-row divider
  }

  // Three-column: analysis fills the right strip from the very top of
  // the plane down to the status bar. There's nothing else over there,
  // so there's no reason to align with the pills row.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    L.analysis_top = 0;
    L.analysis_bottom = L.content_bottom_row;
    L.has_analysis = (L.analysis_bottom - L.analysis_top + 1) >= 3;
  }

  // Two-column: carve rows off the bottom of history for analysis.
  // If history fits without scrolling, hand all the surplus to
  // analysis; otherwise reserve a fixed slab.
  if (L.analysis_placement == ANALYSIS_BELOW_HISTORY) {
    const int interior_rows = L.history_bottom - L.history_top - 1;
    const int needed = history_two_col_rows_needed(state);
    int analysis_rows;
    if (needed <= interior_rows) {
      analysis_rows = interior_rows - needed;
    } else {
      analysis_rows = ANALYSIS_DEFAULT_ROWS;
    }
    if (analysis_rows >= 3 &&
        L.history_bottom - analysis_rows > L.history_top) {
      L.analysis_top = L.history_bottom - analysis_rows + 1;
      L.analysis_bottom = L.history_bottom;
      L.analysis_left = L.right_col_left;
      L.analysis_right = L.right_col_right;
      L.history_bottom = L.analysis_top - 1;
      L.has_analysis = true;
    }
  }

  // Single-column history: analysis sits below the bag in the LEFT
  // column. Bag shrinks to its content height (plus borders + tally),
  // and analysis claims everything else above the status bar.
  if (L.analysis_placement == ANALYSIS_BELOW_BAG) {
    int bag_height;
    if (bag_empty) {
      // The empty-bag divider only needs one row.
      bag_height = 1;
      L.bag_bottom = L.bag_top;
    } else {
      const int interior_width = L.board_width - 2;
      const int chars = bag_unseen_chars(state);
      int content_lines = interior_width > 0
                              ? (chars + interior_width - 1) / interior_width
                              : 1;
      if (content_lines < 1) {
        content_lines = 1;
      }
      // 2 borders + content rows + 1 tally row.
      bag_height = 2 + content_lines + 1;
    }
    // Floor 1 instead of 0 in case the math collapses; we still need
    // at least the bag's own row before the analysis starts.
    const int total = L.content_bottom_row - L.bag_top + 1;
    const int analysis_floor = 3 + 1;
    if (bag_height > total - analysis_floor) {
      bag_height = total - analysis_floor;
    }
    if (bag_height < 1) {
      bag_height = 1;
    }
    L.bag_bottom = L.bag_top + bag_height - 1;
    const int analysis_top = L.bag_bottom + 1;
    if (analysis_top <= L.content_bottom_row &&
        L.content_bottom_row - analysis_top + 1 >= 3) {
      L.analysis_top = analysis_top;
      L.analysis_bottom = L.content_bottom_row;
      L.analysis_left = 0;
      L.analysis_right = L.board_width - 1;
      L.has_analysis = true;
    }
  }

  return L;
}

// ── Box drawing chars ─────────────────────────────────────────────────────
#define BOX_TL "\xe2\x94\x8c" // ┌
#define BOX_TR "\xe2\x94\x90" // ┐
#define BOX_BL "\xe2\x94\x94" // └
#define BOX_BR "\xe2\x94\x98" // ┘
#define BOX_HZ "\xe2\x94\x80" // ─
#define BOX_VT "\xe2\x94\x82" // │
// Double-line variants for the focused-panel border.
#define BOX2_TL "\xe2\x95\x94"     // ╔
#define BOX2_TR "\xe2\x95\x97"     // ╗
#define BOX2_BL "\xe2\x95\x9a"     // ╚
#define BOX2_BR "\xe2\x95\x9d"     // ╝
#define BOX2_HZ "\xe2\x95\x90"     // ═
#define BOX2_VT "\xe2\x95\x91"     // ║
#define BOX_T_DOWN "\xe2\x94\xac"  // ┬
#define BOX_T_UP "\xe2\x94\xb4"    // ┴
#define BOX_T_RIGHT "\xe2\x94\x9c" // ├
#define BOX_T_LEFT "\xe2\x94\xa4"  // ┤
#define BOX_CROSS "\xe2\x94\xbc"   // ┼

// Fullwidth column labels Ａ..Ｚ.
static const char *const fullwidth_col_labels[] = {
    "\xef\xbc\xa1", "\xef\xbc\xa2", "\xef\xbc\xa3", "\xef\xbc\xa4",
    "\xef\xbc\xa5", "\xef\xbc\xa6", "\xef\xbc\xa7", "\xef\xbc\xa8",
    "\xef\xbc\xa9", "\xef\xbc\xaa", "\xef\xbc\xab", "\xef\xbc\xac",
    "\xef\xbc\xad", "\xef\xbc\xae", "\xef\xbc\xaf", "\xef\xbc\xb0",
    "\xef\xbc\xb1", "\xef\xbc\xb2", "\xef\xbc\xb3", "\xef\xbc\xb4",
    "\xef\xbc\xb5", "\xef\xbc\xb6", "\xef\xbc\xb7", "\xef\xbc\xb8",
    "\xef\xbc\xb9", "\xef\xbc\xba",
};

typedef struct {
  const char *glyph; // exactly 2 terminal columns wide
  ThemeRgb fg;
  ThemeRgb bg;
} PremiumMarker;

// Empty / non-premium cells render an ideographic space so the cell still
// claims its full 2 columns of board_bg. Premium cells use either 2-char
// ASCII labels ("TW"/"tw"/etc.), or that same ideographic space when the
// user asked for color-only premiums.
#define PREMIUM_EMPTY_GLYPH "\xe3\x80\x80"

static const char *premium_glyph(const char *upper, const char *lower,
                                 const char *punct, TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return lower;
  case TUI_PREMIUM_LABELS_PUNCT:
    return punct;
  case TUI_PREMIUM_LABELS_NONE:
    return PREMIUM_EMPTY_GLYPH;
  case TUI_PREMIUM_LABELS_UPPERCASE:
  case TUI_PREMIUM_LABELS_COUNT:
  default:
    return upper;
  }
}

// Halfwidth premium markers collapse 2-char labels (TW / DW / TL / DL)
// to single punctuation glyphs that fit in one terminal column. The
// labels=NONE setting still suppresses them in favor of color-only.
static const char *halfwidth_premium_glyph(const char *punct,
                                           TuiPremiumLabels labels) {
  return labels == TUI_PREMIUM_LABELS_NONE ? " " : punct;
}

static PremiumMarker premium_marker_for_cell(const Theme *theme, BonusSquare bs,
                                             int row, int col,
                                             TuiPremiumLabels labels,
                                             int cell_w) {
  const bool halfwidth = (cell_w == 1);
  const int center = BOARD_DIM / 2;
  if (row == center && col == center) {
    // The center is mechanically a DW; macondo paints it with the DW tint
    // and no separate symbol, so we follow suit — the tint identifies it.
    // Fullwidth punct mode: a ＊ asterisk marks the centre, matching the
    // basic string_builder_add_game printer's convention.
    const char *glyph =
        halfwidth ? halfwidth_premium_glyph("*", labels)
                  : premium_glyph(PREMIUM_EMPTY_GLYPH, PREMIUM_EMPTY_GLYPH,
                                  "\xef\xbc\x8a", labels); // ＊ U+FF0A
    return (PremiumMarker){glyph, theme->premium_center_fg,
                           theme->premium_center_bg};
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bs);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bs);
  if (word_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("=", labels)
                                  : premium_glyph("TW", "tw", "\xef\xbc\x9d",
                                                  labels); // ＝ U+FF1D
    return (PremiumMarker){glyph, theme->premium_tws_fg, theme->premium_tws_bg};
  }
  if (word_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("-", labels)
                                  : premium_glyph("DW", "dw", "\xef\xbc\x8d",
                                                  labels); // － U+FF0D
    return (PremiumMarker){glyph, theme->premium_dws_fg, theme->premium_dws_bg};
  }
  if (letter_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("\"", labels)
                                  : premium_glyph("TL", "tl", "\xef\xbc\x82",
                                                  labels); // ＂ U+FF02
    return (PremiumMarker){glyph, theme->premium_tls_fg, theme->premium_tls_bg};
  }
  if (letter_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("'", labels)
                                  : premium_glyph("DL", "dl", "\xef\xbc\x87",
                                                  labels); // ＇ U+FF07
    return (PremiumMarker){glyph, theme->premium_dls_fg, theme->premium_dls_bg};
  }
  return (PremiumMarker){halfwidth ? " " : PREMIUM_EMPTY_GLYPH, theme->dim_fg,
                         theme->board_bg};
}

// ── Lexicon → language label ──────────────────────────────────────────────
// Mirrors the prefix table in tui/lexicon_picker.c. Keep in sync if a new
// lexicon prefix is added. Note the engine's has_iprefix signature:
// `has_iprefix(prefix, str)` — prefix first.
static const char *language_for_lexicon(const char *name) {
  if (name == NULL) {
    return "Unknown";
  }
  if (has_iprefix("CSW", name) || has_iprefix("NWL", name) ||
      has_iprefix("OSPD", name) || has_iprefix("OSW", name) ||
      has_iprefix("TWL", name) || has_iprefix("America", name) ||
      has_iprefix("CEL", name)) {
    return "English";
  }
  if (has_iprefix("FRA", name)) {
    return "French";
  }
  if (has_iprefix("OSPS", name)) {
    return "Polish";
  }
  if (has_iprefix("DISC", name)) {
    return "Catalan";
  }
  if (has_iprefix("DSW", name)) {
    return "Dutch";
  }
  if (has_iprefix("NSF", name)) {
    return "Norwegian";
  }
  if (has_iprefix("RD", name)) {
    return "German";
  }
  return "Other";
}

// ── Generic helpers ───────────────────────────────────────────────────────
// Render a panel border. When `focused` is true the box uses double-
// line glyphs and the border row/col cells paint with
// theme->panel_focus_border_bg so the frame lifts off the void. The
// `hotkey` digit (1..5; 0 = none) renders as "[N] " just before the
// title in the top border, dim-grey when unfocused and bold + bright
// when focused. The title itself is drawn in theme->fg on whichever
// bg the border row is using.
// Three-state badge rendering for panels with internal sub-focus:
//   - unfocused:                 dim "[N]" on theme->bg
//   - focused + badge primary:   inverted "[N>" chip (bg-on-fg, bold)
//   - focused + badge secondary: bright "[N]" on the focused border
//     bg (bold) — visible as the focused panel without claiming the
//     chevron-cursor that's drawn on whichever entry/row the user
//     is currently navigating.
// `badge_secondary` is ignored when `focused` is false.
static void draw_box_styled_ex(struct ncplane *plane, const Theme *theme,
                               int top_row, int left_col, int height, int width,
                               const char *title, int hotkey, bool focused,
                               bool badge_secondary);

static void draw_box_styled(struct ncplane *plane, const Theme *theme,
                            int top_row, int left_col, int height, int width,
                            const char *title, int hotkey, bool focused) {
  draw_box_styled_ex(plane, theme, top_row, left_col, height, width, title,
                     hotkey, focused, /*badge_secondary=*/false);
}

static void draw_box_styled_ex(struct ncplane *plane, const Theme *theme,
                               int top_row, int left_col, int height, int width,
                               const char *title, int hotkey, bool focused,
                               bool badge_secondary) {
  const ThemeRgb border_fg = focused ? theme->fg : theme->dim_fg;
  const ThemeRgb border_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  const char *tl = focused ? BOX2_TL : BOX_TL;
  const char *tr = focused ? BOX2_TR : BOX_TR;
  const char *bl = focused ? BOX2_BL : BOX_BL;
  const char *br = focused ? BOX2_BR : BOX_BR;
  const char *hz = focused ? BOX2_HZ : BOX_HZ;
  const char *vt = focused ? BOX2_VT : BOX_VT;
  theme_apply_fg(plane, border_fg);
  theme_apply_bg(plane, border_bg);
  const int right_col = left_col + width - 1;
  const int bottom_row = top_row + height - 1;

  ncplane_putstr_yx(plane, top_row, left_col, tl);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, top_row, col, hz);
  }
  ncplane_putstr_yx(plane, top_row, right_col, tr);

  for (int row = top_row + 1; row < bottom_row; row++) {
    ncplane_putstr_yx(plane, row, left_col, vt);
    ncplane_putstr_yx(plane, row, right_col, vt);
  }

  ncplane_putstr_yx(plane, bottom_row, left_col, bl);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, bottom_row, col, hz);
  }
  ncplane_putstr_yx(plane, bottom_row, right_col, br);

  if (title != NULL && title[0] != '\0') {
    // Title block flush against the top-left corner (one cell in
    // from the corner glyph). The previous two-cell inset wasted
    // a column on every panel; with N panels stacked, that adds up.
    int col = left_col + 1;
    // [N] hotkey indicator. Dim-grey when unfocused; bold + bright
    // when focused. Always renders if hotkey > 0.
    if (hotkey > 0) {
      char buf[8];
      // Three badge states:
      //  - focused + badge primary: inverted "[N>" chip — chevron
      //    indicates "this is where arrow keys move from".
      //  - focused + badge secondary: bright "[N]" on the focused
      //    border bg (bold, no chevron). The user has navigated
      //    deeper into the panel; the badge stays prominent so
      //    you can see WHICH panel is focused, but the chevron
      //    moves with the cursor onto a sub-element row.
      //  - unfocused: dim "[N]" hint.
      if (focused && !badge_secondary) {
        snprintf(buf, sizeof(buf), "[%d>", hotkey);
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->fg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      } else if (focused) {
        snprintf(buf, sizeof(buf), "[%d]", hotkey);
        theme_apply_fg(plane, theme->fg);
        theme_apply_bg(plane, border_bg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      } else {
        snprintf(buf, sizeof(buf), "[%d]", hotkey);
        theme_apply_fg(plane, theme->modal_shortcut_fg);
        theme_apply_bg(plane, border_bg);
      }
      ncplane_putstr_yx(plane, top_row, col, buf);
      col += (int)strlen(buf);
      ncplane_set_styles(plane, 0);
      // Space between badge and title.
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, border_bg);
      ncplane_putstr_yx(plane, top_row, col++, " ");
    }
    // Title text.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, border_bg);
    ncplane_putstr_yx(plane, top_row, col, title);
    col += (int)strlen(title);
    // Trailing pad.
    ncplane_putstr_yx(plane, top_row, col, " ");
  }
}

// Back-compat wrapper for callers that don't care about focus/hotkey.
static void draw_box(struct ncplane *plane, const Theme *theme, int top_row,
                     int left_col, int height, int width, const char *title) {
  draw_box_styled(plane, theme, top_row, left_col, height, width, title,
                  /*hotkey=*/0, /*focused=*/false);
}

// Draws the outer borders + horizontal divider + vertical column
// divider for the combined pills+history box in two-col mode. Pill
// and history content rendering skip their own draw_box calls when
// this fires; this function paints the full frame with proper T and
// cross junctions.
// The combined pills+history frame treats both pills and the history
// rows as ONE logical component (player columns headering their move
// histories), with a single shared box. The history title and its
// [4] hotkey live in the top border, top-left — flush against the
// corner, so it reads as the component name of the whole assembly.
// When focused (history hotkey active), the entire outer frame plus
// the cross-bar divider switch to double-line glyphs on the lighter
// panel_focus_border_bg.
static void draw_combined_pills_history_frame(struct ncplane *plane,
                                              const Theme *theme,
                                              const TuiGameState *state,
                                              const Layout *L, bool focused) {
  const ThemeRgb border_fg = focused ? theme->fg : theme->dim_fg;
  const ThemeRgb border_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  const char *tl = focused ? BOX2_TL : BOX_TL;
  const char *tr = focused ? BOX2_TR : BOX_TR;
  const char *bl = focused ? BOX2_BL : BOX_BL;
  const char *br = focused ? BOX2_BR : BOX_BR;
  const char *hz = focused ? BOX2_HZ : BOX_HZ;
  const char *vt = focused ? BOX2_VT : BOX_VT;
  // No double-line equivalents for the T-junctions here — keep them
  // single-line for now; the visual hit is small and matches the
  // single-line glyphs the pill cell rendering puts inside.
  theme_apply_fg(plane, border_fg);
  theme_apply_bg(plane, border_bg);
  const int top = L->pill1_top;
  const int divider_row = L->pill1_bottom;
  const int bottom = L->history_bottom;
  const int left = L->right_col_left;
  const int right = L->right_col_right;
  const int mid = L->divider_col;

  // Top border with title in the top-left.
  ncplane_putstr_yx(plane, top, left, tl);
  for (int col = left + 1; col < right; col++) {
    ncplane_putstr_yx(plane, top, col, col == mid ? BOX_T_DOWN : hz);
  }
  ncplane_putstr_yx(plane, top, right, tr);

  // Pill content row (row top+1) is filled in by render_player_pill;
  // we only need to paint the column borders here.
  for (int row = top + 1; row < divider_row; row++) {
    ncplane_putstr_yx(plane, row, left, vt);
    ncplane_putstr_yx(plane, row, mid, BOX_VT);
    ncplane_putstr_yx(plane, row, right, vt);
  }

  // Horizontal divider: ├──────┼──────┤ (single inside, double-into-
  // outer-frame junctions when focused via ╟ / ╢ so the outer ║
  // visually continues through the divider row).
  const char *div_left = focused ? "\xe2\x95\x9f" /* ╟ */ : BOX_T_RIGHT;
  const char *div_right = focused ? "\xe2\x95\xa2" /* ╢ */ : BOX_T_LEFT;
  ncplane_putstr_yx(plane, divider_row, left, div_left);
  for (int col = left + 1; col < right; col++) {
    ncplane_putstr_yx(plane, divider_row, col, col == mid ? BOX_CROSS : BOX_HZ);
  }
  ncplane_putstr_yx(plane, divider_row, right, div_right);

  // History content rows.
  for (int row = divider_row + 1; row < bottom; row++) {
    ncplane_putstr_yx(plane, row, left, vt);
    ncplane_putstr_yx(plane, row, mid, BOX_VT);
    ncplane_putstr_yx(plane, row, right, vt);
  }

  // Bottom border.
  ncplane_putstr_yx(plane, bottom, left, bl);
  for (int col = left + 1; col < right; col++) {
    ncplane_putstr_yx(plane, bottom, col, col == mid ? BOX_T_UP : hz);
  }
  ncplane_putstr_yx(plane, bottom, right, br);

  // [4] History title on the DIVIDER row (between pills and history)
  // rather than the top border — there's nothing controllable in
  // the pills row, and putting the title above the first history
  // entry matches what "this is the history panel" should refer
  // to. Flush against the left edge (col = left + 1) so the
  // indicator aligns with the "1." rank-prefix column below.
  //
  // Focused panels render the badge as the inverted "[4>" chip
  // (grey-on-grey, bold) to match the focus marker every other
  // panel uses — UNLESS the in-panel history cursor has moved off
  // the label (history_cursor >= 0). In that case the cursor is
  // visible on an individual entry and the badge dims to the
  // unfocused "[4]" hint so the user only sees one selection
  // marker at a time.
  {
    int col = left + 1;
    // Three-state badge identical to draw_box_styled_ex's logic:
    //   focused + cursor on label  → "[4>" inverted chip
    //   focused + cursor on entry  → "[4]" bold on focused bg
    //   unfocused                  → "[4]" dim hint
    const bool badge_primary =
        focused && (state == NULL || state->history_cursor == -1);
    if (badge_primary) {
      theme_apply_fg(plane, theme->bg);
      theme_apply_bg(plane, theme->fg);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, divider_row, col, "[4>");
    } else if (focused) {
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, border_bg);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, divider_row, col, "[4]");
    } else {
      theme_apply_fg(plane, theme->modal_shortcut_fg);
      theme_apply_bg(plane, border_bg);
      ncplane_putstr_yx(plane, divider_row, col, "[4]");
    }
    col += 3;
    ncplane_set_styles(plane, 0);
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, border_bg);
    ncplane_putstr_yx(plane, divider_row, col++, " ");
    ncplane_putstr_yx(plane, divider_row, col, "History");
    col += 7;
    ncplane_putstr_yx(plane, divider_row, col, " ");
  }
}

static void format_clock(int seconds, char *buf, size_t buf_size) {
  if (seconds < 0) {
    snprintf(buf, buf_size, "--:--");
    return;
  }
  const int minutes = seconds / 60;
  const int secs = seconds % 60;
  snprintf(buf, buf_size, "%d:%02d", minutes, secs);
}

// ── Pixel-graphics grid overlay ───────────────────────────────────────────
//
// On terminals that support pixel graphics (Kitty graphics protocol or
// Sixel — ghostty/iTerm2/kitty/foot/modern xterm), draw an RGBA bitmap
// of N-pixel borders in theme->bg over a child plane positioned at a
// region of cells. Pixels with alpha=0 composite through to the std plane,
// so the cells' glyph content stays readable underneath. On terminals
// without pixel support, the calls are no-ops via notcurses_canpixel.

// All grid planes live in one module-level registry so a single
// invalidation call (on resize or after the onboarding picker closes)
// can destroy them and the next render rebuilds fresh. Without this,
// font-size changes leave the previous pixel image at its old cell
// offset and the terminal can show ghost lines smearing into nearby
// rows — most visibly cutting the player-pill box borders.
static struct {
  // 2x board pixel composite.
  struct ncplane *board;
  // 2x board row + column coordinate labels. Drawn as pixels so a
  // 1-cell-tall glyph can be centered against the 2-cell-tall board
  // rows (which is impossible with text mode).
  struct ncplane *labels_col;
  struct ncplane *labels_row;
  // 2x rack pixel composite. Tiles in the rack scale alongside the
  // board so they don't read as tiny next to a giant board.
  struct ncplane *rack;
  // Modal box renders to its own child plane so it sits above the 2x
  // pixel composite. A dedicated top-most modal plane keeps both the
  // board and the menu visible at once.
  struct ncplane *modal;
} grid_planes;

// Debug instrumentation. Three counters surfaced in the top-right
// overlay while we tune the pixel-worker pipeline:
//   lat — end-to-end microseconds from a History-cursor change to
//         the corresponding pixel-board blit landing on screen.
//   blit — microseconds the UI thread last spent inside
//          ncblit_rgba (memcpy + plane-state churn). Excludes the
//          notcurses_render Kitty emit that happens later.
//   max — peak UI-thread frame time over the last ~1 second
//         (microseconds between consecutive notcurses_render
//         returns observed from main.c). Lets us see if the loop
//         is dropping below 60fps even when no cursor moved.
static _Atomic long g_board_blit_latency_us;
static _Atomic long g_ncblit_us;
static _Atomic long g_max_frame_us;
// notcurses sprixel emission counters. The renderer queries
// these once per frame to compare against the last snapshot; the
// delta is how many sprixels notcurses actually pushed to the
// terminal that frame, vs how many it elided (placed without
// re-uploading) because the content was unchanged. High emit
// counts on idle frames mean the elision logic isn't catching
// our planes.
static _Atomic uint64_t g_sprixel_emits_delta;
static _Atomic uint64_t g_sprixel_elides_delta;
// Number of tile pixel planes ncblit'd on the most-recent frame
// that performed any board work. 0 means no blits (cache hit).
// High values (5–7) line up with bingos / multi-letter plays
// landing on the board.
static _Atomic int g_last_tile_blits;
static int g_last_blitted_cursor;
static bool g_last_blit_tracked;
static struct timespec g_cursor_pending_since;
static bool g_cursor_pending;

// Snapshot the notcurses sprixel counters and stash the per-frame
// delta into the atomics that the debug overlay reads. Called
// from main.c right after each notcurses_render(). Lets us see
// whether unchanged sprixels are being elided (placed via short
// re-positioning commands) or fully re-emitted (full RGBA push).
void tui_debug_record_sprixel_stats(uint64_t emits, uint64_t elides) {
  static uint64_t last_emits;
  static uint64_t last_elides;
  static bool init;
  if (!init) {
    last_emits = emits;
    last_elides = elides;
    init = true;
    return;
  }
  const uint64_t de = (emits >= last_emits) ? emits - last_emits : 0;
  const uint64_t dl = (elides >= last_elides) ? elides - last_elides : 0;
  atomic_store(&g_sprixel_emits_delta, de);
  atomic_store(&g_sprixel_elides_delta, dl);
  last_emits = emits;
  last_elides = elides;
}

void tui_debug_record_frame_us(long frame_us) {
  static long max_in_window;
  static struct timespec window_start;
  static bool window_init;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!window_init) {
    window_init = true;
    window_start = now;
    max_in_window = 0;
  }
  if (frame_us > max_in_window) {
    max_in_window = frame_us;
  }
  const long since_start_ms =
      (long)(now.tv_sec - window_start.tv_sec) * 1000L +
      (long)(now.tv_nsec - window_start.tv_nsec) / 1000000L;
  if (since_start_ms >= 1000) {
    atomic_store(&g_max_frame_us, max_in_window);
    max_in_window = 0;
    window_start = now;
  }
}

// Cache for the 2x board pixel composite. ncblit_rgba is the FPS
// bottleneck even when the buffer hasn't changed; tracking a signature
// lets us skip the work and rely on notcurses keeping the plane's
// previous pixel content.
typedef struct {
  uint64_t version;     // game_state.render_version at last blit
  unsigned cdy, cdx;    // notcurses cell-pixel dims at last blit
  int param_a, param_b; // scale, antialias
  // History-cursor index at last blit. -1 = live board (the
  // common case); 0..N-1 = previewing entry N's pre-move board.
  // Changing this invalidates the cache so the historical board
  // gets re-rasterized on the first frame after navigation.
  int history_cursor;
  bool valid;
} BlitCache;

// Per-tile cache for the 2x layered renderer (text bg + small
// pixel planes per placed tile). If the (letter, owner, score,
// antialias, subs, cdy, cdx, scale) tuple hasn't changed since
// the tile was last rasterized, we skip both the rasterize AND
// the ncblit_rgba — the existing sprixel on the plane is still
// correct. This is what lets cursor scrolling through history
// snapshots cost only ~the tiles that differ between snapshots,
// instead of re-emitting the entire board.
typedef struct {
  int letter;
  int owner;
  int score;
  bool blank_uppercase;
  bool antialias;
  bool is_preview;
  int score_subscripts;
  int border_thickness;
  unsigned cdy, cdx;
  int scale;
  // bs / premium_labels: cached premium-cell inputs. The same
  // per-cell pixel plane handles both placed tiles AND empty /
  // premium cells now, so the cache key has to discriminate
  // "DLS at H8" from "TLS at H8" and from "[empty] at H8". When
  // ml==EMPTY the bake + bg come from premium_marker_for_cell
  // (which depends on bs + the premium_labels mode); when ml is a
  // letter, bs / premium_labels are ignored.
  int bs;
  int premium_labels;
  bool valid;
} TileCache;
static TileCache board_tile_cache[BOARD_DIM][BOARD_DIM];
static struct ncplane *board_tile_planes[BOARD_DIM][BOARD_DIM];

// Per-tile state for the rack panel. Same idea as the board tile
// cache: one small pixel plane per rack slot, re-blit only when
// the letter at that slot changes. RACK_SIZE = 7 in standard
// games. When the bot finalizes a move and draws fresh tiles,
// only the slots that actually changed letters re-emit instead
// of the whole rack strip thrashing.
typedef struct {
  int letter;
  int player_idx;
  int score;
  bool antialias;
  bool ghost;
  int score_subscripts;
  unsigned cdy, cdx;
  int scale;
  // Last screen position the plane was created at. Pixel-mode
  // terminals don't move sprixel pixels when ncplane_move_yx is
  // called; if our slot needs to shift (rack count changed →
  // start_col shifted), we have to destroy + recreate the plane
  // so the terminal drops the old sprixel and rebuilds at the
  // new position. Otherwise tiles can end up at mismatched y
  // coords (the "ghost Q is misplaced" bug).
  int screen_top;
  int screen_left;
  bool valid;
} RackTileCache;
static RackTileCache rack_tile_cache[RACK_SIZE];
static struct ncplane *rack_tile_planes[RACK_SIZE];

// Annotation editor's directional cursor — a pixel-blitted "next
// tile" marker drawn at the cell past the last tile of the typed
// play. Cached the same way rack tiles are: only re-rasterized
// when geometry, direction, or player changes.
typedef struct {
  bool vertical;
  int player_idx;
  int screen_top;
  int screen_left;
  unsigned cdy, cdx;
  int scale;
  bool valid;
} EditArrowCache;
static EditArrowCache edit_arrow_cache;
static struct ncplane *edit_arrow_plane;

static void invalidate_edit_arrow_plane(void) {
  if (edit_arrow_plane != NULL) {
    ncplane_destroy(edit_arrow_plane);
    edit_arrow_plane = NULL;
  }
  edit_arrow_cache.valid = false;
}

static void invalidate_rack_tile_planes(void) {
  for (int i = 0; i < RACK_SIZE; i++) {
    if (rack_tile_planes[i] != NULL) {
      ncplane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
    }
    rack_tile_cache[i].valid = false;
  }
}

// Tear down every cached tile plane. Used when scale flips back
// to 1x (planes would otherwise occlude the text-mode board) and
// at game-reset / state-destroy.
static void invalidate_tile_planes(void) {
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_tile_planes[row][col] != NULL) {
        ncplane_destroy(board_tile_planes[row][col]);
        board_tile_planes[row][col] = NULL;
      }
      board_tile_cache[row][col].valid = false;
    }
  }
  invalidate_rack_tile_planes();
  invalidate_edit_arrow_plane();
}

static BlitCache board_pixel_cache;
static BlitCache rack_pixel_cache;
static BlitCache label_pixel_cache;

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

static HistoryRowMap history_row_map[TUI_HISTORY_MAX];
static int history_row_map_count;

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
static AnalysisRowMap analysis_row_map[ANALYSIS_ROW_CAP];
static int analysis_row_map_count;
static int analysis_panel_top;
static int analysis_panel_bottom;
static int analysis_panel_left;
static int analysis_panel_right;
// Bounding box of the History panel as a whole — used by
// tui_history_cursor_at to detect clicks on the title / chrome
// (between entries) and snap the cursor back to -1 (the [4>]
// label). Refreshed every frame alongside the row map.
static int history_panel_top;
static int history_panel_bottom;
static int history_panel_left;
static int history_panel_right;

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
static ModalHitMap modal_hit_map;

static void invalidate_blit_caches(void) {
  board_pixel_cache.valid = false;
  rack_pixel_cache.valid = false;
  label_pixel_cache.valid = false;
}

// Pick which Board the board renderer should display: the entry's
// pre-move snapshot when the History panel is focused and the
// cursor is on a committed entry, otherwise the live game board.
// Pending entries fall back to live since their snapshot IS the
// current board and the spinner UX is already conveying that the
// move is in flight.
static const Board *pick_render_board(const TuiGameState *state) {
  if (state == NULL) {
    return NULL;
  }
  if (state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *entry = &state->history[state->history_cursor];
    if (!entry->pending && entry->board_before != NULL) {
      return entry->board_before;
    }
  }
  return state->game != NULL ? game_get_board(state->game) : NULL;
}

// Returns the committed history entry the user has cursored to —
// the "rewind target" for the rack panel + player pills. NULL when
// the cursor is on the label or on a pending entry, meaning the
// live game state should be shown. Cursor persists across panel
// focus changes by design, so we deliberately don't require
// History to be focused — wherever the cursor sits is what every
// rewindable view shows.
static const TuiHistoryEntry *pick_history_view(const TuiGameState *state) {
  if (state == NULL) {
    return NULL;
  }
  if (state->history_cursor < 0 ||
      state->history_cursor >= state->history_count) {
    return NULL;
  }
  const TuiHistoryEntry *entry = &state->history[state->history_cursor];
  if (entry->pending) {
    return NULL;
  }
  return entry;
}

// Rack to display for `player_idx`. When the cursor is on a
// committed turn the entry stashes both the on-turn (rack_before)
// and the off-turn (opp_rack_before) snapshots; we match by
// player_idx so the pill for the player who PLAYED that turn
// shows the rack they faced, and the opposite pill shows what
// their opponent was holding at the same moment.
static const Rack *pick_render_rack(const TuiGameState *state, int player_idx) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    if (entry->player_idx == player_idx && entry->rack_before != NULL) {
      return entry->rack_before;
    }
    if (entry->player_idx != player_idx && entry->opp_rack_before != NULL) {
      return entry->opp_rack_before;
    }
  }
  return state->game != NULL
             ? player_get_rack(game_get_player(state->game, player_idx))
             : NULL;
}

// Which player_idx is "on turn" for rendering purposes. Cursored
// to a committed entry: that turn's player_idx (they were about
// to play). Else: the live on-turn index.
static int pick_render_on_turn(const TuiGameState *state) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    return entry->player_idx;
  }
  // Once the game is over, the engine's on_turn_index has
  // advanced to whoever would have played next — but visually
  // we want the pill to stay with the player who *ended* the
  // game (the one who went out, or whose last play closed it).
  // That player is the author of the last history entry.
  if (state->game != NULL && game_over(state->game) &&
      state->history_count > 0) {
    return state->history[state->history_count - 1].player_idx;
  }
  return state->game != NULL ? game_get_player_on_turn_index(state->game) : 0;
}

static double seconds_remaining(const TuiGameState *state, int player_idx);

// When the Analysis cursor is in MOVE column, find the current
// row index of the anchored move by scanning the prepared
// rows-for-this-frame. Returns -1 if the anchored move is not
// present in the current rows. Caller falls back to RANK-mode
// behavior in that case.
static int find_anchored_move_row(const TuiGameState *state) {
  if (state == NULL || state->analysis_anchored_move[0] == '\0') {
    return -1;
  }
  for (int i = 0; i < state->last_rendered_analysis_row_count; i++) {
    if (strcmp(state->last_rendered_analysis_rows[i].move,
               state->analysis_anchored_move) == 0) {
      return i;
    }
  }
  return -1;
}

// Effective cursor row for the current frame. In RANK column the
// cursor is just state->analysis_cursor (a fixed row index). In
// MOVE column the cursor follows the anchored move's current
// position — find_anchored_move_row resolves it against the
// rows already prepared for this frame.
static int effective_analysis_cursor(const TuiGameState *state) {
  if (state == NULL) {
    return -1;
  }
  if (state->analysis_cursor < 0) {
    // Cursor on the [5] label rather than a specific candidate:
    // implicitly highlight the top rank since that's the move
    // the panel previews on the board. Falls back to -1 when
    // there's nothing to preview.
    return state->last_rendered_analysis_row_count > 0 ? 0 : -1;
  }
  if (state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE) {
    const int idx = find_anchored_move_row(state);
    if (idx >= 0) {
      return idx;
    }
  }
  return state->analysis_cursor;
}

// Resolve the Analysis-cursor row to the underlying Move for
// preview rendering on the board. Returns NULL when no preview
// applies — no row cursored, no live results, or out-of-range.
// Caller must NOT call sim_results_lock_and_sort_*; we don't
// hold the display lock across the return, so the returned
// Move pointer is borrowed and only valid while state's
// SimResults / endgame_snapshot are stable (which they are
// inside a render frame holding state->mutex).
static const Move *pick_analysis_preview_move(const TuiGameState *state,
                                              int *out_player_idx) {
  if (state == NULL) {
    return NULL;
  }
  // While the annotation cell editor is open and the engine has
  // validated the typed move, ghost THAT play on the board — it's
  // what the user is shaping right now, takes precedence over any
  // sim/endgame candidate or saved snapshot preview.
  if (state->edit_history_idx >= 0 && state->edit_preview_move_valid &&
      state->edit_preview_move != NULL &&
      state->edit_history_idx < state->history_count) {
    if (out_player_idx != NULL) {
      *out_player_idx = state->history[state->edit_history_idx].player_idx;
    }
    return state->edit_preview_move;
  }
  const int idx = effective_analysis_cursor(state);
  if (idx < 0) {
    return NULL;
  }
  // History cursor on a committed entry → preview must come from
  // the SAVED analysis for THAT turn, not the live sim. Otherwise
  // a user navigating to turn 3 would see the Analysis panel
  // show turn 3's saved leaderboard but the board would draw a
  // candidate from the bot's current-turn sim. Use the entry's
  // sim_results_saved (a deep clone of the SimResults captured at
  // finalize time), whose display_simmed_plays order matches the
  // analysis_snapshot.rows order the user is reading.
  const TuiHistoryEntry *hist_entry = pick_history_view(state);
  if (hist_entry != NULL) {
    if (hist_entry->sim_results_saved != NULL) {
      SimResults *saved = hist_entry->sim_results_saved;
      if (!sim_results_lock_and_sort_display_simmed_plays(saved)) {
        return NULL;
      }
      const Move *out_move = NULL;
      if (idx < sim_results_get_number_of_plays(saved)) {
        const SimmedPlay *play =
            sim_results_get_display_simmed_play(saved, idx);
        if (play != NULL) {
          out_move = simmed_play_get_move(play);
        }
      }
      sim_results_unlock_display_infos(saved);
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return out_move;
    }
    if (hist_entry->endgame_moves_saved != NULL &&
        idx < hist_entry->endgame_moves_saved_count) {
      // Endgame turn — replay the saved leaderboard moves the
      // same way sim turns do, so the on-board preview works
      // uniformly across both modes.
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return &hist_entry->endgame_moves_saved[idx];
    }
    // Loaded GCG entry — no sim/endgame leaderboard, just the
    // single move that was played. Surface it as the preview so
    // the board ghosts the played tiles when the cursor lands on
    // the "Plays" row.
    if (hist_entry->loaded_move != NULL && idx == 0) {
      if (out_player_idx != NULL) {
        *out_player_idx = hist_entry->player_idx;
      }
      return hist_entry->loaded_move;
    }
    return NULL;
  }
  // No history cursor — preview from live state. Endgame snapshot
  // takes precedence when the bag is empty and a solve has landed.
  const bool bag_empty =
      state->game != NULL && bag_get_letters(game_get_bag(state->game)) == 0;
  if (bag_empty && state->endgame_snapshot.valid &&
      idx < state->endgame_snapshot.num_entries &&
      state->endgame_snapshot.moves != NULL) {
    const Move *m = state->endgame_snapshot.moves[idx];
    if (out_player_idx != NULL) {
      *out_player_idx = state->endgame_snapshot.solving_player;
    }
    return m;
  }
  if (state->sim_results == NULL) {
    return NULL;
  }
  // Same gate the analysis row populator uses: sim_results
  // contents persist across game resets, so without this check
  // we'd ghost a candidate from the prior game (e.g., "GUV"
  // crosses H8 after starting annotation). sim_results_turn_idx
  // is flipped to -1 by the reset paths.
  if (atomic_load(&((TuiGameState *)state)->sim_results_turn_idx) < 0) {
    return NULL;
  }
  SimResults *results = state->sim_results;
  if (!sim_results_lock_and_sort_display_simmed_plays(results)) {
    return NULL;
  }
  const Move *out_move = NULL;
  if (idx < sim_results_get_number_of_plays(results)) {
    const SimmedPlay *play = sim_results_get_display_simmed_play(results, idx);
    if (play != NULL) {
      out_move = simmed_play_get_move(play);
    }
  }
  sim_results_unlock_display_infos(results);
  if (out_player_idx != NULL) {
    *out_player_idx = game_get_player_on_turn_index(state->game);
  }
  return out_move;
}

// Reorder an alphagram-style string (e.g. "AEHIIRV" or "?AAEIN") so
// it reads in the user's chosen rack-sort order. Used for the
// rack + leave columns inside history entries — those strings are
// pre-built engine-side as alphabetical-with-? alphagrams; we
// re-bucket them at render time so the user sees rack/leave
// orderings consistent with the live rack panel.
static int alphagram_char_cmp(const void *a, const void *b) {
  return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void format_alphagram_for_sort(const char *in,
                                      const LetterDistribution *ld,
                                      TuiRackSort sort, char *out,
                                      size_t out_size);

void tui_format_alphagram_for_sort(const char *in,
                                   const struct LetterDistribution *ld,
                                   TuiRackSort sort, char *out,
                                   size_t out_size) {
  // Forward to the in-file static implementation. Keeping the
  // static version means existing internal call sites don't have
  // to be touched.
  format_alphagram_for_sort(in, (const LetterDistribution *)ld, sort, out,
                            out_size);
}

static void format_alphagram_for_sort(const char *in,
                                      const LetterDistribution *ld,
                                      TuiRackSort sort, char *out,
                                      size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (in == NULL || ld == NULL) {
    return;
  }
  // Bucket each input char into blanks / vowels / consonants. We
  // also keep a flat "alphas" run (everything except blanks) in
  // input order, so the alpha-with-? modes don't need to scan
  // twice.
  char blanks[32] = {0};
  int nb = 0;
  char vowels[32] = {0};
  int nv = 0;
  char cons[32] = {0};
  int nc = 0;
  char alphas[64] = {0};
  int na = 0;
  for (const char *p = in; *p != '\0'; p++) {
    const char c = *p;
    if (c == '?') {
      if (nb < (int)sizeof(blanks)) {
        blanks[nb++] = c;
      }
      continue;
    }
    if (na < (int)sizeof(alphas)) {
      alphas[na++] = c;
    }
    // Map char back to ML for vowel classification. Linear scan
    // over the LD's letter table — small (≤30ish entries), and
    // alphagrams are short (≤7 chars), so this stays cheap.
    bool is_vowel = false;
    for (int ml = 1; ml < ld_get_size(ld); ml++) {
      if (ld->ld_ml_to_hl[ml][0] == c && ld->ld_ml_to_hl[ml][1] == '\0') {
        is_vowel = ld_get_is_vowel(ld, (MachineLetter)ml);
        break;
      }
    }
    if (is_vowel) {
      if (nv < (int)sizeof(vowels)) {
        vowels[nv++] = c;
      }
    } else {
      if (nc < (int)sizeof(cons)) {
        cons[nc++] = c;
      }
    }
  }
  // Sort each bucket alphabetically so the user-visible order
  // doesn't depend on what the input happened to be. Committed
  // rack_strs come from string_builder_add_rack (already in
  // ml-order, so this is a no-op) — but the annotation editor's
  // user-typed rack buffer can be in arbitrary entry order, and
  // we don't want a typed "ACKQUEL" to surface as "AUECKQL".
  if (na > 1) {
    qsort(alphas, (size_t)na, 1, alphagram_char_cmp);
  }
  if (nv > 1) {
    qsort(vowels, (size_t)nv, 1, alphagram_char_cmp);
  }
  if (nc > 1) {
    qsort(cons, (size_t)nc, 1, alphagram_char_cmp);
  }
  size_t pos = 0;
#define ALPHAGRAM_APPEND(buf, n)                                               \
  do {                                                                         \
    for (int _i = 0; _i < (n) && pos + 1 < out_size; _i++) {                   \
      out[pos++] = (buf)[_i];                                                  \
    }                                                                          \
  } while (0)
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    ALPHAGRAM_APPEND(blanks, nb);
    ALPHAGRAM_APPEND(alphas, na);
    break;
  case TUI_RACK_SORT_VOWELS:
    ALPHAGRAM_APPEND(vowels, nv);
    ALPHAGRAM_APPEND(cons, nc);
    ALPHAGRAM_APPEND(blanks, nb);
    break;
  case TUI_RACK_SORT_BLANKS_VOWELS:
    ALPHAGRAM_APPEND(blanks, nb);
    ALPHAGRAM_APPEND(vowels, nv);
    ALPHAGRAM_APPEND(cons, nc);
    break;
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    ALPHAGRAM_APPEND(alphas, na);
    ALPHAGRAM_APPEND(blanks, nb);
    break;
  }
#undef ALPHAGRAM_APPEND
  out[pos < out_size ? pos : out_size - 1] = '\0';
}

// Fill `out_slots` with the visible rack tiles for `rack` in the
// display order chosen by `sort`. Returns the slot count. The
// engine's internal rack ordering isn't touched — this is purely
// presentation. Vowel grouping uses the letter-distribution's
// is_vowel flag so the same setting works for any language.
static int sort_rack_for_display(const Rack *rack, const LetterDistribution *ld,
                                 TuiRackSort sort, MachineLetter *out_slots,
                                 int max_slots) {
  if (rack == NULL || ld == NULL || out_slots == NULL || max_slots <= 0) {
    return 0;
  }
  int n = 0;
  const int ld_size = ld_get_size(ld);
#define EMIT_ML(_ml)                                                           \
  do {                                                                         \
    const int _cnt = rack_get_letter(rack, (MachineLetter)(_ml));              \
    for (int _c = 0; _c < _cnt && n < max_slots; _c++) {                       \
      out_slots[n++] = (MachineLetter)(_ml);                                   \
    }                                                                          \
  } while (0)
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    // ? first, then A..Z. Machine-letter order already encodes
    // this since BLANK_MACHINE_LETTER == 0.
    for (int ml = 0; ml < ld_size && n < max_slots; ml++) {
      EMIT_ML(ml);
    }
    break;
  case TUI_RACK_SORT_BLANKS_VOWELS:
    EMIT_ML(BLANK_MACHINE_LETTER);
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    break;
  case TUI_RACK_SORT_VOWELS:
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        EMIT_ML(ml);
      }
    }
    EMIT_ML(BLANK_MACHINE_LETTER);
    break;
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    // Default: A..Z first, blank last.
    for (int ml = 1; ml < ld_size && n < max_slots; ml++) {
      EMIT_ML(ml);
    }
    EMIT_ML(BLANK_MACHINE_LETTER);
    break;
  }
#undef EMIT_ML
  return n;
}

// Mark which rack slots correspond to tiles consumed by the
// currently-previewed move. Visited in slot order so the first
// occurrence of each used letter gets ghosted (the rack panel
// renders tiles in alphabetical order, so this stays stable).
// out_ghost[i] is set to true when slot_letters[i] is a rack
// tile the preview move would spend.
static void compute_rack_ghost_mask(const TuiGameState *state,
                                    const MachineLetter *slot_letters,
                                    int slot_count, bool *out_ghost) {
  for (int i = 0; i < slot_count; i++) {
    out_ghost[i] = false;
  }
  const Move *m = pick_analysis_preview_move(state, NULL);
  if (m == NULL) {
    return;
  }
  const game_event_t mtype = move_get_type(m);
  if (mtype != GAME_EVENT_TILE_PLACEMENT_MOVE && mtype != GAME_EVENT_EXCHANGE) {
    return;
  }
  // Count rack-tiles needed by the move. Blanks consume a blank
  // rack tile (ml = 0) regardless of which letter they stand in
  // for, so strip the blank bit before bucketing. Exchanges have
  // no PLAYED_THROUGH_MARKER positions; placements may.
  int needed[256] = {0};
  const int n = mtype == GAME_EVENT_EXCHANGE ? move_get_tiles_played(m)
                                             : move_get_tiles_length(m);
  for (int t = 0; t < n; t++) {
    const MachineLetter tile = move_get_tile(m, t);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const MachineLetter rack_ml =
        get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    if ((int)rack_ml < 256) {
      needed[(int)rack_ml]++;
    }
  }
  for (int i = 0; i < slot_count; i++) {
    const MachineLetter ml = slot_letters[i];
    if ((int)ml < 256 && needed[(int)ml] > 0) {
      out_ghost[i] = true;
      needed[(int)ml]--;
    }
  }
}

// Clock seconds to display for `player_idx`. When cursor is on a
// committed entry, returns the player's time-remaining as
// snapshotted at the start of that turn (the same moment the
// rack snapshot was taken). Else falls through to the live
// monotonic-clock derivation.
static double pick_render_clock_seconds(const TuiGameState *state,
                                        int player_idx) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry != NULL) {
    const int snap = entry->player_idx == player_idx
                         ? entry->clock_at_start
                         : entry->opp_clock_at_start;
    return snap < 0 ? 0.0 : (double)snap;
  }
  return seconds_remaining(state, player_idx);
}

// Score to display for `player_idx`. When cursor is on entry K
// we return the score they had going INTO turn K (i.e. before
// turn K's play was applied). For the player who played K that
// is total_after - score; for any earlier player we walk back to
// their most-recent prior total_after.
static int pick_render_score(const TuiGameState *state, int player_idx) {
  const TuiHistoryEntry *entry = pick_history_view(state);
  if (entry == NULL) {
    return state->game != NULL ? equity_to_int(player_get_score(
                                     game_get_player(state->game, player_idx)))
                               : 0;
  }
  for (int idx = state->history_cursor - 1; idx >= 0; idx--) {
    const TuiHistoryEntry *prior = &state->history[idx];
    if (prior->pending) {
      continue;
    }
    if (prior->player_idx == player_idx) {
      return prior->total_after + prior->end_bonus;
    }
  }
  return 0;
}

static void invalidate_grid_planes(void) {
  if (grid_planes.board != NULL) {
    ncplane_destroy(grid_planes.board);
    grid_planes.board = NULL;
  }
  if (grid_planes.rack != NULL) {
    ncplane_destroy(grid_planes.rack);
    grid_planes.rack = NULL;
  }
  if (grid_planes.labels_col != NULL) {
    ncplane_destroy(grid_planes.labels_col);
    grid_planes.labels_col = NULL;
  }
  if (grid_planes.labels_row != NULL) {
    ncplane_destroy(grid_planes.labels_row);
    grid_planes.labels_row = NULL;
  }
  if (grid_planes.modal != NULL) {
    ncplane_destroy(grid_planes.modal);
    grid_planes.modal = NULL;
  }
  invalidate_blit_caches();
  invalidate_tile_planes();
}

void tui_game_render_reset_grids(void) { invalidate_grid_planes(); }

// Map a cell coordinate to the panel beneath it. Mirrors the layout
// compute_layout produces for the same state. Returns one of the
// TUI_FOCUS_* values, or -1 when the click misses every panel.
int tui_game_panel_at(struct ncplane *plane, const TuiGameState *state, int y,
                      int x) {
  if (plane == NULL || state == NULL || y < 0 || x < 0) {
    return -1;
  }
  // Pick the user's preferred scale the same way tui_game_render
  // does so the recomputed layout matches what's actually on screen.
  struct notcurses *render_nc = ncplane_notcurses(plane);
  const bool pixel_ok = render_nc != NULL && notcurses_canpixel(render_nc);
  const int user_pref =
      (state->board_scale >= 2 && pixel_ok && state->glyph_cache != NULL) ? 2
                                                                          : 1;
  const Layout L = compute_layout(plane, user_pref, state);
  if (L.scale < 0) {
    return -1;
  }
  // Command bar and status bar — clicks on either focus [0].
  if (y == L.command_bar_row || y == L.status_row) {
    return TUI_FOCUS_NONE;
  }
  // Right column is the pills+history assembly (treated as one
  // component) or the separate panels in 3-column mode. The
  // combined_pills_history bool decides.
  if (x >= L.right_col_left && x <= L.right_col_right) {
    if (L.combined_pills_history) {
      if (y >= L.pill1_top && y <= L.history_bottom) {
        return TUI_FOCUS_HISTORY;
      }
    } else {
      if (L.has_analysis && y >= L.analysis_top && y <= L.analysis_bottom &&
          x >= L.analysis_left && x <= L.analysis_right) {
        return TUI_FOCUS_ANALYSIS;
      }
      if (y >= L.history_top && y <= L.history_bottom) {
        return TUI_FOCUS_HISTORY;
      }
    }
  }
  // Three-column analysis lives even further right (separate from
  // the history column).
  if (L.has_analysis && y >= L.analysis_top && y <= L.analysis_bottom &&
      x >= L.analysis_left && x <= L.analysis_right) {
    return TUI_FOCUS_ANALYSIS;
  }
  // Left column: board (top), rack, bag stacked vertically.
  if (x >= 0 && x < L.board_width) {
    if (y <= L.board_bottom_row + 1) {
      return TUI_FOCUS_BOARD;
    }
    if (y >= L.rack_top && y <= L.rack_bottom) {
      return TUI_FOCUS_RACK;
    }
    if (y >= L.bag_top && y <= L.bag_bottom) {
      return TUI_FOCUS_BAG;
    }
  }
  return -1;
}

int tui_analysis_cursor_at(int y, int x) {
  return tui_analysis_cursor_column_at(y, x, NULL);
}

int tui_analysis_cursor_column_at(int y, int x, TuiAnalysisColumn *out_column) {
  if (y < analysis_panel_top || y > analysis_panel_bottom ||
      x < analysis_panel_left || x > analysis_panel_right) {
    return -2;
  }
  for (int i = 0; i < analysis_row_map_count; i++) {
    const AnalysisRowMap *m = &analysis_row_map[i];
    if (y >= m->top_row && y <= m->bottom_row && x >= m->left_col &&
        x <= m->right_col) {
      if (out_column != NULL) {
        *out_column = (x >= m->move_left_col) ? TUI_ANALYSIS_COLUMN_MOVE
                                              : TUI_ANALYSIS_COLUMN_RANK;
      }
      return m->idx;
    }
  }
  return -1;
}

int tui_modal_item_at(int y, int x) {
  if (!modal_hit_map.valid) {
    return -2;
  }
  // Outside the modal entirely (including the shadow column/row).
  if (y < modal_hit_map.outer_top || y > modal_hit_map.outer_bottom ||
      x < modal_hit_map.outer_left || x > modal_hit_map.outer_right) {
    return -2;
  }
  // Inside the modal but on the chrome (top/bottom border, side
  // columns). The clickable item rectangle runs from
  // (top, left) to (top + item_count - 1, right) inclusive.
  if (y < modal_hit_map.top ||
      y >= modal_hit_map.top + modal_hit_map.item_count ||
      x < modal_hit_map.left || x > modal_hit_map.right) {
    return -1;
  }
  const int idx = y - modal_hit_map.top;
  if (idx < 0 || idx >= modal_hit_map.item_count) {
    return -1;
  }
  // Disabled items count as chrome — clicks are absorbed but not
  // activated.
  if (modal_hit_map.disabled[idx]) {
    return -1;
  }
  return idx;
}

TuiModalChevron tui_modal_chevron_at(int y, int x) {
  const int idx = tui_modal_item_at(y, x);
  if (idx < 0) {
    return TUI_MODAL_CHEVRON_NONE;
  }
  // The chevron column was recorded as the screen col of the
  // glyph cell itself. Accept that exact col as a hit.
  if (modal_hit_map.left_chev_col[idx] >= 0 &&
      x == modal_hit_map.left_chev_col[idx]) {
    return TUI_MODAL_CHEVRON_LEFT;
  }
  if (modal_hit_map.right_chev_col[idx] >= 0 &&
      x == modal_hit_map.right_chev_col[idx]) {
    return TUI_MODAL_CHEVRON_RIGHT;
  }
  return TUI_MODAL_CHEVRON_NONE;
}

int tui_history_cursor_at(int y, int x) {
  return tui_history_cursor_field_at(y, x, NULL);
}

int tui_history_cursor_field_at(int y, int x, int *out_field) {
  // Outside the History panel entirely.
  if (y < history_panel_top || y > history_panel_bottom ||
      x < history_panel_left || x > history_panel_right) {
    return -2;
  }
  // Inside the panel — see whether the point falls on one of the
  // per-entry rectangles populated during the last render.
  for (int i = 0; i < history_row_map_count; i++) {
    const HistoryRowMap *m = &history_row_map[i];
    if (y >= m->top_row && y <= m->bottom_row && x >= m->left_col &&
        x <= m->right_col) {
      if (out_field != NULL) {
        // Row offset within the entry, with a special "leave"
        // sentinel for clicks landing in the right-anchored leave
        // column on the top row:
        //   0 = move text (top row, left of leave zone)
        //   1 = rack (second row)
        //   2 = leave (top row, inside the leave hit zone)
        //   3+ = end-bonus extension, unchanged
        const int dy = y - m->top_row;
        if (dy == 0 && m->leave_left >= 0 && x >= m->leave_left &&
            x <= m->leave_right) {
          *out_field = 2;
        } else {
          *out_field = dy;
        }
      }
      return m->idx;
    }
  }
  // Inside the panel but not on any entry — title / chrome / blank
  // space. Caller snaps the cursor back to -1 (the [4>] label).
  return -1;
}

// Public accessor for the cached modal plane, shared across all modal
// renderers (menu / settings / time picker / lexicon picker). Creates
// the plane on first use; resizes/repositions on subsequent calls.
// Returns NULL on allocation failure. The plane is destroyed when
// tui_game_render sees modal == TUI_MODAL_NONE, so callers don't
// have to manage its lifetime.
struct ncplane *
tui_game_render_get_or_create_modal_plane(struct ncplane *parent, int top,
                                          int left, int rows, int cols) {
  if (grid_planes.modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)rows;
    opts.cols = (unsigned)cols;
    opts.name = "modal";
    grid_planes.modal = ncplane_create(parent, &opts);
    return grid_planes.modal;
  }
  unsigned cur_rows = 0;
  unsigned cur_cols = 0;
  ncplane_dim_yx(grid_planes.modal, &cur_rows, &cur_cols);
  if ((int)cur_rows != rows || (int)cur_cols != cols) {
    ncplane_resize_simple(grid_planes.modal, (unsigned)rows, (unsigned)cols);
  }
  ncplane_move_yx(grid_planes.modal, top, left);
  return grid_planes.modal;
}

// Acquire (or move/resize) a cached child plane via a pointer-to-pointer
// slot in `grid_planes`. On first call we allocate and set the base cell
// to fully transparent; on subsequent calls we just reposition/resize.
static struct ncplane *acquire_grid_plane(struct ncplane **slot,
                                          struct ncplane *parent,
                                          const char *name, int y, int x,
                                          int rows, int cols) {
  if (rows <= 0 || cols <= 0) {
    return NULL;
  }
  if (*slot == NULL) {
    ncplane_options opts = {0};
    opts.y = y;
    opts.x = x;
    opts.rows = (unsigned)rows;
    opts.cols = (unsigned)cols;
    opts.name = name;
    *slot = ncplane_create(parent, &opts);
    if (*slot == NULL) {
      return NULL;
    }
    uint64_t base_ch = 0;
    ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
    ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
    ncplane_set_base(*slot, " ", 0, base_ch);
    return *slot;
  }
  unsigned cur_rows = 0;
  unsigned cur_cols = 0;
  ncplane_dim_yx(*slot, &cur_rows, &cur_cols);
  if ((int)cur_rows != rows || (int)cur_cols != cols) {
    ncplane_resize_simple(*slot, (unsigned)rows, (unsigned)cols);
  }
  ncplane_move_yx(*slot, y, x);
  return *slot;
}

// ── Board ─────────────────────────────────────────────────────────────────
//
// Cell rendering is offset-parameterized so the same routine drives the
// in-game board (at CELL_ROW_BASE/CELL_COL_BASE) and the theme picker's
// preview (at whatever offset the picker chose).
static void render_board_cells(struct ncplane *plane, const Theme *theme,
                               const Board *board, const LetterDistribution *ld,
                               bool blank_uppercase,
                               TuiPremiumLabels premium_labels,
                               int border_thickness, int cell_w, int top,
                               int left) {
  // Borders are delivered separately by a pixel-graphics overlay plane
  // at scale < 2 (render_board_grid_overlay, called from
  // tui_game_render). Don't approximate them with NCSTYLE_UNDERLINE —
  // the underline reads as part of the glyph on fullwidth tiles and
  // the user vetoed that look.
  (void)border_thickness;
  const bool halfwidth = (cell_w == 1);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int screen_row = top + row;
      const int screen_col = left + col * cell_w;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker = premium_marker_for_cell(
            theme, bs, row, col, premium_labels, cell_w);
        theme_apply_fg(plane, marker.fg);
        theme_apply_bg(plane, marker.bg);
        ncplane_putstr_yx(plane, screen_row, screen_col, marker.glyph);
        continue;
      }
      const bool is_blank = get_is_blanked(ml);
      const bool render_uppercase = is_blank && blank_uppercase;
      const MachineLetter glyph_ml =
          render_uppercase ? get_unblanked_machine_letter(ml) : ml;
      const int owner = board_get_square_owner(board, row, col);
      const ThemeRgb tile_fg = owner == 1 ? theme->tile2_fg : theme->tile1_fg;
      const ThemeRgb tile_bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
      theme_apply_fg(plane, is_blank ? theme->blank_tile_fg : tile_fg);
      theme_apply_bg(plane, tile_bg);
      if (halfwidth) {
        const char *ascii = ld->ld_ml_to_hl[glyph_ml];
        ncplane_putstr_yx(plane, screen_row, screen_col,
                          ascii[0] != '\0' ? ascii : " ");
      } else {
        const char *fullwidth = ld->ld_ml_to_alt_hl[glyph_ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
        } else {
          ncplane_putstr_yx(plane, screen_row, screen_col, " ");
          ncplane_putstr(plane, ld->ld_ml_to_hl[glyph_ml]);
        }
      }
    }
  }
}

// 2x pixel-composite render. Builds one RGBA image covering the whole
// board area in a single child plane and ncblit_rgba's it. Each tile is
// (4 * cdx) × (2 * cdy) pixels: filled with the cell's bg, then the
// cached glyph alpha-blended on top using the cell's fg.
// Primitive: blit a glyph alpha-mask into `buf`, with the glyph's top-
// left pixel anchored at (glyph_left, glyph_top). Per-pixel bounds-check
// — necessary because callers position glyphs near tile edges where
// negative starts are routine.
// `bold`: when true, simulate a heavier weight by doing a second
// pass shifted +1px horizontally. Cheap fake-bold suitable for the
// "preview tile" look on top of the regular non-bold rasterizer.
static void blit_glyph_at_bold(uint8_t *buf, int buf_w, int buf_h,
                               int glyph_left, int glyph_top, const TuiGlyph *g,
                               ThemeRgb fg, ThemeRgb bg, bool bold);

static void blit_glyph_at(uint8_t *buf, int buf_w, int buf_h, int glyph_left,
                          int glyph_top, const TuiGlyph *g, ThemeRgb fg,
                          ThemeRgb bg) {
  blit_glyph_at_bold(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg,
                     /*bold=*/false);
}

static void blit_glyph_at_bold(uint8_t *buf, int buf_w, int buf_h,
                               int glyph_left, int glyph_top, const TuiGlyph *g,
                               ThemeRgb fg, ThemeRgb bg, bool bold) {
  if (g == NULL || g->width <= 0 || g->height <= 0) {
    return;
  }
  // The bold pass simulates a heavier weight by OR-ing each row's
  // alpha with the same row shifted +1px right — gives a 1-pixel
  // stroke widening without re-rasterizing. Pure CPU work, runs
  // off the same cached alpha buffer.
  (void)bold;
  for (int row = 0; row < g->height; row++) {
    const int dst_y = glyph_top + row;
    if (dst_y < 0 || dst_y >= buf_h) {
      continue;
    }
    const unsigned char *src = g->alpha + (size_t)row * g->width;
    for (int col = 0; col < g->width; col++) {
      const int dst_x = glyph_left + col;
      if (dst_x < 0 || dst_x >= buf_w) {
        continue;
      }
      const unsigned int a = src[col];
      if (a == 0) {
        continue;
      }
      uint8_t *dst = buf + ((size_t)dst_y * buf_w + (size_t)dst_x) * 4;
      // Linear-space blend between bg and fg; close enough at the
      // contrast levels we ship.
      dst[0] = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
      dst[1] = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
      dst[2] = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
      dst[3] = 255;
    }
  }
}

// Centered-in-tile placement (the historical default). Used for
// premium-square labels and for tile letters when score subscripts
// are off. Baseline biased to 72% down the tile so the glyph optical-
// centers about right for cap-only Latin letters.
static void blit_glyph_into_buf(uint8_t *buf, int buf_w, int buf_h, int tx,
                                int ty, int tile_w, int tile_h,
                                const TuiGlyph *g, ThemeRgb fg, ThemeRgb bg) {
  if (g == NULL || g->width <= 0 || g->height <= 0) {
    return;
  }
  const int baseline = ty + (int)(tile_h * 0.72);
  const int glyph_top = baseline - g->bearing_y;
  const int glyph_left = tx + (tile_w - g->width) / 2;
  blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
}

static void fill_tile_rect(uint8_t *buf, int buf_w, int tx, int ty, int tile_w,
                           int tile_h, ThemeRgb color) {
  for (int row = 0; row < tile_h; row++) {
    uint8_t *p = buf + ((size_t)(ty + row) * buf_w + tx) * 4;
    for (int col = 0; col < tile_w; col++, p += 4) {
      p[0] = color.r;
      p[1] = color.g;
      p[2] = color.b;
      p[3] = 255;
    }
  }
}

// Paint grid lines into the buffer using the same "bottom + right edge
// of each tile" geometry as draw_pixel_grid (which the 1x text path
// uses against a separate overlay plane). At 2x the pixel composite
// owns the only plane covering the board, so we draw the lines inline
// here instead of stacking another plane that would just collide.
static void overlay_grid_lines(uint8_t *buf, int buf_w, int buf_h, int tiles_y,
                               int tiles_x, int tile_h_px, int tile_w_px,
                               int thickness, ThemeRgb color) {
  if (thickness <= 0) {
    return;
  }
  // Horizontal: one band per tile row, sitting in the last `thickness`
  // pixels of that row's vertical extent.
  for (int i = 1; i <= tiles_y; i++) {
    const int line_top = i * tile_h_px - thickness;
    for (int t = 0; t < thickness; t++) {
      const int row = line_top + t;
      if (row < 0 || row >= buf_h) {
        continue;
      }
      uint8_t *p = buf + (size_t)row * buf_w * 4;
      for (int col = 0; col < buf_w; col++, p += 4) {
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = 255;
      }
    }
  }
  // Vertical: one band per tile column, sitting in the last `thickness`
  // pixels of that column's horizontal extent.
  for (int i = 1; i <= tiles_x; i++) {
    const int line_left = i * tile_w_px - thickness;
    for (int t = 0; t < thickness; t++) {
      const int col = line_left + t;
      if (col < 0 || col >= buf_w) {
        continue;
      }
      for (int row = 0; row < buf_h; row++) {
        uint8_t *p = buf + ((size_t)row * buf_w + col) * 4;
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = 255;
      }
    }
  }
}

// Composes the full 2x board RGBA buffer for the given inputs and
// returns a heap-allocated buffer of size *out_buf_w * *out_buf_h
// * 4 bytes. Caller owns the buffer and must free() it. Returns
// NULL on allocation failure.
//
// The function is pure compute (no notcurses calls) so it's safe
// to invoke from a worker thread provided the supplied glyph
// caches are not in concurrent use elsewhere — it calls
// tui_glyph_cache_set_size on both, which would race with a
// concurrent reader of the same cache. The TUI keeps one set of
// caches owned by the UI thread (used for rack + label renders)
// and a separate pair owned by the pixel worker (used here).
static uint8_t *
compose_board_pixels(const Board *board, const LetterDistribution *ld,
                     const Theme *theme, TuiGlyphCache *glyph_cache,
                     TuiGlyphCache *glyph_cache_sub, int board_cell_w,
                     int board_cell_h, unsigned cdy, unsigned cdx,
                     bool blank_uppercase, TuiPremiumLabels premium_labels,
                     bool antialias, TuiScoreSubscripts score_subscripts,
                     int border_thickness, int *out_buf_w, int *out_buf_h) {
  const int tile_w = (int)cdx * board_cell_w;
  const int tile_h = (int)cdy * board_cell_h;
  const int buf_w = tile_w * BOARD_DIM;
  const int buf_h = tile_h * BOARD_DIM;
  if (buf_w <= 0 || buf_h <= 0) {
    return NULL;
  }
  const bool subs_on =
      score_subscripts != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
  if (buf == NULL) {
    return NULL;
  }

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int tx = col * tile_w;
      const int ty = row * tile_h;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      ThemeRgb bg;
      ThemeRgb fg;
      uint32_t glyph_codepoint = 0;
      uint32_t glyph_second = 0;
      bool is_placed_tile = false;
      int tile_score = 0;

      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker = premium_marker_for_cell(
            theme, bs, row, col, premium_labels, board_cell_w);
        bg = marker.bg;
        fg = marker.fg;
        if ((unsigned char)marker.glyph[0] < 0x80 &&
            (unsigned char)marker.glyph[1] < 0x80 && marker.glyph[0] != ' ') {
          glyph_codepoint = (uint32_t)marker.glyph[0];
          glyph_second = (uint32_t)marker.glyph[1];
        }
      } else {
        is_placed_tile = true;
        const bool is_blank = get_is_blanked(ml);
        const bool render_uppercase = is_blank && blank_uppercase;
        const MachineLetter glyph_ml =
            render_uppercase ? get_unblanked_machine_letter(ml) : ml;
        const int owner = board_get_square_owner(board, row, col);
        bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
        fg = is_blank ? theme->blank_tile_fg
                      : (owner == 1 ? theme->tile2_fg : theme->tile1_fg);
        const char *ascii = ld->ld_ml_to_hl[glyph_ml];
        if (ascii != NULL && ascii[0] != '\0' &&
            (unsigned char)ascii[0] < 0x80) {
          glyph_codepoint = (uint32_t)ascii[0];
        }
        tile_score = equity_to_int(ld_get_score(ld, ml));
      }

      fill_tile_rect(buf, buf_w, tx, ty, tile_w, tile_h, bg);

      const bool show_subscript =
          is_placed_tile && subs_on &&
          (score_subscripts == TUI_SCORE_SUBSCRIPTS_ALL || tile_score != 0);
      if (glyph_codepoint != 0 && glyph_second == 0) {
        const TuiGlyph *g = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
        if (is_placed_tile && subs_on && g != NULL && g->width > 0 &&
            g->height > 0) {
          const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
          const int shift_x = (int)((double)tile_w * shift_x_frac);
          const int shift_y = (int)((double)tile_h * 0.08);
          const int baseline = ty + (int)(tile_h * 0.72) - shift_y;
          const int glyph_top = baseline - g->bearing_y;
          const int glyph_left = tx + (tile_w - g->width) / 2 - shift_x;
          blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
        } else {
          blit_glyph_into_buf(buf, buf_w, buf_h, tx, ty, tile_w, tile_h, g, fg,
                              bg);
        }
      } else if (glyph_codepoint != 0 && glyph_second != 0) {
        const TuiGlyph *g1 = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
        const TuiGlyph *g2 = tui_glyph_cache_get(glyph_cache, glyph_second);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx, ty, tile_w / 2, tile_h, g1,
                            fg, bg);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx + tile_w / 2, ty, tile_w / 2,
                            tile_h, g2, fg, bg);
      }

      if (show_subscript) {
        char digits[8];
        snprintf(digits, sizeof(digits), "%d", tile_score);
        const int margin_x = (int)((double)tile_w * 0.12);
        const int margin_y = (int)((double)tile_h * 0.16);
        const int digit_bottom = ty + tile_h - margin_y;
        int pen_right = tx + tile_w - margin_x;
        for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
          const TuiGlyph *gd =
              tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
          if (gd == NULL || gd->width <= 0) {
            continue;
          }
          const int gleft = pen_right - gd->width;
          const int gtop = digit_bottom - gd->height;
          blit_glyph_at(buf, buf_w, buf_h, gleft, gtop, gd, fg, bg);
          pen_right = gleft - 1;
        }
      }
    }
  }

  overlay_grid_lines(buf, buf_w, buf_h, BOARD_DIM, BOARD_DIM, tile_h, tile_w,
                     border_thickness, theme->bg);

  *out_buf_w = buf_w;
  *out_buf_h = buf_h;
  return buf;
}

// Pixel-worker thread main loop. Pulls one pending request at a
// time off state->pixel_request, composes the RGBA buffer using
// the worker's dedicated glyph caches, then publishes the result
// into state->pixel_result. The UI thread picks up the result on
// its next frame and ncblits it onto the board plane. Holds
// pixel_mutex for the lock/unlock around request + result slots
// only; the heavy compose runs unlocked so the UI thread can post
// follow-up requests while the worker is busy.
void *tui_pixel_worker_main(void *arg) {
  TuiGameState *state = (TuiGameState *)arg;
  while (true) {
    pthread_mutex_lock(&state->pixel_mutex);
    while (!state->pixel_request.pending && !atomic_load(&state->pixel_stop)) {
      pthread_cond_wait(&state->pixel_cond, &state->pixel_mutex);
    }
    if (atomic_load(&state->pixel_stop)) {
      pthread_mutex_unlock(&state->pixel_mutex);
      break;
    }
    Board *board = state->pixel_request.board;
    state->pixel_request.board = NULL;
    state->pixel_request.pending = false;
    const Theme *theme = (const Theme *)state->pixel_request.theme;
    const int scale = state->pixel_request.scale;
    const int cell_w = state->pixel_request.cell_w;
    const int cell_h = state->pixel_request.cell_h;
    const unsigned cdy = state->pixel_request.cdy;
    const unsigned cdx = state->pixel_request.cdx;
    const bool blank_uppercase = state->pixel_request.blank_uppercase;
    const bool antialias = state->pixel_request.antialias;
    const TuiPremiumLabels premium_labels = state->pixel_request.premium_labels;
    const TuiScoreSubscripts score_subscripts =
        state->pixel_request.score_subscripts;
    const int border_thickness = state->pixel_request.border_thickness;
    const uint64_t version = state->pixel_request.version;
    const int history_cursor = state->pixel_request.history_cursor;
    pthread_mutex_unlock(&state->pixel_mutex);

    int buf_w = 0;
    int buf_h = 0;
    uint8_t *buf = compose_board_pixels(
        board, state->ld, theme, state->pixel_glyph_cache,
        state->pixel_glyph_cache_sub, cell_w, cell_h, cdy, cdx, blank_uppercase,
        premium_labels, antialias, score_subscripts, border_thickness, &buf_w,
        &buf_h);
    if (board != NULL) {
      board_destroy(board);
    }
    if (buf == NULL) {
      continue;
    }

    pthread_mutex_lock(&state->pixel_mutex);
    if (state->pixel_result.buf != NULL) {
      // UI never consumed the prior result — drop it. Newer is
      // better.
      free(state->pixel_result.buf);
    }
    state->pixel_result.buf = buf;
    state->pixel_result.buf_w = buf_w;
    state->pixel_result.buf_h = buf_h;
    state->pixel_result.scale = scale;
    state->pixel_result.cell_w = cell_w;
    state->pixel_result.cell_h = cell_h;
    state->pixel_result.cdy = cdy;
    state->pixel_result.cdx = cdx;
    state->pixel_result.blank_uppercase = blank_uppercase;
    state->pixel_result.antialias = antialias;
    state->pixel_result.premium_labels = premium_labels;
    state->pixel_result.score_subscripts = score_subscripts;
    state->pixel_result.border_thickness = border_thickness;
    state->pixel_result.version = version;
    state->pixel_result.history_cursor = history_cursor;
    state->pixel_result.ready = true;
    pthread_mutex_unlock(&state->pixel_mutex);
  }
  return NULL;
}

// Populate the 15x15 preview-letter map for the currently-cursored
// Analysis row. Empties out_letters first. Only cells currently
// empty on the rendered board receive preview tiles; played-
// through squares keep their original letter. The candidate move
// is resolved through pick_analysis_preview_move so saved-turn
// navigation pulls from the matching history snapshot rather
// than the live sim.
static void fill_preview_map(const TuiGameState *state, const Board *board,
                             MachineLetter out_letters[BOARD_DIM][BOARD_DIM],
                             int *out_owner) {
  memset(out_letters, 0,
         sizeof(MachineLetter) * (size_t)(BOARD_DIM * BOARD_DIM));
  *out_owner = 0;
  const Move *preview_move = pick_analysis_preview_move(state, out_owner);
  if (preview_move == NULL ||
      move_get_type(preview_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int dir = move_get_dir(preview_move);
  int pr = move_get_row_start(preview_move);
  int pc = move_get_col_start(preview_move);
  const int n = move_get_tiles_length(preview_move);
  for (int i = 0; i < n; i++) {
    if (pr >= 0 && pr < BOARD_DIM && pc >= 0 && pc < BOARD_DIM) {
      const MachineLetter t = move_get_tile(preview_move, i);
      if (t != PLAYED_THROUGH_MARKER &&
          board_get_letter(board, pr, pc) == ALPHABET_EMPTY_SQUARE_MARKER) {
        out_letters[pr][pc] = t;
      }
    }
    if (board_is_dir_vertical(dir)) {
      pr++;
    } else {
      pc++;
    }
  }
}

// Compose a tile-sized RGBA buffer holding a directional arrow.
// FreeType-rasterized Unicode arrow glyph (➡ U+27A1 / ⬇ U+2B07),
// sized to mostly fill the 4×2 cell and centered on the glyph's
// own bounding box (NOT the letter baseline — symbols don't share
// the alphabet's baseline metric, so a baseline-centered blit
// would pin them to the cell's upper-left).
static uint8_t *compose_arrow_pixels(bool vertical, int player_idx, int tile_w,
                                     int tile_h, bool antialias,
                                     int border_thickness,
                                     TuiGlyphCache *glyph_cache,
                                     const Theme *theme) {
  if (glyph_cache == NULL) {
    return NULL;
  }
  if (tile_w <= 0 || tile_h <= 0) {
    return NULL;
  }
  // Transparent buffer (calloc gives alpha=0 everywhere). We only
  // paint the arrow glyph itself and the right + bottom grid
  // border — everywhere else stays alpha=0 so the cell's pixel
  // plane underneath shows through (premium color, TW label,
  // etc.). The arrow reads as a "floating" cursor over whatever
  // cell it sits on rather than a filled-in tile.
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  const ThemeRgb fg = player_idx == 1 ? theme->tile2_fg : theme->tile1_fg;
  // Account for the baked right/bottom grid border. The arrow
  // glyph centers on the cell's *content* area, not the full
  // cell, so its center matches the centers of letters on
  // neighboring tiles (those center on (tile_w-t)/2 because
  // their bake takes the right + bottom t pixels).
  const int content_w = (border_thickness > 0 && border_thickness < tile_w)
                            ? tile_w - border_thickness
                            : tile_w;
  const int content_h = (border_thickness > 0 && border_thickness < tile_h)
                            ? tile_h - border_thickness
                            : tile_h;
  // For blit_glyph_at's alpha blend, we use a synthetic "bg" of
  // pure transparent — glyph pixels with alpha < 255 will blend
  // toward (0,0,0) but alpha will be set to 255 by blit_glyph_at,
  // so anti-aliased edges may darken slightly against the cell
  // bg underneath. If that's visually distracting we can switch
  // to a no-AA glyph path.
  const ThemeRgb transparent_bg = {.r = 0, .g = 0, .b = 0};

  // Match the letter em-size used by compose_tile_pixels with
  // subscripts on: tile_h * 0.50. Same em means the arrow's
  // rasterized bitmap stands at the same visual weight as the
  // letters on neighboring tiles instead of dwarfing them.
  // (The arrow glyph occupies less of its em-square than a
  // capital letter, so it'll read as slightly narrower; that's
  // fine — same height is the goal.)
  const int glyph_px = (int)((double)tile_h * 0.50);
  tui_glyph_cache_set_size(glyph_cache, glyph_px, antialias);

  const uint32_t codepoint = vertical ? 0x2193 /* ↓ */ : 0x2192 /* → */;
  // Bold variant — the regular weight of these symbol glyphs is
  // already heavy, but the bold pass embolden's the outline and
  // gives a slightly chunkier look that reads more confidently
  // as a "cursor" rather than as a text decoration.
  const TuiGlyph *g = tui_glyph_cache_get_bold(glyph_cache, codepoint);
  if (g != NULL && g->width > 0 && g->height > 0) {
    const int glyph_top = (content_h - g->height) / 2;
    const int glyph_left = (content_w - g->width) / 2;
    blit_glyph_at(buf, tile_w, tile_h, glyph_left, glyph_top, g, fg,
                  transparent_bg);
  }
  // Bake the grid border on the right + bottom in theme->bg —
  // matches the border on the cell pixel plane underneath. This
  // is the one place we have opaque pixels (besides the glyph);
  // the rest of the buffer stays alpha=0 so the underlying
  // premium / empty cell shows through.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int row = 0; row < tile_h; row++) {
      uint8_t *p = buf + ((size_t)row * tile_w + (tile_w - t)) * 4;
      for (int col = 0; col < t; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int row = tile_h - t; row < tile_h; row++) {
      uint8_t *p = buf + (size_t)row * tile_w * 4;
      for (int col = 0; col < tile_w; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }
  return buf;
}

// Compose a premium / empty cell's RGBA buffer. Same shape and
// border bake as compose_tile_pixels — bg color from the premium
// marker, optional 1- or 2-char ASCII label glyph, right + bottom
// theme->bg border. Letting every cell (premium, empty, tile,
// arrow) come from a per-cell pixel plane means the grid borders
// are uniformly baked into sprixels at the same pixel positions
// instead of split between sprixel-baked tiles + an overlay plane
// for premiums — which caused the visible thickness asymmetry.
static uint8_t *
compose_premium_pixels(BonusSquare bs, int row, int col, int tile_w, int tile_h,
                       TuiGlyphCache *glyph_cache, int board_cell_w,
                       TuiPremiumLabels premium_labels, bool antialias,
                       int border_thickness, const Theme *theme) {
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  // premium_marker_for_cell expects 1 for halfwidth, anything else
  // for fullwidth. Our 4-wide-cell 2x scale is fullwidth.
  const PremiumMarker marker = premium_marker_for_cell(
      theme, bs, row, col, premium_labels, board_cell_w == 1 ? 1 : 2);
  fill_tile_rect(buf, tile_w, 0, 0, tile_w, tile_h, marker.bg);

  // ASCII label glyphs only — fullwidth premium glyphs are
  // multi-byte UTF-8 (≥0x80 first byte) which we don't pixel-render
  // here. The bg-only fill still communicates "this is a premium"
  // via color even without the label.
  uint32_t glyph_codepoint = 0;
  uint32_t glyph_second = 0;
  if ((unsigned char)marker.glyph[0] < 0x80 &&
      (unsigned char)marker.glyph[1] < 0x80 && marker.glyph[0] != ' ' &&
      marker.glyph[0] != '\0') {
    glyph_codepoint = (uint32_t)marker.glyph[0];
    glyph_second = marker.glyph[1] != '\0' ? (uint32_t)marker.glyph[1] : 0;
  }
  if (glyph_codepoint != 0 && glyph_cache != NULL) {
    const int letter_px = (int)((double)tile_h * 0.50);
    tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
    if (glyph_second == 0) {
      const TuiGlyph *g = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
      if (g != NULL && g->width > 0 && g->height > 0) {
        blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w, tile_h, g,
                            marker.fg, marker.bg);
      }
    } else {
      const TuiGlyph *g1 = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
      const TuiGlyph *g2 = tui_glyph_cache_get(glyph_cache, glyph_second);
      if (g1 != NULL && g2 != NULL && g1->width > 0 && g2->width > 0) {
        blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w / 2, tile_h, g1,
                            marker.fg, marker.bg);
        blit_glyph_into_buf(buf, tile_w, tile_h, tile_w / 2, 0, tile_w / 2,
                            tile_h, g2, marker.fg, marker.bg);
      }
    }
  }

  // Bake the right + bottom border, identical to compose_tile_pixels.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int r = 0; r < tile_h; r++) {
      uint8_t *p = buf + ((size_t)r * tile_w + (tile_w - t)) * 4;
      for (int c = 0; c < t; c++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int r = tile_h - t; r < tile_h; r++) {
      uint8_t *p = buf + (size_t)r * tile_w * 4;
      for (int c = 0; c < tile_w; c++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }
  return buf;
}

// Compose ONE tile's RGBA buffer (bg color + letter glyph +
// optional score subscript). Caller owns the returned buffer
// and must free() it. Returns NULL on failure.
static uint8_t *
compose_tile_pixels(MachineLetter ml, int owner, bool blank_uppercase,
                    bool is_preview, int tile_w, int tile_h,
                    TuiGlyphCache *glyph_cache, TuiGlyphCache *glyph_cache_sub,
                    TuiScoreSubscripts score_subscripts, bool antialias,
                    int border_thickness, const Theme *theme,
                    const LetterDistribution *ld) {
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  const bool is_blank = get_is_blanked(ml);
  const bool render_uppercase = is_blank && blank_uppercase;
  const MachineLetter glyph_ml =
      render_uppercase ? get_unblanked_machine_letter(ml) : ml;
  // Preview tiles: muted player-hue tile under the bright player
  // hue letter (rendered through the bold glyph cache so the
  // outline is actually emboldened, not dilated). The bg is the
  // player's tile_fg color blended halfway with theme->bg so the
  // tile reads as a soft-tinted square instead of a fully
  // saturated one — enough hue to identify the player, low
  // enough contrast that the bold letter dominates.
  ThemeRgb bg;
  ThemeRgb fg;
  if (is_preview) {
    const ThemeRgb hue = owner == 1 ? theme->tile2_fg : theme->tile1_fg;
    ThemeRgb muted;
    muted.r = (uint8_t)((hue.r + theme->bg.r) / 2);
    muted.g = (uint8_t)((hue.g + theme->bg.g) / 2);
    muted.b = (uint8_t)((hue.b + theme->bg.b) / 2);
    bg = muted;
    fg = is_blank ? theme->blank_tile_fg : hue;
  } else {
    bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
    fg = is_blank ? theme->blank_tile_fg
                  : (owner == 1 ? theme->tile2_fg : theme->tile1_fg);
  }
  fill_tile_rect(buf, tile_w, 0, 0, tile_w, tile_h, bg);

  const int sub_mode = (int)score_subscripts;
  const bool subs_on =
      sub_mode != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  const char *ascii = ld->ld_ml_to_hl[glyph_ml];
  uint32_t glyph_codepoint = 0;
  if (ascii != NULL && ascii[0] != '\0' && (unsigned char)ascii[0] < 0x80) {
    glyph_codepoint = (uint32_t)ascii[0];
  }
  const int tile_score = equity_to_int(ld_get_score(ld, ml));
  const bool show_subscript =
      subs_on && (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL || tile_score != 0);

  if (glyph_codepoint != 0) {
    // Preview tiles pull from the bold cache so FreeType emboldens the
    // outline before rasterization — that gives a real bold stroke
    // weight, not a bitmap dilation of the regular glyph.
    const TuiGlyph *g =
        is_preview ? tui_glyph_cache_get_bold(glyph_cache, glyph_codepoint)
                   : tui_glyph_cache_get(glyph_cache, glyph_codepoint);
    if (subs_on && g != NULL && g->width > 0 && g->height > 0) {
      const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
      const int shift_x = (int)((double)tile_w * shift_x_frac);
      const int shift_y = (int)((double)tile_h * 0.08);
      const int baseline = (int)(tile_h * 0.72) - shift_y;
      const int glyph_top = baseline - g->bearing_y;
      const int glyph_left = (tile_w - g->width) / 2 - shift_x;
      blit_glyph_at_bold(buf, tile_w, tile_h, glyph_left, glyph_top, g, fg, bg,
                         /*bold=*/false);
    } else {
      blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w, tile_h, g, fg, bg);
    }
  }

  if (show_subscript) {
    char digits[8];
    snprintf(digits, sizeof(digits), "%d", tile_score);
    const int margin_x = (int)((double)tile_w * 0.12);
    const int margin_y = (int)((double)tile_h * 0.16);
    const int digit_bottom = tile_h - margin_y;
    int pen_right = tile_w - margin_x;
    for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
      const TuiGlyph *gd =
          is_preview
              ? tui_glyph_cache_get_bold(glyph_cache_sub, (uint32_t)digits[i])
              : tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
      if (gd == NULL || gd->width <= 0) {
        continue;
      }
      const int gleft = pen_right - gd->width;
      const int gtop = digit_bottom - gd->height;
      blit_glyph_at_bold(buf, tile_w, tile_h, gleft, gtop, gd, fg, bg,
                         /*bold=*/false);
      pen_right = gleft - 1;
    }
  }

  // Bake the grid lines into the tile itself: paint the right N
  // columns and bottom N rows in theme->bg. Adjacent tiles'
  // edges combine into a continuous grid, and there's no z-order
  // dependency on a separate overlay plane.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int row = 0; row < tile_h; row++) {
      uint8_t *p = buf + ((size_t)row * tile_w + (tile_w - t)) * 4;
      for (int col = 0; col < t; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int row = tile_h - t; row < tile_h; row++) {
      uint8_t *p = buf + (size_t)row * tile_w * 4;
      for (int col = 0; col < tile_w; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }

  return buf;
}

// Layered 2x board render. The cell backgrounds + premium labels
// go on the std plane as text (fast), and each placed tile gets
// its own small pixel plane (~6KB Kitty graphics payload, vs
// ~1.4MB for the previous single full-board plane). A per-tile
// cache means cursor scrolling through history snapshots only
// touches the tiles that actually differ between snapshots.
static void render_board_pixel(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL) {
    return;
  }
  // Probe the cell-pixel geometry. We need a child plane to ask
  // ncplane_pixel_geom about pixel ratios; reuse the first cached
  // tile plane if one exists, or make a tiny scratch plane.
  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned cdy = 0;
  unsigned cdx = 0;
  unsigned mby = 0;
  unsigned mbx = 0;
  ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }

  const int tile_w = (int)cdx * L->board_cell_w;
  const int tile_h = (int)cdy * L->board_cell_h;
  if (tile_w <= 0 || tile_h <= 0) {
    return;
  }

  const Board *board = pick_render_board(state);
  if (board == NULL) {
    return;
  }
  const int cursor_key = state->history_cursor;
  if (g_last_blit_tracked && cursor_key != g_last_blitted_cursor &&
      !g_cursor_pending) {
    clock_gettime(CLOCK_MONOTONIC, &g_cursor_pending_since);
    g_cursor_pending = true;
  }

  MachineLetter preview_letters[BOARD_DIM][BOARD_DIM];
  int preview_owner = 0;
  fill_preview_map(state, board, preview_letters, &preview_owner);

  bool any_blit = false;
  long total_blit_us = 0;
  int tile_blits = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter board_ml = board_get_letter(board, row, col);
      const MachineLetter preview_ml = preview_letters[row][col];
      const bool is_preview =
          preview_ml != 0 && board_ml == ALPHABET_EMPTY_SQUARE_MARKER;
      // Effective tile content. Cells with no tile and no preview
      // get rendered as premium / empty (per-cell pixel plane, not
      // an early-return drop). That's the whole point of this
      // refactor — uniform per-cell pixel planes so the grid
      // borders are consistent across cell types.
      const MachineLetter ml = is_preview
                                   ? preview_ml
                                   : (board_ml == ALPHABET_EMPTY_SQUARE_MARKER
                                          ? ALPHABET_EMPTY_SQUARE_MARKER
                                          : board_ml);
      const bool is_tile = (ml != ALPHABET_EMPTY_SQUARE_MARKER);
      TileCache *tc = &board_tile_cache[row][col];
      int owner = 0;
      int tile_score = 0;
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      if (is_tile) {
        owner = is_preview ? preview_owner
                           : board_get_square_owner(board, row, col);
        tile_score = equity_to_int(ld_get_score(state->ld, ml));
      }
      // Cache hit: cell's pixel plane already shows the right
      // content. Skip the rasterize + blit. The plane is sticky on
      // the screen until something invalidates it. Key now includes
      // bs + premium_labels for the empty / premium path.
      if (tc->valid && tc->letter == (int)ml && tc->owner == owner &&
          tc->score == tile_score &&
          tc->blank_uppercase == state->blank_uppercase &&
          tc->antialias == state->antialias && tc->is_preview == is_preview &&
          tc->score_subscripts == (int)state->score_subscripts &&
          tc->border_thickness == state->border_thickness && tc->cdy == cdy &&
          tc->cdx == cdx && tc->scale == L->scale && tc->bs == (int)bs.raw &&
          tc->premium_labels == (int)state->premium_labels &&
          board_tile_planes[row][col] != NULL) {
        continue;
      }
      // Ensure a plane exists at the tile's screen position.
      const int screen_top = CELL_ROW_BASE + row * L->board_cell_h;
      const int screen_left = CELL_COL_BASE + col * L->board_cell_w;
      if (board_tile_planes[row][col] == NULL) {
        ncplane_options opts = {0};
        opts.y = screen_top;
        opts.x = screen_left;
        opts.rows = (unsigned)L->board_cell_h;
        opts.cols = (unsigned)L->board_cell_w;
        opts.name = "tile";
        board_tile_planes[row][col] = ncplane_create(plane, &opts);
        if (board_tile_planes[row][col] == NULL) {
          continue;
        }
      } else {
        ncplane_move_yx(board_tile_planes[row][col], screen_top, screen_left);
      }
      uint8_t *buf = NULL;
      if (is_tile) {
        buf = compose_tile_pixels(
            ml, owner, state->blank_uppercase, is_preview, tile_w, tile_h,
            state->glyph_cache, state->glyph_cache_sub, state->score_subscripts,
            state->antialias, state->border_thickness, theme, state->ld);
      } else {
        buf = compose_premium_pixels(bs, row, col, tile_w, tile_h,
                                     state->glyph_cache, L->board_cell_w,
                                     state->premium_labels, state->antialias,
                                     state->border_thickness, theme);
      }
      if (buf == NULL) {
        continue;
      }
      struct ncvisual_options vopts = {0};
      vopts.n = board_tile_planes[row][col];
      vopts.blitter = NCBLIT_PIXEL;
      vopts.leny = (unsigned)tile_h;
      vopts.lenx = (unsigned)tile_w;
      struct timespec blit_start;
      clock_gettime(CLOCK_MONOTONIC, &blit_start);
      ncblit_rgba(buf, tile_w * 4, &vopts);
      tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
      struct timespec blit_end;
      clock_gettime(CLOCK_MONOTONIC, &blit_end);
      total_blit_us += (long)(blit_end.tv_sec - blit_start.tv_sec) * 1000000L +
                       (long)(blit_end.tv_nsec - blit_start.tv_nsec) / 1000L;
      free(buf);
      any_blit = true;
      tile_blits++;
      tc->letter = (int)ml;
      tc->owner = owner;
      tc->score = tile_score;
      tc->blank_uppercase = state->blank_uppercase;
      tc->antialias = state->antialias;
      tc->score_subscripts = (int)state->score_subscripts;
      tc->border_thickness = state->border_thickness;
      tc->is_preview = is_preview;
      tc->cdy = cdy;
      tc->cdx = cdx;
      tc->scale = L->scale;
      tc->bs = (int)bs.raw;
      tc->premium_labels = (int)state->premium_labels;
      tc->valid = true;
    }
  }

  if (any_blit) {
    atomic_store(&g_ncblit_us, total_blit_us);
    atomic_store(&g_last_tile_blits, tile_blits);
    if (g_cursor_pending) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long us =
          (long)(now.tv_sec - g_cursor_pending_since.tv_sec) * 1000000L +
          (long)(now.tv_nsec - g_cursor_pending_since.tv_nsec) / 1000L;
      atomic_store(&g_board_blit_latency_us, us);
      g_cursor_pending = false;
    }
    g_last_blitted_cursor = cursor_key;
    g_last_blit_tracked = true;
  }
}

// Renders the cell-text backdrop for the 2x board: premium markers
// on empty cells, plain tile-bg color on placed cells (the per-
// tile pixel planes draw the letter + subscript on top). Doing
// the backdrop in cells instead of in the pixel plane is what
// keeps Kitty graphics payloads tiny.
//
// Legacy text-mode board backdrop. Superseded at scale 2 by the
// per-cell pixel planes (compose_premium_pixels for empties /
// premiums, compose_tile_pixels for placed tiles), which produce
// uniformly-baked borders. Kept around for reference / future
// scale-0/1 work; cast to void in the caller so the compiler
// stops complaining about it being unused.
__attribute__((unused)) static void
render_board_text_bg(struct ncplane *plane, const Theme *theme,
                     const Board *board, const LetterDistribution *ld,
                     TuiPremiumLabels premium_labels, int cell_w, int cell_h,
                     int top, int left) {
  (void)ld;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int screen_row = top + row * cell_h;
      const int screen_col = left + col * cell_w;
      const MachineLetter ml = board_get_letter(board, row, col);
      ThemeRgb fg;
      ThemeRgb bg;
      const char *glyph;
      bool glyph_fullwidth;
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        // premium_marker_for_cell wants cell_w of 1 for halfwidth
        // glyphs, anything else for fullwidth. Map our 4-wide cell
        // back to 2 (fullwidth) so we get the same marker glyph the
        // 1x mode uses.
        const PremiumMarker marker = premium_marker_for_cell(
            theme, board_get_bonus_square(board, row, col), row, col,
            premium_labels, cell_w == 1 ? 1 : 2);
        fg = marker.fg;
        bg = marker.bg;
        glyph = marker.glyph;
        glyph_fullwidth = (cell_w != 1);
      } else {
        const int owner = board_get_square_owner(board, row, col);
        const ThemeRgb tile_bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
        fg = tile_bg;
        bg = tile_bg;
        glyph = (cell_w == 1) ? " " : "\xe3\x80\x80"; // U+3000
        glyph_fullwidth = (cell_w != 1);
      }
      // Row 0: glyph, then bg-colored halfwidth padding to fill
      // the rest of the cell's width.
      theme_apply_fg(plane, fg);
      theme_apply_bg(plane, bg);
      ncplane_putstr_yx(plane, screen_row, screen_col, glyph);
      const int glyph_cells = glyph_fullwidth ? 2 : 1;
      theme_apply_fg(plane, bg);
      theme_apply_bg(plane, bg);
      for (int c = glyph_cells; c < cell_w; c++) {
        ncplane_putstr_yx(plane, screen_row, screen_col + c, " ");
      }
      // Remaining rows: solid bg, halfwidth spaces all the way
      // across the cell.
      for (int r = 1; r < cell_h; r++) {
        for (int c = 0; c < cell_w; c++) {
          ncplane_putstr_yx(plane, screen_row + r, screen_col + c, " ");
        }
      }
    }
  }
}

// Pixel-graphics grid overlay for the cell-text modes (scale 0 / 1).
// 2x bakes its grid into the same RGBA buffer that holds the tiles;
// 0x and 1x have their tiles in std plane cells, so the grid lives in
// a separate transparent-base pixel plane stacked on top — same look
// the 2x composite delivers, just delivered through a child plane.
static void render_board_grid_overlay(struct ncplane *parent,
                                      const Theme *theme, const Layout *L,
                                      int thickness, uint64_t render_version) {
  struct notcurses *nc = ncplane_notcurses(parent);
  if (nc == NULL || !notcurses_canpixel(nc) || thickness <= 0) {
    return;
  }
  struct ncplane *p = acquire_grid_plane(
      &grid_planes.board, parent, "board_grid_overlay", CELL_ROW_BASE,
      CELL_COL_BASE, BOARD_DIM * L->board_cell_h, BOARD_DIM * L->board_cell_w);
  if (p == NULL) {
    return;
  }
  // Don't force a z-order. The grid plane was created (via
  // acquire_grid_plane) on the first frame this function ran;
  // by ncplane_create semantics it sat at the top of the pile
  // then. Per-tile planes are created later by render_board_pixel,
  // landing above the grid plane in z-order — so tile sprixels
  // naturally cover the grid plane where tiles exist, and the
  // grid sprixel shows in empty / premium cells where no tile
  // plane sits on top. Calling move_top would re-block tiles;
  // calling move_bottom sinks the plane below std and the
  // pixel content stops rendering.
  unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
  ncplane_pixel_geom(p, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }
  // Cache: key off cell dims + scale + thickness. The grid overlay's
  // pixels are a pure function of those, NOT of render_version — so a
  // bot move shouldn't force a re-blit. Kitty graphics treats each
  // blit as delete+create, and the brief gap can flash on screen.
  // Shared board_pixel_cache because grid_planes.board is reused for
  // either the 2x composite or this 1x/0x overlay; the scale field
  // (param_a) discriminates between modes when switching back.
  (void)render_version;
  if (board_pixel_cache.valid && board_pixel_cache.cdy == cdy &&
      board_pixel_cache.cdx == cdx && board_pixel_cache.param_a == L->scale &&
      board_pixel_cache.param_b == thickness) {
    return;
  }
  const int tile_w = (int)cdx * L->board_cell_w;
  const int tile_h = (int)cdy * L->board_cell_h;
  const int buf_w = tile_w * BOARD_DIM;
  const int buf_h = tile_h * BOARD_DIM;
  if (buf_w <= 0 || buf_h <= 0) {
    return;
  }
  // calloc gives us alpha=0 everywhere by default; overlay_grid_lines
  // only writes opaque theme->bg pixels along the bottom + right edges
  // of each tile. Cells underneath show through the alpha=0 regions.
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
  if (buf == NULL) {
    return;
  }
  overlay_grid_lines(buf, buf_w, buf_h, BOARD_DIM, BOARD_DIM, tile_h, tile_w,
                     thickness, theme->bg);

  struct ncvisual_options vopts = {0};
  vopts.n = p;
  vopts.blitter = NCBLIT_PIXEL;
  vopts.leny = (unsigned)buf_h;
  vopts.lenx = (unsigned)buf_w;
  ncblit_rgba(buf, buf_w * 4, &vopts);
  tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
  free(buf);

  board_pixel_cache.valid = true;
  board_pixel_cache.version = 0; // unused for the overlay path
  board_pixel_cache.cdy = cdy;
  board_pixel_cache.cdx = cdx;
  board_pixel_cache.param_a = L->scale;
  board_pixel_cache.param_b = thickness;
}

// 2x mode coordinate labels (Ａ-Ｏ above the board, 1-15 to its left).
// Drawn as pixels so a single-cell-tall glyph can be vertically
// centered against the two-cell-tall board rows. Each label lives in a
// 2×1 cell box (squarish in pixels, same footprint as a fullwidth letter
// or a two-digit ASCII number).
static void render_board_labels_pixel(struct ncplane *plane, const Theme *theme,
                                      const TuiGameState *state,
                                      const Layout *L) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache_sub == NULL) {
    return;
  }
  const int col_rows = 1;
  const int col_cols = BOARD_DIM * L->board_cell_w;
  const int row_rows = BOARD_DIM * L->board_cell_h;
  const int row_cols = CELL_COL_BASE;

  struct ncplane *col_p =
      acquire_grid_plane(&grid_planes.labels_col, plane, "board_col_labels",
                         COL_LABELS_ROW, CELL_COL_BASE, col_rows, col_cols);
  struct ncplane *row_p =
      acquire_grid_plane(&grid_planes.labels_row, plane, "board_row_labels",
                         CELL_ROW_BASE, 0, row_rows, row_cols);
  if (col_p == NULL || row_p == NULL) {
    return;
  }
  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned cdy = 0;
  unsigned cdx = 0;
  unsigned mby = 0;
  unsigned mbx = 0;
  ncplane_pixel_geom(col_p, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }

  // Labels are static text (Ａ-Ｏ + 1-15) — they don't depend on the
  // game state, only on cell dims + scale + antialias. Drop the version
  // dependency to avoid the kitty-graphics flash that comes with each
  // re-blit.
  if (label_pixel_cache.valid && label_pixel_cache.cdy == cdy &&
      label_pixel_cache.cdx == cdx && label_pixel_cache.param_a == L->scale &&
      label_pixel_cache.param_b == (state->antialias ? 1 : 0)) {
    return;
  }

  // Pick glyph pixel size to nearly fill the cell-tall box, but cap by
  // width so a 2-digit number ("15") still fits inside the 2×cdx-wide
  // box. Monospace glyph advance is roughly 0.6× the em size at the
  // fonts we ship.
  int label_px = (int)((double)cdy * 0.80);
  const int max_by_width = (int)((double)cdx * 1.5);
  if (label_px > max_by_width) {
    label_px = max_by_width;
  }
  if (label_px < 1) {
    label_px = 1;
  }
  tui_glyph_cache_set_size(state->glyph_cache_sub, label_px, state->antialias);

  const ThemeRgb fg = theme->dim_fg;
  const ThemeRgb bg = theme->bg;
  const int icdy = (int)cdy;
  const int icdx = (int)cdx;

  // Column labels: each letter centered in a 2×cdx box, which itself
  // is centered in the cell's L->board_cell_w columns.
  {
    const int buf_w = col_cols * icdx;
    const int buf_h = col_rows * icdy;
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
    if (buf == NULL) {
      return;
    }
    fill_tile_rect(buf, buf_w, 0, 0, buf_w, buf_h, bg);
    for (int col = 0; col < BOARD_DIM; col++) {
      const char ch = (char)('A' + col);
      const TuiGlyph *g =
          tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)ch);
      if (g == NULL || g->width <= 0 || g->height <= 0) {
        continue;
      }
      const int cell_left = col * L->board_cell_w * icdx;
      const int cell_w_px = L->board_cell_w * icdx;
      const int glyph_left = cell_left + (cell_w_px - g->width) / 2;
      // Baseline ~78% of the way down a 1-cell box leaves enough room
      // for the cap-height of Latin letters.
      const int baseline = (int)(cdy * 0.78);
      const int glyph_top = baseline - g->bearing_y;
      blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
    }
    struct ncvisual_options vopts = {0};
    vopts.n = col_p;
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)buf_h;
    vopts.lenx = (unsigned)buf_w;
    ncblit_rgba(buf, buf_w * 4, &vopts);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
  }

  // Row labels: "%2d" right-anchored against the board, with a margin
  // matching the col labels' bottom margin (the gap between an "A" and
  // the tile beneath). Both margins are a fixed fraction of cdy so they
  // scale with the board. Digits render slightly smaller than the col
  // labels so they leave room for that right margin.
  const int row_label_px = (int)((double)cdy * 0.65);
  tui_glyph_cache_set_size(state->glyph_cache_sub,
                           row_label_px > 0 ? row_label_px : 1,
                           state->antialias);
  {
    const int buf_w = row_cols * icdx;
    const int buf_h = row_rows * icdy;
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
    if (buf == NULL) {
      return;
    }
    // Leave the buffer's background transparent (calloc gave us
    // alpha=0 everywhere). The pixel plane then composites on top of
    // the text plane, so the box's left border `│` shows through for
    // the rows that need it — only the digit pixels mask the border.
    const int right_margin = (int)((double)cdy * 0.18);
    const int content_right = buf_w - right_margin;
    const int inter_digit_gap = (int)((double)cdy * 0.05);
    for (int row = 0; row < BOARD_DIM; row++) {
      char label[4];
      snprintf(label, sizeof(label), "%2d", row + 1);
      const int cell_top = row * L->board_cell_h * icdy;
      const int cell_h_px = L->board_cell_h * icdy;
      // Center a 1-row-tall label box vertically in the 2-row cell.
      const int box_top = cell_top + (cell_h_px - icdy) / 2;
      const int baseline = box_top + (int)(cdy * 0.78);
      // Pack digits flush right with a small fixed gap, right-to-left.
      // Single-digit rows (1..9) have the leading char as ' '; skip it
      // and only render the trailing digit at the rightmost slot.
      int right_edge = content_right;
      for (int i = 1; i >= 0; i--) {
        const char ch = label[i];
        if (ch == '\0' || ch == ' ') {
          continue;
        }
        const TuiGlyph *g =
            tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)ch);
        if (g == NULL || g->width <= 0 || g->height <= 0) {
          continue;
        }
        const int glyph_left = right_edge - g->width;
        const int glyph_top = baseline - g->bearing_y;
        blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
        right_edge = glyph_left - inter_digit_gap;
      }
    }
    struct ncvisual_options vopts = {0};
    vopts.n = row_p;
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)buf_h;
    vopts.lenx = (unsigned)buf_w;
    ncblit_rgba(buf, buf_w * 4, &vopts);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
  }

  label_pixel_cache.valid = true;
  label_pixel_cache.version = 0; // unused for the labels path
  label_pixel_cache.cdy = cdy;
  label_pixel_cache.cdx = cdx;
  label_pixel_cache.param_a = L->scale;
  label_pixel_cache.param_b = state->antialias ? 1 : 0;
}

// Count of letters currently on the given board (regular + blanks).
// Pass NULL to get 0 — callers route the result of pick_render_board
// here so the count tracks whatever the user is currently viewing
// (live game vs. a history-cursor snapshot).
static int board_tile_count(const Board *board) {
  if (board == NULL) {
    return 0;
  }
  int count = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_get_letter(board, row, col) != ALPHABET_EMPTY_SQUARE_MARKER) {
        count++;
      }
    }
  }
  return count;
}

// Draw the board widget's surrounding box. Title is "Board (N)" where
// N is the current tile count. The box spans rows 0..board_bottom+1
// (top border to bottom border) and cols 0..board_width-1.
static void render_board_box(struct ncplane *plane, const Theme *theme,
                             const TuiGameState *state, const Layout *L) {
  const int height = L->board_bottom_row + 2; // top border .. bottom border
  char title[32];
  snprintf(title, sizeof(title), "Board (%d)",
           board_tile_count(pick_render_board(state)));
  const bool focused = state->focused_panel == TUI_FOCUS_BOARD;
  draw_box_styled(plane, theme, 0, 0, height, L->board_width, title,
                  TUI_FOCUS_BOARD, focused);
}

static void render_board(struct ncplane *plane, const Theme *theme,
                         const TuiGameState *state, const Layout *L) {
  render_board_box(plane, theme, state, L);
  if (L->scale >= 2 && state->glyph_cache != NULL) {
    render_board_labels_pixel(plane, theme, state, L);
    // Every cell — placed tile, premium, plain empty — gets its
    // own per-cell pixel plane via render_board_pixel. Each plane
    // bakes its right + bottom theme->bg border, so the grid is
    // uniform across cell types. No text-mode bg, no overlay
    // plane: there's exactly one render path and one source of
    // border pixels per cell. Caching (TileCache) skips planes
    // whose content + geometry haven't changed, so a 60fps idle
    // frame re-blits zero cells.
    if (grid_planes.board != NULL) {
      ncplane_destroy(grid_planes.board);
      grid_planes.board = NULL;
      board_pixel_cache.valid = false;
    }
    render_board_pixel(plane, theme, state, L);
    return;
  }
  // 2x-only pixel planes are no-ops at scale < 2 but their stale
  // image data would otherwise sit on top of the text-mode layout.
  // Drop the cached board grid-overlay plane, the labels, AND any
  // per-tile pixel planes from the layered 2x renderer.
  if (grid_planes.board != NULL) {
    ncplane_destroy(grid_planes.board);
    grid_planes.board = NULL;
    board_pixel_cache.valid = false;
  }
  if (grid_planes.labels_col != NULL) {
    ncplane_destroy(grid_planes.labels_col);
    grid_planes.labels_col = NULL;
    label_pixel_cache.valid = false;
  }
  if (grid_planes.labels_row != NULL) {
    ncplane_destroy(grid_planes.labels_row);
    grid_planes.labels_row = NULL;
    label_pixel_cache.valid = false;
  }
  invalidate_tile_planes();
  // Column labels: fullwidth Ａ-Ｏ at scale 1, halfwidth A-O at scale 0.
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  if (L->board_cell_w == 1) {
    for (int col = 0; col < BOARD_DIM; col++) {
      char ch[2] = {(char)('A' + col), '\0'};
      ncplane_putstr_yx(plane, COL_LABELS_ROW, CELL_COL_BASE + col, ch);
    }
  } else {
    for (int col = 0; col < BOARD_DIM; col++) {
      ncplane_putstr_yx(plane, COL_LABELS_ROW,
                        CELL_COL_BASE + col * L->board_cell_w,
                        fullwidth_col_labels[col]);
    }
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    char label[4];
    snprintf(label, sizeof(label), "%2d", row + 1);
    ncplane_putstr_yx(plane, CELL_ROW_BASE + row, ROW_LABEL_COL, label);
  }
  const Board *brd = pick_render_board(state);
  render_board_cells(plane, theme, brd, state->ld, state->blank_uppercase,
                     state->premium_labels, state->border_thickness,
                     L->board_cell_w, CELL_ROW_BASE, CELL_COL_BASE);
  // Analysis-cursored candidate overlay: paint the proposed
  // tiles on top of empty cells in a muted player-hue palette
  // (matches the 2x pixel-mode preview look, just delivered via
  // text cells).
  if (brd != NULL) {
    MachineLetter preview_letters[BOARD_DIM][BOARD_DIM];
    int preview_owner = 0;
    fill_preview_map(state, brd, preview_letters, &preview_owner);
    const ThemeRgb hue = preview_owner == 1 ? theme->tile2_fg : theme->tile1_fg;
    ThemeRgb muted;
    muted.r = (uint8_t)((hue.r + theme->bg.r) / 2);
    muted.g = (uint8_t)((hue.g + theme->bg.g) / 2);
    muted.b = (uint8_t)((hue.b + theme->bg.b) / 2);
    const bool halfwidth = (L->board_cell_w == 1);
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        const MachineLetter ml = preview_letters[row][col];
        if (ml == 0) {
          continue;
        }
        theme_apply_fg(plane, hue);
        theme_apply_bg(plane, muted);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        const int screen_row = CELL_ROW_BASE + row;
        const int screen_col = CELL_COL_BASE + col * L->board_cell_w;
        if (halfwidth) {
          const char *ascii = (ml == 0) ? "?" : state->ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, screen_row, screen_col,
                            ascii[0] != '\0' ? ascii : " ");
        } else {
          const char *fullwidth = state->ld->ld_ml_to_alt_hl[ml];
          if (fullwidth[0] != '\0') {
            ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
          } else {
            const char *ascii = (ml == 0) ? "?" : state->ld->ld_ml_to_hl[ml];
            ncplane_putstr_yx(plane, screen_row, screen_col, " ");
            ncplane_putstr(plane, ascii);
          }
        }
        ncplane_set_styles(plane, 0);
      }
    }
  }
}

void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const Game *game,
                         const LetterDistribution *ld, bool blank_uppercase,
                         TuiPremiumLabels premium_labels,
                         int border_thickness) {
  if (plane == NULL || theme == NULL || game == NULL || ld == NULL) {
    return;
  }
  // Preview is always fullwidth (cell_w=2); the picker is shown on a
  // dedicated screen where halfwidth fallback isn't relevant.
  render_board_cells(plane, theme, game_get_board(game), ld, blank_uppercase,
                     premium_labels, border_thickness, CELL_WIDTH, top, left);
}

// ── Rack panel ────────────────────────────────────────────────────────────
//
// Rack tiles scale alongside the board: at scale=2 we composite an RGBA
// strip via FreeType (same path as the board); at scale=1 we use
// fullwidth Unicode glyphs in cells; at scale=0 we collapse to single
// ASCII chars. The panel box gains one row at scale=2 so the 2-row
// tiles fit.

// Compose ONE rack tile's RGBA buffer (rack_tileN bg + letter
// glyph + optional score subscript). Caller owns + frees the
// buffer. Returns NULL on failure. The rack uses its own
// palette (rack_tile1_*, rack_tile2_*) rather than the board's
// tile colors; blanks render as "?" with subscript 0.
static uint8_t *compose_rack_tile_pixels(MachineLetter ml, int player_idx,
                                         bool ghost, int tile_w, int tile_h,
                                         TuiGlyphCache *glyph_cache,
                                         TuiGlyphCache *glyph_cache_sub,
                                         TuiScoreSubscripts score_subscripts,
                                         bool antialias, const Theme *theme,
                                         const LetterDistribution *ld) {
  // Supersample: rasterize the glyph + composite the tile at SSx
  // the target pixel size, then box-average 2x2 down to the cell
  // pixel size the terminal expects. Sidesteps FT hinting that
  // snaps stems to integer pixels badly at certain sizes
  // (cdy=27/30 looked blocky, cdy=32/34 looked clean) — by
  // rendering at 2*letter_px, hints and rasterization quirks
  // happen on a finer grid, then the average yields smooth
  // sub-pixel-positioned strokes regardless of the target size.
  const int SS = 2;
  const int tw = tile_w * SS;
  const int th = tile_h * SS;
  uint8_t *buf_ss = (uint8_t *)calloc(1, (size_t)tw * th * 4);
  if (buf_ss == NULL) {
    return NULL;
  }
  // Ghosted rack tiles: the slot's letter is being projected onto
  // the board by the currently-previewed move. Render the slot
  // as a neutral gray-on-bg ghost (no player-hue tile, no score
  // subscript) so the rack visually communicates "this tile is
  // about to be spent" while keeping the slot's physical
  // position occupied.
  const ThemeRgb bg =
      ghost ? theme->bg
            : (player_idx == 1 ? theme->rack_tile2_bg : theme->rack_tile1_bg);
  const ThemeRgb fg =
      ghost ? (ThemeRgb){.r = 102, .g = 102, .b = 102}
            : (player_idx == 1 ? theme->rack_tile2_fg : theme->rack_tile1_fg);
  fill_tile_rect(buf_ss, tw, 0, 0, tw, th, bg);

  const int sub_mode = (int)score_subscripts;
  const bool subs_on =
      sub_mode != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)th * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)th * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
  const TuiGlyph *g =
      (ascii != NULL && ascii[0] != '\0' && (unsigned char)ascii[0] < 0x80)
          ? tui_glyph_cache_get(glyph_cache, (uint32_t)ascii[0])
          : NULL;
  const int tile_score = (ml == 0) ? 0 : equity_to_int(ld_get_score(ld, ml));
  if (g != NULL && g->width > 0 && g->height > 0) {
    if (subs_on) {
      const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
      const int shift_x = (int)((double)tw * shift_x_frac);
      const int shift_y = (int)((double)th * 0.08);
      const int baseline = (int)(th * 0.72) - shift_y;
      const int glyph_top = baseline - g->bearing_y;
      const int glyph_left = (tw - g->width) / 2 - shift_x;
      blit_glyph_at(buf_ss, tw, th, glyph_left, glyph_top, g, fg, bg);
    } else {
      blit_glyph_into_buf(buf_ss, tw, th, 0, 0, tw, th, g, fg, bg);
    }
  }
  if (subs_on && !ghost) {
    // Blanks always get a "0" subscript so their value is explicit.
    const bool show_subscript = (ml == 0) ||
                                (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL) ||
                                (tile_score != 0);
    if (show_subscript) {
      char digits[8];
      snprintf(digits, sizeof(digits), "%d", tile_score);
      const int margin_x = (int)((double)tw * 0.12);
      const int margin_y = (int)((double)th * 0.16);
      const int digit_bottom = th - margin_y;
      int pen_right = tw - margin_x;
      for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
        const TuiGlyph *gd =
            tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
        if (gd == NULL || gd->width <= 0) {
          continue;
        }
        const int gleft = pen_right - gd->width;
        const int gtop = digit_bottom - gd->height;
        blit_glyph_at(buf_ss, tw, th, gleft, gtop, gd, fg, bg);
        pen_right = gleft - 1;
      }
    }
  }
  // Downsample SSxSS box average to (tile_w x tile_h). Alpha is
  // fully opaque everywhere in the supersample buffer, so we
  // skip averaging it.
  uint8_t *buf = (uint8_t *)malloc((size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    free(buf_ss);
    return NULL;
  }
  const int denom = SS * SS;
  for (int row = 0; row < tile_h; row++) {
    for (int col = 0; col < tile_w; col++) {
      uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
      for (int dy = 0; dy < SS; dy++) {
        const uint8_t *src_row =
            buf_ss + (size_t)((row * SS + dy) * tw + col * SS) * 4;
        for (int dx = 0; dx < SS; dx++) {
          const uint8_t *src = src_row + (size_t)dx * 4;
          r_sum += src[0];
          g_sum += src[1];
          b_sum += src[2];
        }
      }
      uint8_t *dst = buf + (size_t)(row * tile_w + col) * 4;
      dst[0] = (uint8_t)(r_sum / denom);
      dst[1] = (uint8_t)(g_sum / denom);
      dst[2] = (uint8_t)(b_sum / denom);
      dst[3] = 255;
    }
  }
  free(buf_ss);
  return buf;
}

static void render_rack_panel_pixel(struct ncplane *plane, const Theme *theme,
                                    const TuiGameState *state, const Layout *L,
                                    int start_col, int tile_count) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL) {
    return;
  }
  // When the rack drops to 0 tiles (e.g. P2 just went out), fall
  // through to the cleanup loop below so the previously-rendered
  // pixel planes get destroyed instead of sitting on screen
  // showing stale DI/etc tiles.
  if (tile_count <= 0) {
    for (int i = 0; i < RACK_SIZE; i++) {
      if (rack_tile_planes[i] != NULL) {
        ncplane_destroy(rack_tile_planes[i]);
        rack_tile_planes[i] = NULL;
        rack_tile_cache[i].valid = false;
      }
    }
    return;
  }
  const int cell_w = L->board_cell_w; // 4
  const int cell_h = L->board_cell_h; // 2
  // Probe cell-pixel geometry off the parent plane.
  unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
  ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }
  const int tile_w = (int)cdx * cell_w;
  const int tile_h = (int)cdy * cell_h;
  if (tile_w <= 0 || tile_h <= 0) {
    return;
  }

  // Expand the rack into a per-slot letter array so we can cache
  // each slot independently. tile_count comes from the caller and
  // is the visible rack length. Cursor-aware: the on-turn player
  // is resolved through pick_render_on_turn so navigating to a
  // committed turn rewinds the rack panel to that turn's player
  // + rack.
  const int player_idx = pick_render_on_turn(state);
  const Rack *rack = pick_render_rack(state, player_idx);
  MachineLetter slot_letters[RACK_SIZE];
  for (int i = 0; i < RACK_SIZE; i++) {
    slot_letters[i] = ALPHABET_EMPTY_SQUARE_MARKER;
  }
  const int slot_max = tile_count < RACK_SIZE ? tile_count : RACK_SIZE;
  int slot_idx = sort_rack_for_display(rack, state->ld, state->rack_sort,
                                       slot_letters, slot_max);
  bool slot_ghost[RACK_SIZE];
  compute_rack_ghost_mask(state, slot_letters, slot_idx, slot_ghost);
  for (int i = slot_idx; i < RACK_SIZE; i++) {
    slot_ghost[i] = false;
  }

  // Drop any cached tile plane beyond the visible rack length so
  // shrinking rack (end of game) frees its planes.
  for (int i = tile_count; i < RACK_SIZE; i++) {
    if (rack_tile_planes[i] != NULL) {
      ncplane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
      rack_tile_cache[i].valid = false;
    }
  }

  for (int i = 0; i < tile_count && i < RACK_SIZE; i++) {
    const MachineLetter ml = slot_letters[i];
    const bool ghost = slot_ghost[i];
    RackTileCache *rc = &rack_tile_cache[i];
    const int tile_score =
        (ml == 0) ? 0 : equity_to_int(ld_get_score(state->ld, ml));
    const int screen_top = L->rack_top + 1;
    const int screen_left = start_col + i * cell_w;
    // Pixel-mode terminals don't actually move sprixel pixels when
    // ncplane_move_yx is called — they only update notcurses'
    // internal plane state. If our slot needs to shift (because
    // the rack count changed and the rack is re-centered on the
    // board), the old sprixel content sits at the previous y/x
    // until we destroy + recreate the plane. So: any time the
    // cached screen position doesn't match the current one,
    // tear the plane down so we start fresh. The cache only
    // counts as "hit" when content AND position match.
    const bool same_pos = rc->valid && rack_tile_planes[i] != NULL &&
                          rc->screen_top == screen_top &&
                          rc->screen_left == screen_left;
    if (!same_pos && rack_tile_planes[i] != NULL) {
      ncplane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
      rc->valid = false;
    }
    if (rc->valid && rc->letter == (int)ml && rc->player_idx == player_idx &&
        rc->score == tile_score && rc->antialias == state->antialias &&
        rc->ghost == ghost &&
        rc->score_subscripts == (int)state->score_subscripts &&
        rc->cdy == cdy && rc->cdx == cdx && rc->scale == L->scale &&
        rack_tile_planes[i] != NULL) {
      continue;
    }
    if (rack_tile_planes[i] == NULL) {
      ncplane_options opts = {0};
      opts.y = screen_top;
      opts.x = screen_left;
      opts.rows = (unsigned)cell_h;
      opts.cols = (unsigned)cell_w;
      opts.name = "rack_tile";
      rack_tile_planes[i] = ncplane_create(plane, &opts);
      if (rack_tile_planes[i] == NULL) {
        continue;
      }
    }
    uint8_t *buf = compose_rack_tile_pixels(
        ml, player_idx, ghost, tile_w, tile_h, state->glyph_cache,
        state->glyph_cache_sub, state->score_subscripts, state->antialias,
        theme, state->ld);
    if (buf == NULL) {
      continue;
    }
    struct ncvisual_options vopts = {0};
    vopts.n = rack_tile_planes[i];
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)tile_h;
    vopts.lenx = (unsigned)tile_w;
    ncblit_rgba(buf, tile_w * 4, &vopts);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
    rc->letter = (int)ml;
    rc->player_idx = player_idx;
    rc->score = tile_score;
    rc->antialias = state->antialias;
    rc->ghost = ghost;
    rc->score_subscripts = (int)state->score_subscripts;
    rc->cdy = cdy;
    rc->cdx = cdx;
    rc->scale = L->scale;
    rc->screen_top = screen_top;
    rc->screen_left = screen_left;
    rc->valid = true;
  }
  // The legacy single-plane rack_pixel_cache is unused now; clear
  // it so a future regression that re-enables it starts cold
  // rather than reusing stale state.
  rack_pixel_cache.valid = false;
  if (grid_planes.rack != NULL) {
    ncplane_destroy(grid_planes.rack);
    grid_planes.rack = NULL;
  }
}

static void render_rack_panel(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L) {
  const int box_height = L->rack_bottom - L->rack_top + 1;
  // Follow the History cursor: when parked on a committed turn we
  // show that turn's player + rack, not the live game's on-turn
  // player. pick_render_rack falls through to live state when the
  // cursor is on the label or a pending entry.
  const int player_idx = pick_render_on_turn(state);
  const Rack *rack = pick_render_rack(state, player_idx);
  if (rack == NULL) {
    return;
  }
  char title[32];
  snprintf(title, sizeof(title), "Rack (%d)", rack_get_total_letters(rack));
  const bool rack_focused = state->focused_panel == TUI_FOCUS_RACK;
  draw_box_styled(plane, theme, L->rack_top, 0, box_height, L->board_width,
                  title, TUI_FOCUS_RACK, rack_focused);
  const LetterDistribution *ld = state->ld;

  const int cell_w = L->board_cell_w;
  const int total_letters = rack_get_total_letters(rack);
  const int rack_width = total_letters * cell_w;
  // Align the rack's center column with the board's middle column
  // (H8 in standard 15×15), not the rack panel's interior center.
  // Board cells span [CELL_COL_BASE, CELL_COL_BASE + BOARD_DIM*cell_w),
  // so the midpoint is CELL_COL_BASE + (BOARD_DIM*cell_w)/2. Centering
  // on the panel made the rack drift relative to the board because the
  // panel box starts at col 0 (not CELL_COL_BASE) — visibly off by 2
  // cols at fullwidth.
  const int board_center = CELL_COL_BASE + (BOARD_DIM * cell_w) / 2;
  int start_col = board_center - rack_width / 2;
  if (start_col < 1) {
    start_col = 1;
  }

  if (total_letters == 0) {
    // Idle / pre-game state: rack hasn't been drawn yet. Print a
    // single dim placeholder centered in the panel interior. The
    // 2x pixel path still gets called below so any leftover rack
    // pixel planes from a prior game get destroyed.
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const char *msg = "(no rack specified)";
    const int msg_col = 1 + (L->board_width - 2 - (int)strlen(msg)) / 2;
    ncplane_putstr_yx(plane, L->rack_top + 1, msg_col, msg);
  }

  if (L->scale >= 2 && state->glyph_cache != NULL) {
    render_rack_panel_pixel(plane, theme, state, L, start_col, total_letters);
    return;
  }

  // Drop the 2x-only rack pixel plane if we're not using it — stale
  // pixel content otherwise sits on top of the text-mode rack.
  if (grid_planes.rack != NULL) {
    ncplane_destroy(grid_planes.rack);
    grid_planes.rack = NULL;
    rack_pixel_cache.valid = false;
  }

  // Cell-text rack: row sits one below the box top. Halfwidth uses
  // single-char glyphs, fullwidth uses ld_ml_to_alt_hl.
  const bool halfwidth = (cell_w == 1);
  // Build slot_letters in the user's chosen display order, then
  // compute the ghost mask against it so previewed-move tiles
  // render gray in the same positions the pixel renderer uses.
  MachineLetter slot_letters[RACK_SIZE];
  const int slot_idx = sort_rack_for_display(rack, ld, state->rack_sort,
                                             slot_letters, RACK_SIZE);
  bool slot_ghost[RACK_SIZE];
  compute_rack_ghost_mask(state, slot_letters, slot_idx, slot_ghost);
  const ThemeRgb ghost_fg = {.r = 102, .g = 102, .b = 102};
  int col_offset = 0;
  for (int i = 0; i < slot_idx; i++) {
    const MachineLetter ml = slot_letters[i];
    {
      const bool is_ghost = slot_ghost[i];
      if (is_ghost) {
        theme_apply_fg(plane, ghost_fg);
        theme_apply_bg(plane, theme->bg);
      } else {
        theme_apply_fg(plane, player_idx == 1 ? theme->rack_tile2_fg
                                              : theme->rack_tile1_fg);
        theme_apply_bg(plane, player_idx == 1 ? theme->rack_tile2_bg
                                              : theme->rack_tile1_bg);
      }
      if (halfwidth) {
        // Halfwidth blank: just the "?" glyph. We used to splice a
        // zero-width non-joiner in between adjacent blanks to defeat
        // "??" font ligatures, but it caused the row containing
        // blanks to drop out on some terminals; the ligature is the
        // lesser evil.
        const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
        ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                          ascii[0] != '\0' ? ascii : " ");
      } else {
        const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                            fullwidth);
        } else {
          const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                            " ");
          ncplane_putstr(plane, ascii);
        }
      }
      col_offset += cell_w;
    }
  }
}

// ── Bag panel ─────────────────────────────────────────────────────────────
// Single-row divider used when the bag is empty: just a horizontal
// rule with " Bag (0) " inset near the left. Replaces the full 4-row
// box so endgames don't waste vertical space on a blank panel.
static void render_bag_divider(struct ncplane *plane, const Theme *theme,
                               int row, int left, int width, const char *title,
                               int hotkey, bool focused) {
  const ThemeRgb border_fg = focused ? theme->fg : theme->dim_fg;
  const ThemeRgb border_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  const char *hz = focused ? BOX2_HZ : BOX_HZ;
  theme_apply_fg(plane, border_fg);
  theme_apply_bg(plane, border_bg);
  const int right = left + width - 1;
  for (int col = left; col <= right; col++) {
    ncplane_putstr_yx(plane, row, col, hz);
  }
  if (title != NULL && title[0] != '\0') {
    int col = left + 1;
    if (hotkey > 0) {
      char buf[8];
      if (focused) {
        snprintf(buf, sizeof(buf), "[%d>", hotkey);
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->fg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      } else {
        snprintf(buf, sizeof(buf), "[%d]", hotkey);
        theme_apply_fg(plane, theme->modal_shortcut_fg);
        theme_apply_bg(plane, border_bg);
      }
      ncplane_putstr_yx(plane, row, col, buf);
      col += (int)strlen(buf);
      ncplane_set_styles(plane, 0);
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, border_bg);
      ncplane_putstr_yx(plane, row, col++, " ");
    }
    ncplane_putstr_yx(plane, row, col, title);
    col += (int)strlen(title);
    ncplane_putstr_yx(plane, row, col, " ");
  }
}

static void render_bag_panel(struct ncplane *plane, const Theme *theme,
                             const TuiGameState *state, const Layout *L) {
  // Bag inventory follows the history-cursor view, not the live
  // game state, so navigating through loaded turns shows what the
  // bag looked like at each point. The inventory we display is
  // "unseen by the on-turn player" = bag + opponent's rack, which
  // is identical to ld_total − board − on_turn_rack. Computing it
  // that way avoids needing per-turn bag snapshots.
  const LetterDistribution *ld = state->ld;
  const int ld_size = ld_get_size(ld);
  int counts[64] = {0};

  const Board *render_board = pick_render_board(state);
  const TuiHistoryEntry *hview = pick_history_view(state);
  const Rack *on_turn_rack = NULL;
  if (hview != NULL && hview->rack_before != NULL) {
    on_turn_rack = hview->rack_before;
  } else if (state->game != NULL) {
    on_turn_rack = player_get_rack(game_get_player(
        state->game, game_get_player_on_turn_index(state->game)));
  }

  if (render_board != NULL) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        MachineLetter ml = board_get_letter(render_board, row, col);
        if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
          continue;
        }
        // Played blanks consume a blank tile from the bag; map them
        // back to ml=0 for distribution accounting.
        if (ml & BLANK_MASK) {
          ml = 0;
        }
        if (ml < (int)(sizeof(counts) / sizeof(int))) {
          counts[ml]++;
        }
      }
    }
  }

  int bag_count = 0;
  for (int ml = 0; ml < ld_size && ml < (int)(sizeof(counts) / sizeof(int));
       ml++) {
    const int dist = ld_get_dist(ld, (MachineLetter)ml);
    const int on_rack = on_turn_rack != NULL
                            ? rack_get_letter(on_turn_rack, (MachineLetter)ml)
                            : 0;
    // ld_total − on_board − on_turn_rack. Clamp at 0 in case the
    // snapshot data is slightly inconsistent (defensive).
    int v = dist - counts[ml] - on_rack;
    if (v < 0) {
      v = 0;
    }
    counts[ml] = v;
    bag_count += v;
  }

  char title[32];
  snprintf(title, sizeof(title), "Bag (%d)", bag_count);

  // Empty bag: skip the box, the tile listing, and the tally line.
  // The endgame UI doesn't need any of that — opponent's tiles are
  // already in the P2 pill.
  if (bag_count == 0) {
    const bool empty_focused = state->focused_panel == TUI_FOCUS_BAG;
    render_bag_divider(plane, theme, L->bag_top, 0, L->board_width, title,
                       TUI_FOCUS_BAG, empty_focused);
    return;
  }

  const int height = L->bag_bottom - L->bag_top + 1;
  if (height < 3) {
    return;
  }
  const bool bag_focused = state->focused_panel == TUI_FOCUS_BAG;
  draw_box_styled(plane, theme, L->bag_top, 0, height, L->board_width, title,
                  TUI_FOCUS_BAG, bag_focused);

  // Build the dense inline "?? AAAAAAA BB ..." string. The walk
  // order matches the user's rack_sort preference so the bag
  // listing follows the same grouping as their tiles in the rack:
  //   ALPHA          → A..Z then ?
  //   BLANKS_ALPHA   → ? then A..Z
  //   VOWELS         → vowels, consonants, ?
  //   BLANKS_VOWELS  → ?, vowels, consonants
  // Build the order as a sequence of ml indices (with one slot
  // for the blank) and then walk it.
  int ml_order[64];
  int ml_order_n = 0;
  switch (state->rack_sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    for (int ml = 0; ml < ld_size && ml_order_n < 64; ml++) {
      ml_order[ml_order_n++] = ml;
    }
    break;
  case TUI_RACK_SORT_BLANKS_VOWELS:
    if (ml_order_n < 64) {
      ml_order[ml_order_n++] = BLANK_MACHINE_LETTER;
    }
    for (int ml = 1; ml < ld_size && ml_order_n < 64; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        ml_order[ml_order_n++] = ml;
      }
    }
    for (int ml = 1; ml < ld_size && ml_order_n < 64; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        ml_order[ml_order_n++] = ml;
      }
    }
    break;
  case TUI_RACK_SORT_VOWELS:
    for (int ml = 1; ml < ld_size && ml_order_n < 64; ml++) {
      if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
        ml_order[ml_order_n++] = ml;
      }
    }
    for (int ml = 1; ml < ld_size && ml_order_n < 64; ml++) {
      if (!ld_get_is_vowel(ld, (MachineLetter)ml)) {
        ml_order[ml_order_n++] = ml;
      }
    }
    if (ml_order_n < 64) {
      ml_order[ml_order_n++] = BLANK_MACHINE_LETTER;
    }
    break;
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    for (int ml = 1; ml < ld_size && ml_order_n < 64; ml++) {
      ml_order[ml_order_n++] = ml;
    }
    if (ml_order_n < 64) {
      ml_order[ml_order_n++] = BLANK_MACHINE_LETTER;
    }
    break;
  }

  char line[512];
  size_t pos = 0;
  for (int k = 0; k < ml_order_n; k++) {
    const int ml = ml_order[k];
    if (counts[ml] == 0) {
      continue;
    }
    const char *letter = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
    if (pos > 0 && pos + 1 < sizeof(line)) {
      line[pos++] = ' ';
    }
    const size_t letter_len = strlen(letter);
    for (int i = 0; i < counts[ml] && pos + letter_len + 1 < sizeof(line);
         i++) {
      // No ZWNJ between adjacent blanks: dropping the row entirely on
      // some terminals (when the bag listing contains "??" + a ZWNJ)
      // is worse than letting fonts ligature the two question marks.
      memcpy(line + pos, letter, letter_len);
      pos += letter_len;
    }
  }
  line[pos] = '\0';

  // Wrap across all available interior rows except the last (reserved for
  // the vowel/consonant tally). Content butts up against the 1-col
  // border on each side; the extra column on each side previously
  // wasted made e.g. "30 vows/34 cons" overflow the right border at
  // halfwidth.
  const int interior_left = 1;
  const int interior_width = L->board_width - 2;
  const int content_top = L->bag_top + 1;
  const int content_bottom = L->bag_bottom - 2; // last row before tally
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  int line_row = content_top;
  size_t i = 0;
  while (i < pos && line_row <= content_bottom) {
    // Walk the bytes from `i` forward, counting visible cells.
    // Each ASCII byte = 1 cell. The 3-byte ZWNJ sequence
    // (0xE2 0x80 0x8C) is 0 cells. Remember the last space we
    // passed; if we hit the column limit mid-word we'll back up to
    // that space and break there.
    size_t scan = i;
    size_t last_space = i;
    bool space_seen = false;
    int cells = 0;
    while (scan < pos && cells < interior_width) {
      if (scan + 2 < pos && (unsigned char)line[scan] == 0xE2 &&
          (unsigned char)line[scan + 1] == 0x80 &&
          (unsigned char)line[scan + 2] == 0x8C) {
        scan += 3; // ZWNJ — invisible
        continue;
      }
      if (line[scan] == ' ') {
        last_space = scan;
        space_seen = true;
      }
      scan++;
      cells++;
    }
    size_t limit = scan;
    if (limit < pos && space_seen) {
      limit = last_space;
    }
    char chunk[256];
    size_t chunk_len = limit - i;
    if (chunk_len >= sizeof(chunk)) {
      chunk_len = sizeof(chunk) - 1;
    }
    memcpy(chunk, line + i, chunk_len);
    chunk[chunk_len] = '\0';
    ncplane_putstr_yx(plane, line_row, interior_left, chunk);
    line_row++;
    i = limit;
    // Skip leading spaces and any ZWNJ run on the next row.
    while (i < pos) {
      if (line[i] == ' ') {
        i++;
        continue;
      }
      if (i + 2 < pos && (unsigned char)line[i] == 0xE2 &&
          (unsigned char)line[i + 1] == 0x80 &&
          (unsigned char)line[i + 2] == 0x8C) {
        i += 3;
        continue;
      }
      break;
    }
  }

  // Vowel / consonant tally on the last interior row.
  int vowels = 0;
  int consonants = 0;
  for (int ml = 1; ml < ld_size; ml++) {
    if (ld->is_vowel[ml]) {
      vowels += counts[ml];
    } else {
      consonants += counts[ml];
    }
  }
  char tally_long[64];
  char tally_short[64];
  // U+00B7 is two bytes UTF-8 but one display column, so the display
  // width is the byte count minus one.
  const int long_bytes =
      snprintf(tally_long, sizeof(tally_long),
               "%d vowels \xc2\xb7 %d consonants", vowels, consonants);
  const int long_cols = long_bytes - 1;
  snprintf(tally_short, sizeof(tally_short), "%d vows/%d cons", vowels,
           consonants);
  const char *tally = (long_cols <= interior_width) ? tally_long : tally_short;
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, L->bag_bottom - 1, interior_left, tally);
}

// Live clock: how many seconds remain for player_idx, accounting for the
// time elapsed in the current on-turn player's turn so the display ticks
// in real time. Caller must hold state->mutex.
//
// Defensive clamping: a stray bad turn_started or seconds_used should
// produce a flat 0:00 / time_per_side display, never multi-million-minute
// nonsense. Negative `used` becomes 0; remaining is clamped to
// [0, time_per_side] so the visible clock always lives in the player's
// budget.
static double seconds_remaining(const TuiGameState *state, int player_idx) {
  double used = state->seconds_used[player_idx];
  if (used < 0.0) {
    used = 0.0;
  }
  // Only tick the live elapsed-time portion when a game is actually
  // in progress (the bot worker is running). At app launch the bot
  // is idle behind the startup menu — without this gate the on-turn
  // clock would count down even though no game has started.
  if (state->bot_started &&
      game_get_player_on_turn_index(state->game) == player_idx &&
      !game_over(state->game)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec - state->turn_started.tv_sec) +
                     (double)(now.tv_nsec - state->turn_started.tv_nsec) / 1e9;
    if (elapsed < 0.0) {
      elapsed = 0.0;
    }
    used += elapsed;
  }
  const double total = (double)state->time_per_side_seconds;
  double remaining = total - used;
  if (remaining < 0.0) {
    remaining = 0.0;
  }
  if (remaining > total) {
    remaining = total;
  }
  return remaining;
}

// Spectator-style pill: name, halfwidth rack inline, score, clock.
// Bounds (top, left, right) are taken from the Layout so pills can sit
// side-by-side as column headers in two-col mode or stack in one-col mode.
static void render_player_pill(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, int player_idx,
                               int top, int left, int right, bool halfwidth,
                               bool draw_box_around) {
  const int width = right - left + 1;
  if (draw_box_around) {
    draw_box(plane, theme, top, left, PILL_HEIGHT, width, NULL);
  }

  const Player *player = game_get_player(state->game, player_idx);
  const bool on_turn = pick_render_on_turn(state) == player_idx;
  const ThemeRgb player_accent =
      player_idx == 1 ? theme->on_turn_fg_p2 : theme->on_turn_fg;
  const int content_row = top + 1;
  // One col of padding inside the box (was 2). The on-turn arrow lives
  // in the very first interior col so the rack has more room.
  const int content_left = left + 1;
  const int content_right = right - 1;

  theme_apply_fg(plane, on_turn ? player_accent : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, content_row, content_left,
                    on_turn ? "\xe2\x96\xb6 " : "  ");
  // Player name (from a loaded GCG) if set; else fall back to the
  // generic "P1"/"P2" label. Always in the player's accent so the
  // header reads as belonging to that player even when off-turn.
  // Truncated to MAX_NAME_W so longer names like "New_Player_1"
  // don't push out the rack/score area. ASCII byte length is
  // used as a column count, which is accurate for the nicknames
  // we see in practice.
  enum { MAX_NAME_W = 10 };
  theme_apply_fg(plane, player_accent);
  char name[32];
  if (state->player_names[player_idx][0] != '\0') {
    snprintf(name, sizeof(name), "%s", state->player_names[player_idx]);
  } else {
    snprintf(name, sizeof(name), "P%d", player_idx + 1);
  }
  int name_w = (int)strlen(name);
  if (name_w > MAX_NAME_W) {
    name[MAX_NAME_W] = '\0';
    name_w = MAX_NAME_W;
  }
  ncplane_putstr(plane, name);

  // Right side: clock and score, separated by a single col gap. Clock
  // tops out at "99:59" (5 chars). Score follows the History cursor:
  // it's whatever this player had going into the cursored turn.
  (void)player;
  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           pick_render_score(state, player_idx));
  // Clocks are only meaningful for live Watch games. In CGP / load
  // mode the bot worker isn't running and the displayed times
  // don't reflect any real time control, so we hide them and
  // right-align the score against the pill's right edge.
  const bool show_clock = state->bot_started;
  int score_col;
  if (show_clock) {
    const double remaining = pick_render_clock_seconds(state, player_idx);
    char clock_str[16];
    format_clock(remaining < 0 ? 0 : (int)remaining, clock_str,
                 sizeof(clock_str));
    const int clock_len = (int)strlen(clock_str);
    const int clock_col = content_right - clock_len + 1;
    theme_apply_fg(plane, on_turn ? player_accent : theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_putstr_yx(plane, content_row, clock_col, clock_str);
    const int score_len = (int)strlen(score_str);
    score_col = clock_col - 1 - score_len;
  } else {
    const int score_len = (int)strlen(score_str);
    score_col = content_right - score_len + 1;
  }
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, content_row, score_col, score_str);

  // Rack between the name and the score, on tile_bg. Anchored to
  // the right edge of the name + a 1-col gap, so longer names
  // (from loaded GCGs) don't get clobbered by tile pills. Each
  // tile is 2 cols wide in fullwidth mode, 1 col in halfwidth —
  // halfwidth kicks in when the right column is too narrow for
  // two fullwidth pills side-by-side but still wide enough to
  // fit two halfwidth pills.
  const Rack *rack = pick_render_rack(state, player_idx);
  if (rack == NULL) {
    return;
  }
  const LetterDistribution *ld = state->ld;
  // content_left + 2 = column after the arrow / 2-space prefix.
  // + name_w = past the name. + 1 = one-col gap before the rack.
  const int rack_left = content_left + 2 + name_w + 1;
  const int rack_right_max = score_col - 2;
  const int tile_w = halfwidth ? 1 : 2;
  // Idle state: no rack tiles to render. Print a dim placeholder
  // where the tiles would normally appear.
  if (rack_get_total_letters(rack) == 0) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const char *msg = "(no rack)";
    if (rack_left + (int)strlen(msg) - 1 <= rack_right_max) {
      ncplane_putstr_yx(plane, content_row, rack_left, msg);
    }
    return;
  }
  if (rack_right_max >= rack_left + (tile_w - 1)) {
    int rcol = rack_left;
    theme_apply_fg(plane, player_idx == 1 ? theme->rack_tile2_fg
                                          : theme->rack_tile1_fg);
    theme_apply_bg(plane, player_idx == 1 ? theme->rack_tile2_bg
                                          : theme->rack_tile1_bg);
    MachineLetter pill_slots[RACK_SIZE];
    const int pill_slot_count = sort_rack_for_display(
        rack, ld, state->rack_sort, pill_slots, RACK_SIZE);
    for (int i = 0;
         i < pill_slot_count && rcol + (tile_w - 1) <= rack_right_max; i++) {
      const MachineLetter ml = pill_slots[i];
      if (halfwidth) {
        // Halfwidth blank: plain "?" glyph. The previous ZWNJ-
        // splicing-between-adjacent-blanks trick caused the row
        // containing the blanks to drop out on some terminals.
        const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
        ncplane_putstr_yx(plane, content_row, rcol, ascii);
      } else {
        const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, content_row, rcol, fullwidth);
        } else {
          const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, content_row, rcol, " ");
          ncplane_putstr(plane, ascii);
        }
      }
      rcol += tile_w;
    }
  }
}

// ── History panel ─────────────────────────────────────────────────────────
//
// Each entry takes 2 rows, woogles-style:
//   " 18. L1 RE(W)I(N)                  +38"
//   "     4:42 AEINRT                    91"
//
// When the going-out bonus is attached, two more rows are appended —
// rendered with the same delta-on-top / total-on-bottom shape as a
// scoring play so it reads as the closing adjustment rather than a
// crammed third column:
//   "     (EE)                          +4"
//   "                                    95"
//
// Both players use the same color scheme — a lighter gray (theme->fg) for
// the top rows of each row-pair, a darker gray (theme->dim_fg) for the
// lower rows. Selective bold marks the position and played-tile letters
// in the move, plus the running totals on the right.

enum { HISTORY_ERROR_MAX_LINES = 6 };

// Greedy word-wrap of plain ASCII `text` into `width`-column
// lines, written into `lines` (each row up to 127 chars + NUL).
// Returns the line count (capped at max_lines). Breaks at spaces
// when possible, hard-breaks an over-long word otherwise. Used by
// both the row-count pass and the render pass so they agree on
// how many rows an error message occupies.
static int wrap_error_lines(const char *text, int width, char lines[][128],
                            int max_lines) {
  if (text == NULL || text[0] == '\0' || width <= 0) {
    return 0;
  }
  if (width > 127) {
    width = 127;
  }
  int count = 0;
  const char *p = text;
  while (*p != '\0' && count < max_lines) {
    while (*p == ' ') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    int taken = 0;
    int last_space = -1;
    while (p[taken] != '\0' && taken < width) {
      if (p[taken] == ' ') {
        last_space = taken;
      }
      taken++;
    }
    int line_len;
    if (p[taken] == '\0') {
      line_len = taken; // remainder fits
    } else if (last_space >= 0) {
      line_len = last_space; // break at last space
    } else {
      line_len = width; // hard break a long word
    }
    memcpy(lines[count], p, (size_t)line_len);
    lines[count][line_len] = '\0';
    count++;
    p += line_len;
  }
  return count;
}

// Number of wrapped rows the error message needs for the given
// interior `width`. Accounts for the 2-cell "⚠ " prefix / indent
// by wrapping the message to width-2.
static int history_error_rows(const TuiHistoryEntry *e, int width) {
  if (e->error_str[0] == '\0') {
    return 0;
  }
  int ew = width - 2;
  if (ew < 4) {
    ew = 4;
  }
  char lines[HISTORY_ERROR_MAX_LINES][128];
  int n = wrap_error_lines(e->error_str, ew, lines, HISTORY_ERROR_MAX_LINES);
  return n < 1 ? 1 : n;
}

static int history_entry_rows(const TuiHistoryEntry *e, int width) {
  int rows = e->end_bonus != 0 ? 4 : 2;
  // Revalidation error: word-wrapped rows tucked under the entry's
  // move/rack pair. Lets the user see the impossible-move
  // explanation inline rather than hunting for a status line.
  rows += history_error_rows(e, width);
  return rows;
}

// Render a GCG-style move notation with played-through letters (the
// segments wrapped in parentheses) and the post-play leave (in square
// brackets) unbolded — everything outside the bracketed/parenthesized
// segments reads as bold. The history and analysis panels share this
// so segments look consistent in both. Caller positions the text;
// this routine handles segmenting and style changes only.
//
// `hide_parens` is true in the analysis panel: the parentheses
// themselves are skipped (so "T(H)OLOI" reads as "THOLOI", with the H
// rendered non-bold). History keeps the parens so a played-through
// tile is visually unmistakable in the move log. Square brackets
// (used for leaves in history) are always shown verbatim.
// hide_parens:               drop every `(…)` group's brackets
// hide_playthrough_parens:   drop only `(…)` groups whose content is all
//                            uppercase ASCII (playthrough); keep parens
//                            around lowercase content (blank designation).
//                            Ignored when hide_parens is true.
// Both modes still render parens content non-bold.
static void render_move_styled(struct ncplane *plane, int row, int col,
                               const char *move_str, bool hide_parens,
                               bool hide_playthrough_parens) {
  if (move_str == NULL || *move_str == '\0') {
    return;
  }
  const char *p = move_str;
  const char *end = p + strlen(move_str);
  int x = col;
  // Compact-exchange marker: a leading "-" (used by the analysis
  // panel's "-ABCD" shorthand) renders non-bold so it reads as
  // punctuation rather than as part of the tile letters.
  if (*p == '-') {
    ncplane_set_styles(plane, 0);
    ncplane_putstr_yx(plane, row, x, "-");
    x += 1;
    p += 1;
  }
  while (p < end) {
    const char *seg_start = p;
    const char *seg_end;
    bool seg_bold;
    if (*p == '(') {
      seg_bold = false;
      seg_end = p;
      while (seg_end < end && *seg_end != ')') {
        seg_end++;
      }
      // Decide whether to drop this paren group's brackets. hide_parens
      // drops every group; hide_playthrough_parens only drops groups
      // whose content is all uppercase ASCII (the engine emits
      // lowercase inside parens for blank designation — N(a)P — and
      // uppercase for playthrough — CA(JO)N).
      bool drop_this = hide_parens;
      if (!drop_this && hide_playthrough_parens) {
        bool all_upper = (seg_end > p + 1);
        for (const char *q = p + 1; q < seg_end; q++) {
          const unsigned char ch = (unsigned char)*q;
          if (ch < 'A' || ch > 'Z') {
            all_upper = false;
            break;
          }
        }
        drop_this = all_upper;
      }
      if (drop_this) {
        seg_start = p + 1; // skip '('; ')' is dropped via seg_end below
      } else if (seg_end < end) {
        seg_end++; // include the close paren in the rendered segment
      }
    } else if (*p == '[') {
      seg_bold = false;
      seg_end = p;
      while (seg_end < end && *seg_end != ']') {
        seg_end++;
      }
      if (seg_end < end) {
        seg_end++; // include the close bracket in the rendered segment
      }
    } else {
      // Bold segment: a run of characters before the next paren or
      // bracket. We subdivide it so the '.' playthrough marker —
      // a fallback for moves whose position prefix couldn't be
      // parsed and so didn't get its dots resolved to (L) — still
      // renders non-bold. Lowercase letters in this segment are
      // newly-played blanks (the GCG convention is bare lowercase
      // for new blanks; played-through blanks come pre-wrapped as
      // (l)) and render bold like the rest. seg_bold is set
      // per-sub-run inside the inner loop, so the outer
      // initialization is just a placeholder.
      seg_bold = true;
      seg_end = p;
      while (seg_end < end && *seg_end != '(' && *seg_end != '[') {
        seg_end++;
      }
      const char *q = seg_start;
      while (q < seg_end) {
        const char *run_start = q;
        const bool run_dim = (*q == '.');
        while (q < seg_end && (*q == '.') == run_dim) {
          q++;
        }
        ncplane_set_styles(plane, run_dim ? 0 : NCSTYLE_BOLD);
        char rbuf[64];
        size_t rlen = (size_t)(q - run_start);
        if (rlen >= sizeof(rbuf)) {
          rlen = sizeof(rbuf) - 1;
        }
        memcpy(rbuf, run_start, rlen);
        rbuf[rlen] = '\0';
        ncplane_putstr_yx(plane, row, x, rbuf);
        x += (int)strlen(rbuf);
      }
      p = seg_end;
      continue;
    }
    ncplane_set_styles(plane, seg_bold ? NCSTYLE_BOLD : 0);
    char buf[64];
    size_t len = (size_t)(seg_end - seg_start);
    if (len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    memcpy(buf, seg_start, len);
    buf[len] = '\0';
    ncplane_putstr_yx(plane, row, x, buf);
    x += (int)strlen(buf);
    // When we dropped the parens for this group we landed on ')'; skip
    // past it so it doesn't render as a stray bracket. (seg_end was
    // not advanced over ')' in the drop branch.)
    if (seg_end < end && *seg_end == ')' && seg_start > p) {
      p = seg_end + 1;
    } else {
      p = seg_end;
    }
  }
  ncplane_set_styles(plane, 0);
}

static void
render_history_entry(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, const TuiHistoryEntry *e,
                     int idx, int row, int interior_left, int interior_right,
                     int row_bottom_inclusive, int leave_col_w, int rank_digits,
                     bool cursor_here, bool history_focused, bool clocks_active,
                     const LetterDistribution *ld, TuiRackSort rack_sort) {
  // Editor is active on this entry whenever edit_history_idx
  // points at it — committed entries included. (Previously gated
  // on e->pending, which meant clicking a finalized turn moved
  // edit_history_idx here but rendered no input zones / cursor,
  // so re-editing prior turns looked broken.)
  const bool editing = state != NULL && state->edit_history_idx == idx;
  // Pick the player-specific text-color pair so the entry reads as
  // belonging to whichever player made the move.
  const ThemeRgb player_fg =
      e->player_idx == 1 ? theme->history_p2_fg : theme->history_p1_fg;
  const ThemeRgb player_dim_fg =
      e->player_idx == 1 ? theme->history_p2_dim_fg : theme->history_p1_dim_fg;
  // ── Row 1 (lighter): " 18. L1 RE(W)I(N)              +38" ──────────────
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, player_fg);
  // Right-align the turn number to rank_digits — so a panel showing
  // turns 1-25 renders single-digit numbers with one leading space
  // (`" 1. "`), keeping every period on the same column.
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "%*d. ", rank_digits > 0 ? rank_digits : 1,
           idx + 1);
  if (cursor_here) {
    // History-panel cursor sits on this entry: render the rank
    // prefix with inverted colors (player hue preserved — bg =
    // player_fg, fg = theme->bg) and turn the trailing "." into
    // ">" when the panel is focused so the chip reads as "5>";
    // when the panel has lost focus, keep the inverted highlight
    // but restore the "." so it reads as a parked selection
    // rather than the active cursor. Bold for the same visual
    // weight a played tile uses. This takes precedence over the
    // pending tile-style highlight so the user can land the
    // cursor on the in-flight turn while the spinner is still
    // running.
    //
    // While the editor is active on this entry, focus has moved
    // INTO the row (a specific cell has the white cursor), so
    // the entry-level highlight steps down: we still show the
    // ">" chevron in player_fg + bold, but drop the inverted
    // background fill. That keeps "this is the active row" cue
    // without competing visually with the fine-grained cursor.
    int digit_start = 0;
    while (prefix[digit_start] == ' ') {
      digit_start++;
    }
    int after_period = digit_start;
    while (prefix[after_period] != '\0' && prefix[after_period] != ' ') {
      after_period++;
    }
    // Leading spaces stay on theme->bg in the player color.
    if (digit_start > 0) {
      char leading[8];
      memcpy(leading, prefix, (size_t)digit_start);
      leading[digit_start] = '\0';
      ncplane_putstr_yx(plane, row, interior_left, leading);
    }
    // "5>" / "5." segment: digits as-is, then ">" or "." in the
    // tail. Both share the inverted-colors chip — except in
    // editing mode, where the chip downgrades to non-inverted
    // bold text to defer to the in-cell white cursor.
    char chip[8];
    int chip_len = 0;
    for (int k = digit_start; k < after_period - 1 && chip_len < 6; k++) {
      chip[chip_len++] = prefix[k];
    }
    chip[chip_len++] = history_focused ? '>' : '.';
    chip[chip_len] = '\0';
    if (editing) {
      theme_apply_fg(plane, player_fg);
      theme_apply_bg(plane, theme->bg);
    } else {
      theme_apply_fg(plane, theme->bg);
      theme_apply_bg(plane, player_fg);
    }
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, interior_left + digit_start, chip);
    ncplane_set_styles(plane, 0);
    theme_apply_fg(plane, player_fg);
    theme_apply_bg(plane, theme->bg);
    if (prefix[after_period] != '\0') {
      ncplane_putstr_yx(plane, row, interior_left + after_period,
                        prefix + after_period);
    }
  } else if (e->pending) {
    // Pending turn: paint the "N." chunk with tile colors so it reads
    // like a played tile next to the spinner. Leading space (when N
    // is a single digit) and the trailing separator stay on the
    // normal background.
    int digit_start = 0;
    while (prefix[digit_start] == ' ') {
      digit_start++;
    }
    int after_period = digit_start;
    while (prefix[after_period] != '\0' && prefix[after_period] != ' ') {
      after_period++;
    }
    if (digit_start > 0) {
      char leading[8];
      memcpy(leading, prefix, (size_t)digit_start);
      leading[digit_start] = '\0';
      ncplane_putstr_yx(plane, row, interior_left, leading);
    }
    char tile_part[8];
    const int tile_len = after_period - digit_start;
    memcpy(tile_part, prefix + digit_start, (size_t)tile_len);
    tile_part[tile_len] = '\0';
    theme_apply_fg(plane,
                   e->player_idx == 1 ? theme->tile2_fg : theme->tile1_fg);
    theme_apply_bg(plane,
                   e->player_idx == 1 ? theme->tile2_bg : theme->tile1_bg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, interior_left + digit_start, tile_part);
    ncplane_set_styles(plane, 0);
    theme_apply_fg(plane, player_fg);
    theme_apply_bg(plane, theme->bg);
    if (prefix[after_period] != '\0') {
      ncplane_putstr_yx(plane, row, interior_left + after_period,
                        prefix + after_period);
    }
  } else {
    ncplane_putstr_yx(plane, row, interior_left, prefix);
  }

  if (editing) {
    // Annotation editor — modal-text-edit style. The row when
    // selected for editing gets a "selection bar" background
    // tinted toward the player's accent at a grey-level
    // brightness (max channel pinned to ~50, low saturation).
    // Editable text-input rectangles sit on pure black inside
    // the bar, mirroring the modal's "row_bg + dark zone"
    // pattern so the visual language stays consistent.
    //
    // Layout:  "1> [move zone (black)] [leave (black)]    +score"
    const int base_col = interior_left + (int)strlen(prefix);
    // Tint the row bg toward player_fg. Scale so the brightest
    // channel hits 50, then floor each channel at 24 so even the
    // dimmer channels stay visible enough to read as "tinted
    // grey" rather than "pure black with one bright channel".
    int max_ch = player_fg.r;
    if (player_fg.g > max_ch) {
      max_ch = player_fg.g;
    }
    if (player_fg.b > max_ch) {
      max_ch = player_fg.b;
    }
    ThemeRgb row_bg = {38, 38, 38};
    if (max_ch > 0) {
      const int target_max = 50;
      const int floor_ch = 24;
      int rr = (player_fg.r * target_max + max_ch / 2) / max_ch;
      int gg = (player_fg.g * target_max + max_ch / 2) / max_ch;
      int bb = (player_fg.b * target_max + max_ch / 2) / max_ch;
      if (rr < floor_ch) {
        rr = floor_ch;
      }
      if (gg < floor_ch) {
        gg = floor_ch;
      }
      if (bb < floor_ch) {
        bb = floor_ch;
      }
      row_bg.r = (uint8_t)rr;
      row_bg.g = (uint8_t)gg;
      row_bg.b = (uint8_t)bb;
    }
    const ThemeRgb zone_bg = {0, 0, 0};
    const ThemeRgb white_bg = {255, 255, 255};
    // 4-cell strip at the right edge holds "+100" — same as
    // committed rows, so the score column stays aligned across
    // edit / non-edit states.
    const int score_col_w = 4;
    const int score_col_right = interior_right;
    (void)score_col_w; // width is implicit in leave_zone_right's offset
    // Compute the leave display string up front so we can size
    // its zone to fit. Empty leave = "·" (1 cell). Non-empty
    // leave gets alphagrammed via the user's rack-sort; that
    // sets the zone width (which can be up to 6 for a max
    // non-bingo leave). The right edge of the leave zone sits
    // at interior_right - 4 — the same column the committed-row
    // renderer anchors its leave_right_edge to — so a turn's "·"
    // (or last leave letter) doesn't shift horizontally when the
    // editor exits and the row re-renders in committed style.
    // Leave display: use the user-typed leave_buf as-is when the
    // user has touched it (either clicked focus + typed, or pressed
    // anything into the field). Otherwise show the auto-derived
    // edit_move_leave (alphagrammed for consistency with committed
    // rows). When LEAVE has focus we display the buffer even if
    // empty so the cursor lands on a visible zone.
    const bool leave_user_typed = state->edit_leave_len > 0;
    const bool leave_focused = state->edit_field == TUI_EDIT_FIELD_LEAVE;
    char leave_disp[24];
    leave_disp[0] = '\0';
    if (leave_user_typed || leave_focused) {
      const int copy = state->edit_leave_len < (int)sizeof(leave_disp) - 1
                           ? state->edit_leave_len
                           : (int)sizeof(leave_disp) - 1;
      memcpy(leave_disp, state->edit_leave_buf, (size_t)copy);
      leave_disp[copy] = '\0';
    } else if (state->edit_move_leave[0] != '\0') {
      format_alphagram_for_sort(state->edit_move_leave, state->ld,
                                state->rack_sort, leave_disp,
                                sizeof(leave_disp));
    }
    const bool leave_empty = leave_disp[0] == '\0';
    // Ensure the zone is wide enough for the cursor when focused
    // — at least 1 cell wider than the text so an end-of-buffer
    // cursor has somewhere to sit. Caps at 8 cells (max practical
    // leave length + cursor).
    int leave_zone_w = leave_empty ? 1 : (int)strlen(leave_disp);
    if (leave_focused) {
      leave_zone_w = state->edit_leave_len + 1;
      if (leave_zone_w < 1) {
        leave_zone_w = 1;
      }
      if (leave_zone_w > 8) {
        leave_zone_w = 8;
      }
    }
    const int leave_zone_right = interior_right - 4;
    const int leave_zone_left = leave_zone_right - leave_zone_w + 1;
    const int move_zone_left = base_col;
    const int move_zone_right = leave_zone_left - 2; // 1-cell gap
    // Paint the entire row (under the chip / prefix region too)
    // with the selection bar bg. The "1>" chip was already
    // painted above on theme->bg; repaint the post-chip prefix
    // tail and the gaps between zones with row_bg.
    theme_apply_bg(plane, row_bg);
    theme_apply_fg(plane, theme->fg);
    for (int c = interior_left + (int)strlen(prefix) - 1; c <= interior_right;
         c++) {
      if (c < interior_left) {
        continue;
      }
      ncplane_putstr_yx(plane, row, c, " ");
    }
    // Recessed black rectangle for the move-text input.
    theme_apply_bg(plane, zone_bg);
    theme_apply_fg(plane, theme->dim_fg);
    for (int c = move_zone_left; c <= move_zone_right && c <= interior_right;
         c++) {
      ncplane_putstr_yx(plane, row, c, " ");
    }
    // Move buffer text overlay (preserves zone bg).
    const char *buf = state->edit_move_buf;
    const int buf_len = state->edit_move_len;
    const ThemeRgb move_txt_fg =
        state->edit_move_valid ? player_fg : theme->error_fg;
    for (int j = 0; j < buf_len; j++) {
      const int col = move_zone_left + j;
      if (col > move_zone_right) {
        break;
      }
      theme_apply_fg(plane, move_txt_fg);
      theme_apply_bg(plane, zone_bg);
      char ch[2] = {buf[j], '\0'};
      ncplane_putstr_yx(plane, row, col, ch);
    }
    // Leave input rectangle (also recessed black). Width was
    // already sized to fit the alphagrammed leave; render its
    // letters left-justified inside the zone. Empty leave shows
    // the "·" placeholder in a single-cell zone.
    if (leave_zone_left > move_zone_right) {
      theme_apply_bg(plane, zone_bg);
      theme_apply_fg(plane, player_dim_fg);
      for (int c = leave_zone_left; c <= leave_zone_right; c++) {
        ncplane_putstr_yx(plane, row, c, " ");
      }
      if (leave_focused) {
        // Render each char of edit_leave_buf left-justified; the
        // placeholder "·" only shows when the field is empty AND
        // not focused.
        for (int j = 0; j < state->edit_leave_len; j++) {
          const int col = leave_zone_left + j;
          if (col > leave_zone_right) {
            break;
          }
          char ch[2] = {state->edit_leave_buf[j], '\0'};
          theme_apply_bg(plane, zone_bg);
          theme_apply_fg(plane, player_dim_fg);
          ncplane_putstr_yx(plane, row, col, ch);
        }
      } else if (!leave_empty) {
        ncplane_putstr_yx(plane, row, leave_zone_left, leave_disp);
      } else {
        ncplane_putstr_yx(plane, row, leave_zone_left, "\xc2\xb7");
      }
    }
    // White cursor block when LEAVE has focus — mirrors the MOVE
    // cursor block rendered below.
    if (leave_focused && leave_zone_left > move_zone_right) {
      const int cur_col = leave_zone_left + state->edit_leave_cursor;
      if (cur_col >= leave_zone_left && cur_col <= leave_zone_right) {
        char ch[2] = {' ', '\0'};
        if (state->edit_leave_cursor < state->edit_leave_len) {
          ch[0] = state->edit_leave_buf[state->edit_leave_cursor];
        }
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, white_bg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, row, cur_col, ch);
        ncplane_set_styles(plane, 0);
      }
    }
    // "+score" right-anchored at interior_right on the row bar
    // bg (not on the input black) — matches committed-row geometry.
    if (state->edit_move_score >= 0) {
      char score_str[8];
      snprintf(score_str, sizeof(score_str), "+%d", state->edit_move_score);
      const int score_len = (int)strlen(score_str);
      const int score_col = score_col_right - score_len + 1;
      theme_apply_fg(plane, player_fg);
      theme_apply_bg(plane, row_bg);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row, score_col, score_str);
      ncplane_set_styles(plane, 0);
    }
    // White block cursor, only when MOVE has focus.
    if (state->edit_field == TUI_EDIT_FIELD_MOVE) {
      const int cur_col = move_zone_left + state->edit_move_cursor;
      if (cur_col >= move_zone_left && cur_col <= move_zone_right) {
        char ch[2] = {' ', '\0'};
        if (state->edit_move_cursor < buf_len) {
          ch[0] = buf[state->edit_move_cursor];
        }
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, white_bg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, row, cur_col, ch);
        ncplane_set_styles(plane, 0);
      }
    }
    // Restore default bg for any code that follows.
    theme_apply_bg(plane, theme->bg);
  } else if (e->pending && e->move_str[0] != '\0') {
    // Annotation in progress: the user has committed a move
    // into this still-pending row (via Enter on the move field).
    // Render the move text + leave + "+score" the same way a
    // finalized entry would, so closing the editor doesn't make
    // the committed text disappear.
    render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                       e->move_str, /*hide_parens=*/true,
                       /*hide_playthrough_parens=*/false);
    char delta_str[16];
    snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
    const int delta_len = (int)strlen(delta_str);
    const int delta_col = interior_right - delta_len + 1;
    if (delta_col > interior_left + (int)strlen(prefix)) {
      theme_apply_fg(plane, player_fg);
      theme_apply_bg(plane, theme->bg);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row, delta_col, delta_str);
      ncplane_set_styles(plane, 0);
    }
    // Leave column — same right-anchored geometry committed
    // turns use (4 cells right of interior_right for the delta,
    // then leave snug to its left). Empty leave shows the "·"
    // bingo glyph; otherwise the alphagrammed leave.
    {
      const bool empty = e->leave_str[0] == '\0';
      char sorted_leave[24];
      if (!empty && ld != NULL) {
        format_alphagram_for_sort(e->leave_str, ld, rack_sort, sorted_leave,
                                  sizeof(sorted_leave));
      } else {
        sorted_leave[0] = '\0';
      }
      const char *leave_text = empty ? "\xc2\xb7" : sorted_leave;
      const int leave_w = empty ? 1 : (int)strlen(leave_text);
      int leave_right_edge = interior_right - 4;
      if (leave_right_edge >= delta_col) {
        leave_right_edge = delta_col - 1;
      }
      const int leave_col = leave_right_edge - leave_w + 1;
      if (leave_col > interior_left + (int)strlen(prefix)) {
        theme_apply_fg(plane, player_dim_fg);
        theme_apply_bg(plane, theme->bg);
        ncplane_set_styles(plane, 0);
        ncplane_putstr_yx(plane, row, leave_col, leave_text);
      }
    }
  } else if (e->pending) {
    // Bot is still computing this turn — show a braille spinner where
    // the move notation will go and leave the +score column blank.
    // 10-frame cycle at ~80ms per frame derives from CLOCK_MONOTONIC
    // so the animation runs even when the renderer is otherwise idle.
    //
    // Skip the spinner in CGP / non-bot mode: no one is "thinking,"
    // and showing a perpetual spinner reads as the app being busy.
    if (clocks_active) {
      static const char *const spinner_frames[] = {
          "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
          "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
          "\xe2\xa0\x87", "\xe2\xa0\x8f",
      };
      enum { SPINNER_FRAMES = 10 };
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const uint64_t ms =
          (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
      const int frame = (int)((ms / 80) % SPINNER_FRAMES);
      ncplane_putstr(plane, spinner_frames[frame]);
    }
  } else {
    // All paren groups in the engine's move notation are playthrough
    // (the lowercase-inside-parens case is a playthrough blank, not a
    // newly-played blank — newly-played blanks render as lowercase
    // letters with no parens). Drop them all and render the content
    // non-bold so playthrough tiles read as background context.
    render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                       e->move_str, /*hide_parens=*/true,
                       /*hide_playthrough_parens=*/false);

    char delta_str[16];
    snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
    const int delta_len = (int)strlen(delta_str);
    const int delta_col = interior_right - delta_len + 1;
    if (delta_col > interior_left + (int)strlen(prefix)) {
      ncplane_putstr_yx(plane, row, delta_col, delta_str);
    }

    // Leave column. Right edge anchored to (interior_right - 4) so a
    // 2-digit delta (`+24`, the common case) sits with a 1-cell gap
    // (`ADLMT +24`) and a 3-digit delta (`+185`) lands flush against
    // the leave (`·+185`). For unusually long deltas (4+ chars) we
    // clamp so the leave never overlaps the delta string. Color is
    // the muted player-accent (same family as line 2) so the leave
    // reads as belonging to that player.
    if (leave_col_w > 0) {
      const bool empty = e->leave_str[0] == '\0';
      char sorted_leave[24];
      if (!empty && ld != NULL) {
        format_alphagram_for_sort(e->leave_str, ld, rack_sort, sorted_leave,
                                  sizeof(sorted_leave));
      } else {
        sorted_leave[0] = '\0';
      }
      const char *leave_text = empty ? "\xc2\xb7" : sorted_leave;
      // Visual width: "·" (U+00B7) is 2 bytes but renders as 1 cell.
      // strlen would over-shift the right-alignment by a column.
      const int leave_w = empty ? 1 : (int)strlen(leave_text);
      int leave_right_edge = interior_right - 4;
      if (leave_right_edge >= delta_col) {
        leave_right_edge = delta_col - 1;
      }
      const int leave_col = leave_right_edge - leave_w + 1;
      if (leave_col > interior_left + (int)strlen(prefix)) {
        theme_apply_fg(plane, player_dim_fg);
        ncplane_set_styles(plane, 0);
        ncplane_putstr_yx(plane, row, leave_col, leave_text);
      }
    }
  }
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, player_fg);

  // ── Row 2 (darker): "     4:42 AEINRT                91" ───────────────
  if (row + 1 > row_bottom_inclusive) {
    return;
  }
  const int row2 = row + 1;

  theme_apply_fg(plane, player_dim_fg);
  char left_line[48];
  // Resort the rack alphagram per the user's preference so it
  // matches what their rack panel shows for the same letters.
  char sorted_rack[24];
  if (e->rack_str[0] != '\0' && ld != NULL) {
    format_alphagram_for_sort(e->rack_str, ld, rack_sort, sorted_rack,
                              sizeof(sorted_rack));
  } else {
    sorted_rack[0] = '\0';
  }
  // Empty rack on a finalized entry renders an em-dash ("—") so
  // the row's secondary line isn't ambiguously blank. Pending
  // entries (annotation seed rows, in-flight bot turns with no
  // rack yet) just leave the cell empty — once a block cursor
  // lives in this cell during editing, the cursor itself will
  // be the only visible mark.
  const char *rack_disp = sorted_rack[0] != '\0' ? sorted_rack
                          : e->pending           ? ""
                                                 : "\xe2\x80\x94";
  // Row 2 indent matches the prefix length so the secondary
  // info (clock + rack, or just rack in CGP mode) aligns with
  // where the move started on row 1 ("4. 14F XU" → "   2:45 EGIPS").
  if (clocks_active) {
    char clock_str[16];
    format_clock(e->clock_at_start < 0 ? 0 : e->clock_at_start, clock_str,
                 sizeof(clock_str));
    snprintf(left_line, sizeof(left_line), "%*s%s %s", (int)strlen(prefix), "",
             clock_str, rack_disp);
  } else {
    snprintf(left_line, sizeof(left_line), "%*s%s", (int)strlen(prefix), "",
             rack_disp);
  }
  ncplane_putstr_yx(plane, row2, interior_left, left_line);

  // When this entry's RACK field is being edited, overlay the
  // edit-rack buffer (with optional white block cursor) on top
  // of the row-2 rack rendering. The buffer is rendered at the
  // same indent as left_line's rack position (after the prefix
  // and the optional clock). The zone is 8 cells wide — 7 for
  // tile slots plus a final cell that's reserved for the cursor
  // when the rack is full. Input is capped at 7 tiles so the
  // cursor in cell 8 reads as "rack is full, no room for more".
  if (editing) {
    int rack_col = interior_left + (int)strlen(prefix);
    if (clocks_active) {
      char clock_str[16];
      format_clock(e->clock_at_start < 0 ? 0 : e->clock_at_start, clock_str,
                   sizeof(clock_str));
      rack_col += (int)strlen(clock_str) + 1;
    }
    // Same player-tinted row bg as row 1 — computed inline so
    // both rows share the exact same hue and brightness floor.
    int max_ch_r2 = player_fg.r;
    if (player_fg.g > max_ch_r2) {
      max_ch_r2 = player_fg.g;
    }
    if (player_fg.b > max_ch_r2) {
      max_ch_r2 = player_fg.b;
    }
    ThemeRgb row_bg = {38, 38, 38};
    if (max_ch_r2 > 0) {
      const int target_max = 50;
      const int floor_ch = 24;
      int rr = (player_fg.r * target_max + max_ch_r2 / 2) / max_ch_r2;
      int gg = (player_fg.g * target_max + max_ch_r2 / 2) / max_ch_r2;
      int bb = (player_fg.b * target_max + max_ch_r2 / 2) / max_ch_r2;
      if (rr < floor_ch) {
        rr = floor_ch;
      }
      if (gg < floor_ch) {
        gg = floor_ch;
      }
      if (bb < floor_ch) {
        bb = floor_ch;
      }
      row_bg.r = (uint8_t)rr;
      row_bg.g = (uint8_t)gg;
      row_bg.b = (uint8_t)bb;
    }
    const ThemeRgb zone_bg = {0, 0, 0};
    const ThemeRgb white_bg = {255, 255, 255};
    const int rack_zone_left = rack_col;
    const int rack_zone_right = rack_col + 7; // 8 cells: 7 tiles + cursor slot
    // Repaint the entire row-2 with the selection-bar bg so the
    // gaps around the zone match modal text-edit chrome.
    theme_apply_bg(plane, row_bg);
    theme_apply_fg(plane, player_dim_fg);
    for (int c = interior_left; c <= interior_right; c++) {
      ncplane_putstr_yx(plane, row2, c, " ");
    }
    // Recessed black rectangle for the rack input.
    theme_apply_bg(plane, zone_bg);
    theme_apply_fg(plane, theme->dim_fg);
    for (int c = rack_zone_left; c <= rack_zone_right && c <= interior_right;
         c++) {
      ncplane_putstr_yx(plane, row2, c, " ");
    }
    // Buffer display. While the RACK field has focus we render
    // the typed buffer verbatim — letters stay where the user
    // put them. When RACK is unfocused (edit_field == MOVE) we
    // alphagram via the user's rack-sort preference so it lines
    // up with the rest of the UI. The sort only kicks in on
    // focus-leave, matching the modal text-edit feel.
    //
    // If the rack buffer is empty but the move parser inferred
    // a played-tiles rack, preview it here too — so the user
    // sees the rack populating in row 2 as they type the move
    // (alphagrammed since RACK doesn't have focus yet).
    char display_buf[24];
    display_buf[0] = '\0';
    if (state->edit_field == TUI_EDIT_FIELD_RACK && state->edit_rack_len > 0) {
      // While the RACK field is focused, show the live buffer in the user's
      // typed order — even if it's not yet a valid rack — so editing is
      // visible keystroke by keystroke.
      const int copy = state->edit_rack_len < (int)sizeof(display_buf) - 1
                           ? state->edit_rack_len
                           : (int)sizeof(display_buf) - 1;
      memcpy(display_buf, state->edit_rack_buf, (size_t)copy);
      display_buf[copy] = '\0';
    } else {
      // Otherwise mirror the pill exactly: the same effective-rack selection
      // sync_player_rack_to_editor uses for the engine rack. This is the
      // single source of truth, so the cell's rack row and the player pill
      // can't disagree (the bug where an invalid-but-present buffer showed
      // in one place but not the other).
      char eff[24];
      if (tui_game_state_effective_editor_rack(state, eff, sizeof(eff), NULL) >
          0) {
        format_alphagram_for_sort(eff, state->ld, state->rack_sort, display_buf,
                                  sizeof(display_buf));
      }
    }
    const int display_len = (int)strlen(display_buf);
    const ThemeRgb rack_fg =
        state->edit_rack_valid ? player_dim_fg : theme->error_fg;
    for (int j = 0; j < display_len; j++) {
      const int col = rack_zone_left + j;
      if (col > rack_zone_right) {
        break;
      }
      theme_apply_fg(plane, rack_fg);
      theme_apply_bg(plane, zone_bg);
      char ch[2] = {display_buf[j], '\0'};
      ncplane_putstr_yx(plane, row2, col, ch);
    }
    if (state->edit_field == TUI_EDIT_FIELD_RACK) {
      // Cursor sits at the buffer's end position. With input
      // capped at 7 tiles, the cursor reaches column 7 (the
      // 8th cell) when the rack is full — a visual signal that
      // further keypresses won't add tiles.
      int cur_off = display_len;
      if (cur_off > 7) {
        cur_off = 7;
      }
      const int cur_col = rack_zone_left + cur_off;
      if (cur_col >= rack_zone_left && cur_col <= rack_zone_right) {
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, white_bg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, row2, cur_col, " ");
        ncplane_set_styles(plane, 0);
      }
    }
    theme_apply_bg(plane, theme->bg);
  }

  if (!e->pending) {
    char total_str[16];
    snprintf(total_str, sizeof(total_str), "%d", e->total_after);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    if (total_col > interior_left + (int)strlen(left_line)) {
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row2, total_col, total_str);
      ncplane_set_styles(plane, 0);
    }
  } else if (e->pending && !editing && e->move_str[0] != '\0') {
    // Pending row with a committed move — show the cumulative
    // total the same way a finalized row does. Sum prior same-
    // player committed scores and add this entry's score.
    int total_before = 0;
    for (int prev = 0; prev < idx; prev++) {
      const TuiHistoryEntry *pe = &state->history[prev];
      if (pe->player_idx == e->player_idx && !pe->pending) {
        total_before += pe->score + pe->end_bonus;
      }
    }
    char total_str[16];
    snprintf(total_str, sizeof(total_str), "%d", total_before + e->score);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    if (total_col > interior_left + (int)strlen(left_line)) {
      theme_apply_fg(plane, player_dim_fg);
      theme_apply_bg(plane, theme->bg);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row2, total_col, total_str);
      ncplane_set_styles(plane, 0);
    }
  } else if (editing && state->edit_move_score >= 0) {
    // Running total preview: sum prior turns for this player
    // plus the just-parsed move's score. Renders bold in the
    // muted player accent so the eye reads it as "what your
    // score WILL be" rather than committed.
    int total_before = 0;
    for (int prev = 0; prev < idx; prev++) {
      const TuiHistoryEntry *pe = &state->history[prev];
      if (pe->player_idx == e->player_idx && !pe->pending) {
        total_before += pe->score + pe->end_bonus;
      }
    }
    char total_str[16];
    snprintf(total_str, sizeof(total_str), "%d",
             total_before + state->edit_move_score);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    theme_apply_fg(plane, player_dim_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row2, total_col, total_str);
    ncplane_set_styles(plane, 0);
  }

  // ── Row 3 (going-out bonus delta): "    (LNRU)               +8" ──────
  // Split coloring: the leftover-rack chunk on the left is rendered
  // in the *opponent's* color (those are their tiles), while the
  // "+N" bonus stays in the going-out player's color (their points).
  if (e->end_bonus == 0 || row + 2 > row_bottom_inclusive) {
    return;
  }
  const int row3 = row + 2;
  ncplane_set_styles(plane, 0);
  const ThemeRgb opponent_fg =
      e->player_idx == 1 ? theme->history_p1_fg : theme->history_p2_fg;
  char bonus_left[48];
  if (e->end_rack_str[0] != '\0') {
    char sorted_end[24];
    if (ld != NULL) {
      format_alphagram_for_sort(e->end_rack_str, ld, rack_sort, sorted_end,
                                sizeof(sorted_end));
    } else {
      snprintf(sorted_end, sizeof(sorted_end), "%s", e->end_rack_str);
    }
    snprintf(bonus_left, sizeof(bonus_left), "    (%s)", sorted_end);
  } else {
    snprintf(bonus_left, sizeof(bonus_left), "    ");
  }
  theme_apply_fg(plane, opponent_fg);
  ncplane_putstr_yx(plane, row3, interior_left, bonus_left);

  char delta3_str[16];
  snprintf(delta3_str, sizeof(delta3_str), "+%d", e->end_bonus);
  const int delta3_len = (int)strlen(delta3_str);
  const int delta3_col = interior_right - delta3_len + 1;
  if (delta3_col > interior_left + (int)strlen(bonus_left)) {
    theme_apply_fg(plane, player_fg);
    ncplane_putstr_yx(plane, row3, delta3_col, delta3_str);
  }

  // ── Row 4 (final clock + final score): "    0:09         489" ──────────
  // Bold, right-aligned final game total; on the left, the player's
  // clock at the moment they finished the game so the closing time
  // shows in-place rather than only in the player pill. Mirrors
  // row 2's "<clock> <rack>" layout — same indent, same player
  // accent color.
  if (row + 3 > row_bottom_inclusive) {
    return;
  }
  const int row4 = row + 3;

  if (clocks_active) {
    char end_clock_str[16];
    format_clock(e->clock_at_end < 0 ? 0 : e->clock_at_end, end_clock_str,
                 sizeof(end_clock_str));
    char end_line[32];
    snprintf(end_line, sizeof(end_line), "%*s%s", (int)strlen(prefix), "",
             end_clock_str);
    theme_apply_fg(plane, player_fg);
    ncplane_putstr_yx(plane, row4, interior_left, end_line);
  }

  theme_apply_fg(plane, player_dim_fg);
  char total4_str[16];
  snprintf(total4_str, sizeof(total4_str), "%d", e->total_after + e->end_bonus);
  const int total4_len = (int)strlen(total4_str);
  const int total4_col = interior_right - total4_len + 1;
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, row4, total4_col, total4_str);
  ncplane_set_styles(plane, 0);
}

// Render the entry's revalidation error message on the row just
// past the entry's main body. `err_row` is the absolute screen
// row to draw on; the message is truncated to fit between
// interior_left and interior_right. Caller has already reserved
// the row via history_entry_rows. Two-column callers reserve the
// row per-entry, so two adjacent entries' errors can coexist on
// the same screen row (each in its own column).
// Render the entry's (word-wrapped) error message starting at
// `err_row`. The first line carries the "⚠ " glyph prefix;
// continuation lines indent 2 cells to align under the message.
// Returns the number of rows drawn (matches history_error_rows).
static int render_history_error_row(struct ncplane *plane, const Theme *theme,
                                    const TuiHistoryEntry *e, int err_row,
                                    int interior_left, int interior_right) {
  if (e == NULL || e->error_str[0] == '\0') {
    return 0;
  }
  const int width = interior_right - interior_left + 1;
  if (width <= 0) {
    return 0;
  }
  int ew = width - 2; // leave room for the "⚠ " prefix / indent
  if (ew < 4) {
    ew = 4;
  }
  char lines[HISTORY_ERROR_MAX_LINES][128];
  const int n =
      wrap_error_lines(e->error_str, ew, lines, HISTORY_ERROR_MAX_LINES);
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  for (int i = 0; i < n; i++) {
    if (i == 0) {
      // "⚠ " (U+26A0 + space) then the first wrapped segment.
      ncplane_putstr_yx(plane, err_row, interior_left, "\xe2\x9a\xa0 ");
      ncplane_putstr_yx(plane, err_row, interior_left + 2, lines[i]);
    } else {
      // 2-cell indent to align continuation lines under the text.
      ncplane_putstr_yx(plane, err_row + i, interior_left + 2, lines[i]);
    }
  }
  return n;
}

static void render_history_panel(struct ncplane *plane, const Theme *theme,
                                 const TuiGameState *state, const Layout *L) {
  const int width = L->right_col_right - L->right_col_left + 1;
  const int height = L->history_bottom - L->history_top + 1;
  if (height < 3) {
    return;
  }
  const bool history_focused = state->focused_panel == TUI_FOCUS_HISTORY;
  // Reset the per-frame hit-test map. Bounding box always reflects
  // the whole panel so a click on the chrome (or the title row in
  // combined mode) still resolves to "in history, but not on an
  // entry" and snaps the cursor to the [4>] label.
  history_row_map_count = 0;
  history_panel_top = L->history_top;
  history_panel_bottom = L->history_bottom;
  history_panel_left = L->right_col_left;
  history_panel_right = L->right_col_right;
  if (!L->combined_pills_history) {
    // badge_secondary = true when focus is on a sub-element (an
    // entry row) so the chevron moves off the label and onto the
    // selected row; the badge stays bright as the focus-of-panel
    // indicator but reverts from "[4>" to "[4]".
    const bool badge_secondary = state->history_cursor >= 0;
    draw_box_styled_ex(plane, theme, L->history_top, L->right_col_left, height,
                       width, "History", TUI_FOCUS_HISTORY, history_focused,
                       badge_secondary);
  }

  if (state->history_count == 0) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    // Place the placeholder on the panel's top interior row instead
    // of vertically centered — the startup modal sits in the middle
    // of the screen and would otherwise hide the empty-state text.
    const int top_row = L->history_top + 1;
    const char *msg = "(no moves yet)";
    const int interior_width = width - 2;
    const int msg_col =
        L->right_col_left + 1 + (interior_width - (int)strlen(msg)) / 2;
    ncplane_putstr_yx(plane, top_row, msg_col, msg);
    return;
  }

  const int top = L->history_top + 1;       // first interior row
  const int bottom = L->history_bottom - 1; // last interior row (inclusive)
  const int rows_avail = bottom - top + 1;

  if (!L->two_col) {
    const int interior_left = L->right_col_left + 1;
    const int interior_right = L->right_col_right - 1;

    // Walk backwards to find the oldest entry that still fits, so the
    // most recent entries always show.
    int first = state->history_count;
    int rows_used = 0;
    const int interior_width = interior_right - interior_left + 1;
    while (first > 0) {
      const int rows =
          history_entry_rows(&state->history[first - 1], interior_width);
      if (rows_used + rows > rows_avail) {
        break;
      }
      rows_used += rows;
      first--;
    }
    // Pre-pass: compute the widest leave_str (or "·" for an outplay)
    // among the visible finalized entries. Pending entries contribute
    // nothing — they have no leave yet. leave_col_w == 0 means the
    // visible window has no leave-bearing rows; render_history_entry
    // suppresses the column in that case.
    int leave_col_w = 0;
    for (int idx = first; idx < state->history_count; idx++) {
      const TuiHistoryEntry *e = &state->history[idx];
      if (e->pending) {
        continue;
      }
      const int w = e->leave_str[0] != '\0' ? (int)strlen(e->leave_str) : 1;
      if (w > leave_col_w) {
        leave_col_w = w;
      }
    }
    // rank_digits: width to right-align the turn number against. Once
    // any 2-digit turn is visible, single-digit turns get a leading
    // space so periods stay column-aligned.
    int rank_digits = 1;
    for (int n = state->history_count; n >= 10; n /= 10) {
      rank_digits++;
    }
    int row = top;
    for (int idx = first; idx < state->history_count; idx++) {
      const TuiHistoryEntry *e = &state->history[idx];
      const bool cursor_here = state->history_cursor == idx;
      const int entry_rows = history_entry_rows(e, interior_width);
      render_history_entry(plane, theme, state, e, idx, row, interior_left,
                           interior_right, bottom, leave_col_w, rank_digits,
                           cursor_here, history_focused, state->bot_started,
                           state->ld, state->rack_sort);
      // Error rows sit below the entry body. The wrapped error
      // occupies (entry_rows − base) rows; base = entry_rows minus
      // the error-row count, so the first error row is at
      // row + base.
      if (e->error_str[0] != '\0') {
        const int err_rows = history_error_rows(e, interior_width);
        render_history_error_row(plane, theme, e, row + entry_rows - err_rows,
                                 interior_left, interior_right);
      }
      if (history_row_map_count <
          (int)(sizeof(history_row_map) / sizeof(history_row_map[0]))) {
        HistoryRowMap *m = &history_row_map[history_row_map_count++];
        m->top_row = row;
        m->bottom_row = row + entry_rows - 1;
        m->left_col = interior_left;
        m->right_col = interior_right;
        m->idx = idx;
        // Leave hit zone: right-anchored, just inside the score
        // column. render_history_entry computes leave_right_edge =
        // interior_right - 4 and walks left by the leave glyph
        // width. We use a generous fixed-width zone that always
        // covers the "·" placeholder and any reasonable leave
        // (up to 8 chars wide) so the user doesn't have to land
        // their click on the exact glyph cell.
        m->leave_right = interior_right - 4;
        m->leave_left = m->leave_right - 7;
        if (m->leave_left < interior_left) {
          m->leave_left = interior_left;
        }
      }
      row += entry_rows;
    }
    return;
  }

  // Two-column layout. Entries are assigned to columns by absolute index
  // parity (even → left, odd → right) so the going-out player's row
  // stays in whichever column they happened to land on. Combined mode
  // (when pills + history share a single box) draws the outer borders
  // and the column divider for us; this just positions the content.
  const int left_l = L->right_col_left + 1;
  const int left_r = L->divider_col - 1;
  const int right_l = L->divider_col + 1;
  const int right_r = L->right_col_right - 1;

  // Walk backwards, tracking per-column row usage, to find the oldest
  // entry that still fits in its target column.
  int left_used = 0;
  int right_used = 0;
  int first = state->history_count;
  const int col_width_left = left_r - left_l + 1;
  const int col_width_right = right_r - right_l + 1;
  while (first > 0) {
    const int idx = first - 1;
    const int col_w = (idx % 2 == 0) ? col_width_left : col_width_right;
    const int rows = history_entry_rows(&state->history[idx], col_w);
    int *used = (idx % 2 == 0) ? &left_used : &right_used;
    if (*used + rows > rows_avail) {
      break;
    }
    *used += rows;
    first--;
  }

  // Per-column widest leave so each column's leave-column reads as a
  // tidy fixed strip. Empty leave on a finalized move counts as 1 col
  // (the "·" placeholder).
  int leave_w_left = 0;
  int leave_w_right = 0;
  for (int idx = first; idx < state->history_count; idx++) {
    const TuiHistoryEntry *e = &state->history[idx];
    if (e->pending) {
      continue;
    }
    const int w = e->leave_str[0] != '\0' ? (int)strlen(e->leave_str) : 1;
    int *bucket = (idx % 2 == 0) ? &leave_w_left : &leave_w_right;
    if (w > *bucket) {
      *bucket = w;
    }
  }
  // Single global rank_digits so turn numbers across both columns line
  // up (e.g. left-col " 9." matches right-col "10.").
  int rank_digits = 1;
  for (int n = state->history_count; n >= 10; n /= 10) {
    rank_digits++;
  }
  int row_left = top;
  int row_right = top;
  for (int idx = first; idx < state->history_count; idx++) {
    const TuiHistoryEntry *e = &state->history[idx];
    const int col_w = (idx % 2 == 0) ? col_width_left : col_width_right;
    const int rows = history_entry_rows(e, col_w);
    const int err_rows = history_error_rows(e, col_w);
    const bool cursor_here = state->history_cursor == idx;
    int row_top = 0;
    int col_left = 0;
    int col_right = 0;
    if ((idx % 2) == 0) {
      row_top = row_left;
      col_left = left_l;
      col_right = left_r;
      render_history_entry(plane, theme, state, e, idx, row_left, left_l,
                           left_r, bottom, leave_w_left, rank_digits,
                           cursor_here, history_focused, state->bot_started,
                           state->ld, state->rack_sort);
      if (e->error_str[0] != '\0') {
        render_history_error_row(plane, theme, e, row_left + rows - err_rows,
                                 left_l, left_r);
      }
      row_left += rows;
    } else {
      row_top = row_right;
      col_left = right_l;
      col_right = right_r;
      render_history_entry(plane, theme, state, e, idx, row_right, right_l,
                           right_r, bottom, leave_w_right, rank_digits,
                           cursor_here, history_focused, state->bot_started,
                           state->ld, state->rack_sort);
      if (e->error_str[0] != '\0') {
        render_history_error_row(plane, theme, e, row_right + rows - err_rows,
                                 right_l, right_r);
      }
      row_right += rows;
    }
    if (history_row_map_count <
        (int)(sizeof(history_row_map) / sizeof(history_row_map[0]))) {
      HistoryRowMap *m = &history_row_map[history_row_map_count++];
      m->top_row = row_top;
      m->bottom_row = row_top + rows - 1;
      m->left_col = col_left;
      m->right_col = col_right;
      m->idx = idx;
      // Same right-anchored leave hit zone as the single-column
      // path. Reserves the rightmost ~8 cols (minus the +score
      // tail) on the move row for the leave click target.
      m->leave_right = col_right - 4;
      m->leave_left = m->leave_right - 7;
      if (m->leave_left < col_left) {
        m->leave_left = col_left;
      }
    }
  }
}

// ── Analysis panel ────────────────────────────────────────────────────────
//
// Shows the latest engine leaderboard — sim candidates while the bag
// still has tiles, endgame PVs after it's empty. Ranked candidates
// scroll below the title: each row shows rank, move notation, an
// optional leave column, a "primary" metric (win% in sim, W/T/L in
// endgame) and a "secondary" metric (mean equity in sim, integer
// spread delta in endgame).

// AnalysisRow / AnalysisTint / row + ply caps now live in
// game_state.h so per-turn snapshots stored on TuiHistoryEntry
// can share the same shape with no conversion.

// Render the ranked candidates given a pre-populated row array.
// Handles the leave column auto-sizing, exchange compaction, and
// right-anchored primary/secondary columns. primary_bold gates whether
// the primary string renders in bold (true for win%, false for W/T/L
// which already pop visually).
static void render_analysis_rows(struct ncplane *plane, const Theme *theme,
                                 const TuiGameState *state, const Layout *L,
                                 AnalysisRow *rows, int visible, int primary_w,
                                 int secondary_w, int primary_secondary_gap,
                                 bool primary_bold, int title_end_col) {
  const int interior_left = L->analysis_left + 1;
  const int interior_right_full = L->analysis_right - 1;
  // Reserve the rightmost interior cell for the scrollbar whenever
  // the candidate list overflows the visible window. interior_right
  // (and everything that anchors to it — sec_col, prim_col, avg
  // block, leave column, etc.) shrinks by 1 cell in that case so
  // data doesn't sit underneath the scrollbar track.
  const int total_rows =
      state != NULL ? state->last_rendered_analysis_row_count : 0;
  const bool scrollbar_visible = total_rows > visible;
  const int interior_right =
      scrollbar_visible ? interior_right_full - 1 : interior_right_full;
  const int interior_top = L->analysis_top + 1;
  const int interior_bottom = L->analysis_bottom - 1;
  // Reset per-frame hit-test map + panel bounds so mouse clicks
  // and arrow-key navigation can target individual analysis rows.
  analysis_row_map_count = 0;
  analysis_panel_top = L->analysis_top;
  analysis_panel_bottom = L->analysis_bottom;
  analysis_panel_left = L->analysis_left;
  analysis_panel_right = L->analysis_right;
  // Reserve a column-header strip; either on the panel's top border
  // (sharing the row with the title, when there's room) or on the
  // first interior row. The on-border placement is preferred since
  // it gives the data one extra row of vertical space. Final
  // decision happens below once we know how far the leftmost header
  // would extend; placeholder values here.
  bool show_headers = interior_top <= interior_bottom;
  int header_row = interior_top;
  int list_top = show_headers ? interior_top + 1 : interior_top;

  // Forward declaration only — final values are set further down
  // after rank_w / max_move_w / score_w are known. Initialized to
  // safe defaults so any accidental early read doesn't crash.
  enum { AVG_COL_W = 4, AVG_GAP_W = 1 };

  // Size the rank column to the digit count of the largest visible
  // rank, so a 9-row list shows "9. " (no pad) and only a 10+ list
  // pays for the leading space. Generalizes to 100+ if ever needed.
  int rank_digits = 1;
  {
    int n = visible;
    while (n >= 10) {
      rank_digits++;
      n /= 10;
    }
  }
  char rank_fmt[8];
  snprintf(rank_fmt, sizeof(rank_fmt), "%%%dd. ", rank_digits);
  const int rank_w = rank_digits + 2; // digits + ". "
  const int move_col = interior_left + rank_w;

  const int leave_gap_l = 2;
  // No explicit gap between the leave column and the primary column —
  // the win% format ("%5.1f%%") leaves an implicit leading space
  // unless the value hits exactly 100.0%, which gives a clean 1-col
  // visual gap from the leave for any realistic win percentage.
  const int leave_gap_r = 0;

  // Move text in analysis renders with hide_parens=true, so the
  // rendered column width is strlen minus paren characters. Use the
  // rendered width for layout decisions; using raw strlen
  // overestimates and causes the leave column to get suppressed when
  // a playthrough move's parens push the max over the budget.
  int max_move_w = 0;
  int max_leave_w = 0;
  for (int i = 0; i < visible; i++) {
    if (!rows[i].valid) {
      continue;
    }
    int rendered = 0;
    for (const char *p = rows[i].move; *p != '\0'; p++) {
      if (*p != '(' && *p != ')') {
        rendered++;
      }
    }
    const int ll = (int)strlen(rows[i].leave);
    if (rendered > max_move_w) {
      max_move_w = rendered;
    }
    if (ll > max_leave_w) {
      max_leave_w = ll;
    }
  }

  // Always compact "(exch ABCD)" → "-ABCD" in the analysis panel —
  // the verbose form takes too much horizontal room and the short
  // form is unambiguous next to placement moves like "7F JUTE".
  // Recompute max_move_w after compaction using rendered width
  // (parens hidden), since parens don't show up at render time.
  {
    int new_max = 0;
    for (int i = 0; i < visible; i++) {
      char *s = rows[i].move;
      if (strncmp(s, "(exch ", 6) == 0) {
        char *close_paren = strchr(s, ')');
        if (close_paren != NULL) {
          const int letters_len = (int)(close_paren - (s + 6));
          char tmp[80];
          tmp[0] = '-';
          const int copy = letters_len < (int)sizeof(tmp) - 2
                               ? letters_len
                               : (int)sizeof(tmp) - 2;
          memcpy(tmp + 1, s + 6, (size_t)copy);
          tmp[1 + copy] = '\0';
          const size_t tlen = strlen(tmp);
          const size_t cap = sizeof(rows[i].move) - 1;
          const size_t finalcopy = tlen < cap ? tlen : cap;
          memcpy(s, tmp, finalcopy);
          s[finalcopy] = '\0';
        }
      }
      int ml = 0;
      for (const char *p = s; *p != '\0'; p++) {
        if (*p != '(' && *p != ')') {
          ml++;
        }
      }
      if (ml > new_max) {
        new_max = ml;
      }
    }
    max_move_w = new_max;
  }

  // Now that rank_w + max_move_w are final, decide the avg-block
  // width and the right-side anchor in one pass. score_w isn't
  // known yet but its eventual maximum is 3, so probe with that
  // ceiling here — slightly pessimistic but keeps sec_col / prim_col
  // stable for the rest of the function.
  int avg_block_w_probe_ply = 0;
  for (int i = 0; i < visible; i++) {
    if (rows[i].valid && rows[i].ply_count > avg_block_w_probe_ply) {
      avg_block_w_probe_ply = rows[i].ply_count;
    }
  }
  const int avg_block_w_tentative =
      avg_block_w_probe_ply * (AVG_COL_W + AVG_GAP_W);
  const int avg_need =
      rank_w + max_move_w + 1 + 4 /* score column + gap, ceiling */ +
      primary_secondary_gap + primary_w + secondary_w + avg_block_w_tentative;
  const int avail_w = interior_right - interior_left + 1;
  const bool show_avgs = avg_block_w_probe_ply > 0 && avg_need <= avail_w;
  const int avg_block_w = show_avgs ? avg_block_w_tentative : 0;
  const int max_ply_count = show_avgs ? avg_block_w_probe_ply : 0;
  const int right_anchor =
      show_avgs ? interior_right - avg_block_w : interior_right;
  const int sec_col = right_anchor - secondary_w + 1;
  const int prim_col = sec_col - primary_secondary_gap - primary_w;
  const int avg_left_edge = show_avgs ? right_anchor + 1 : -1;

  // Compact mode: when the standard layout (rank + widest move +
  // primary + spread) doesn't fit the panel, drop to a tight form.
  // Spread is always omitted; everything else is added back in
  // priority order as long as it fits.
  //
  //   level 0: <move>  <int%>
  //   level 1: + rank "N." (no space after period)
  //   level 2: + space after period -> "N. <move>" (first priority)
  //   level 3: + leave column (second priority)
  const int interior_width = interior_right - interior_left + 1;
  const int standard_need =
      rank_w + max_move_w + 1 + primary_w + primary_secondary_gap + secondary_w;
  if (standard_need > interior_width) {
    // Width of the widest integer-percent we'll actually render. "100%"
    // is 4 cols but only matters when a row actually hits it; with
    // every visible row below 100% we size the slot at 3 ("XX%") and
    // get a free column for rank, space, or leave. W/T/L primaries
    // stay 1 col.
    int compact_primary_w = 0;
    for (int i = 0; i < visible; i++) {
      if (!rows[i].valid) {
        continue;
      }
      int len_here;
      if (strchr(rows[i].primary, '.') != NULL) {
        const double val = atof(rows[i].primary);
        int int_pct = (int)(val + 0.5);
        if (int_pct > 100) {
          int_pct = 100;
        }
        if (int_pct < 0) {
          int_pct = 0;
        }
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%d%%", int_pct);
        len_here = (int)strlen(tmp);
      } else {
        const char *src = rows[i].primary;
        while (*src == ' ') {
          src++;
        }
        len_here = (int)strlen(src);
      }
      if (len_here > compact_primary_w) {
        compact_primary_w = len_here;
      }
    }
    if (compact_primary_w == 0) {
      compact_primary_w = 1;
    }
    const int rank_short = rank_digits + 1; // "N."
    const int rank_full = rank_short + 1;   // "N. "
    const int base_need = max_move_w + 1 + compact_primary_w;
    const int level1_need = rank_short + base_need;
    const int level2_need = rank_full + base_need;
    const int level3_need = rank_full + max_move_w + leave_gap_l + max_leave_w +
                            1 + compact_primary_w;

    int level = 0;
    if (level1_need <= interior_width) {
      level = 1;
    }
    if (level2_need <= interior_width) {
      level = 2;
    }
    if (level3_need <= interior_width && max_leave_w > 0) {
      level = 3;
    }

    const int compact_rank_w = (level == 0)   ? 0
                               : (level == 1) ? rank_short
                                              : rank_full;
    const int compact_move_col = interior_left + compact_rank_w;
    const bool compact_show_leave = (level >= 3);
    char rfmt[8];
    if (level >= 1) {
      snprintf(rfmt, sizeof(rfmt), level == 1 ? "%%%dd." : "%%%dd. ",
               rank_digits);
    }

    // Compact mode also gets a "win%" header. Try the panel's top
    // border row first (sharing with the title) — if the title is
    // too long for that, fall back to the first interior row and
    // shift the data rows down by one.
    int compact_header_row = -1;
    {
      const char *win_label = "win%";
      const int win_label_len = (int)strlen(win_label);
      const int win_label_col = interior_right - win_label_len + 1;
      const bool win_fits_on_border =
          title_end_col >= 0 && win_label_col > title_end_col + 1;
      if (win_fits_on_border) {
        compact_header_row = L->analysis_top;
        // list_top was initialised to interior_top+1 expecting an
        // interior header row — reclaim that first interior row
        // for data since the header is up on the title bar.
        list_top = interior_top;
      } else if (interior_top <= interior_bottom) {
        compact_header_row = interior_top;
        list_top = interior_top + 1;
      }
      if (compact_header_row >= 0) {
        // Fade-in to the inverted band: each cell to the left of the
        // label is a left-half-block ▌ (U+258C) whose fg/bg ramp from
        // theme->bg to theme->dim_fg. Same trick the standard-mode
        // header uses, giving 2x gradient resolution per cell.
        const int fade_right = win_label_col - 1;
        // On the title-border row, the title text occupies cells up
        // through title_end_col — start the fade just past it so we
        // don't overwrite "Sim (...)". On an interior fallback row
        // the entire interior is ours to fade across.
        const int fade_left = compact_header_row == L->analysis_top
                                  ? title_end_col + 1
                                  : interior_left;
        if (fade_left <= fade_right) {
          const int fade_w = fade_right - fade_left + 1;
          const int sub_steps = 2 * fade_w;
          for (int c = fade_left; c <= fade_right; c++) {
            const int pos = c - fade_left;
            const int left_sub = 2 * pos;
            const int right_sub = left_sub + 1;
            const double tl = sub_steps > 1
                                  ? (double)left_sub / (double)(sub_steps - 1)
                                  : 1.0;
            const double tr = sub_steps > 1
                                  ? (double)right_sub / (double)(sub_steps - 1)
                                  : 1.0;
            ThemeRgb fg;
            fg.r =
                (uint8_t)(theme->bg.r + (theme->dim_fg.r - theme->bg.r) * tl);
            fg.g =
                (uint8_t)(theme->bg.g + (theme->dim_fg.g - theme->bg.g) * tl);
            fg.b =
                (uint8_t)(theme->bg.b + (theme->dim_fg.b - theme->bg.b) * tl);
            ThemeRgb bg;
            bg.r =
                (uint8_t)(theme->bg.r + (theme->dim_fg.r - theme->bg.r) * tr);
            bg.g =
                (uint8_t)(theme->bg.g + (theme->dim_fg.g - theme->bg.g) * tr);
            bg.b =
                (uint8_t)(theme->bg.b + (theme->dim_fg.b - theme->bg.b) * tr);
            theme_apply_fg(plane, fg);
            theme_apply_bg(plane, bg);
            ncplane_putstr_yx(plane, compact_header_row, c,
                              "\xe2\x96\x8c"); // ▌ left-half block
          }
        }
        // The "win%" label itself sits on the inverted band.
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->dim_fg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, compact_header_row, win_label_col, win_label);
        ncplane_set_styles(plane, 0);
      }
    }

    theme_apply_bg(plane, theme->bg);
    int row = list_top;
    for (int i = 0; i < visible && row <= interior_bottom; i++) {
      if (!rows[i].valid) {
        row++;
        continue;
      }
      // Reformat the primary: percent values (containing '.') round
      // to an integer; W/T/L stay as-is.
      char pbuf[8];
      if (strchr(rows[i].primary, '.') != NULL) {
        const double val = atof(rows[i].primary);
        int int_pct = (int)(val + 0.5);
        if (int_pct > 100) {
          int_pct = 100;
        }
        if (int_pct < 0) {
          int_pct = 0;
        }
        snprintf(pbuf, sizeof(pbuf), "%d%%", int_pct);
      } else {
        const char *src = rows[i].primary;
        while (*src == ' ') {
          src++;
        }
        snprintf(pbuf, sizeof(pbuf), "%s", src);
      }
      const int plen = (int)strlen(pbuf);
      const int pcol = interior_right - plen + 1;

      if (level >= 1) {
        char rstr[8];
        snprintf(rstr, sizeof(rstr), rfmt, i + 1);
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, row, interior_left, rstr);
      }

      // Optional leave column, right-anchored just before the int%.
      const int leave_len = (int)strlen(rows[i].leave);
      int leave_text_col = 0;
      bool this_row_show_leave = false;
      int move_budget;
      if (compact_show_leave && leave_len > 0) {
        leave_text_col = pcol - 1 - leave_len;
        const int with_leave_budget =
            leave_text_col - leave_gap_l - compact_move_col;
        // Count this row's rendered move width.
        int row_rendered = 0;
        for (const char *p = rows[i].move; *p != '\0'; p++) {
          if (*p != '(' && *p != ')') {
            row_rendered++;
          }
        }
        if (with_leave_budget > 0 && row_rendered <= with_leave_budget) {
          this_row_show_leave = true;
          move_budget = with_leave_budget;
        } else {
          move_budget = pcol - compact_move_col - 1;
        }
      } else {
        move_budget = pcol - compact_move_col - 1;
      }

      char *move_text = rows[i].move;
      int rendered = 0;
      for (const char *p = move_text; *p != '\0'; p++) {
        if (*p != '(' && *p != ')') {
          rendered++;
        }
      }
      if (move_budget <= 0) {
        move_text[0] = '\0';
      } else if (rendered > move_budget) {
        int cnt = 0;
        char *p = move_text;
        for (; *p != '\0'; p++) {
          if (*p != '(' && *p != ')') {
            if (cnt >= move_budget) {
              break;
            }
            cnt++;
          }
        }
        *p = '\0';
      }
      if (move_budget > 0 && move_text[0] != '\0') {
        theme_apply_fg(plane, theme->fg);
        render_move_styled(plane, row, compact_move_col, move_text,
                           /*hide_parens=*/true,
                           /*hide_playthrough_parens=*/false);
      }

      if (this_row_show_leave) {
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, row, leave_text_col, rows[i].leave);
      }

      theme_apply_fg(plane, theme->fg);
      if (primary_bold) {
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      }
      ncplane_putstr_yx(plane, row, pcol, pbuf);
      if (primary_bold) {
        ncplane_set_styles(plane, 0);
      }

      row++;
    }
    return;
  }

  // Score column: shown when any visible row has a score and the
  // widest move still fits beside it. Score takes priority over leave
  // — if both can't fit, drop leave first. Width is 2 cols when no
  // score reaches 100, else 3.
  int max_score_int = 0;
  bool any_score = false;
  for (int i = 0; i < visible; i++) {
    if (!rows[i].valid || rows[i].score[0] == '\0') {
      continue;
    }
    any_score = true;
    const int s = atoi(rows[i].score);
    if (s > max_score_int) {
      max_score_int = s;
    }
  }
  int score_w = 0;
  if (any_score) {
    score_w = max_score_int >= 100 ? 3 : 2;
    // Budget for: rank + max_move + 1 (gap) + score + ps_gap + primary + sec
    const int with_score_need = rank_w + max_move_w + 1 + score_w +
                                primary_secondary_gap + primary_w + secondary_w;
    const int avail_width = interior_right - interior_left + 1;
    if (with_score_need > avail_width) {
      score_w = 0;
    }
  }
  const bool show_score = score_w > 0;
  // show_avgs / avg_block_w were finalized earlier (before sec_col
  // and prim_col were derived) so the right_anchor doesn't drift.
  // With avgs to the right of sprd, the leave/score block's right
  // boundary is unchanged (= prim_col).
  const int score_right_edge = show_score ? prim_col - 1 : prim_col;
  const int score_left_edge =
      show_score ? score_right_edge - score_w + 1 : prim_col;

  // Find the max values across visible rows so we can bold the
  // row(s) that achieve each maximum. Score is integer (exact ties
  // legit and all bold); primary/secondary use the raw double so
  // ties at the displayed precision still resolve to a unique winner
  // when the underlying values differ.
  int best_score = INT_MIN;
  double best_primary = -1e300;
  double best_secondary = -1e300;
  bool any_primary = false;
  bool any_secondary = false;
  // Per-ply column maxes. best_ply_avg[k] is the maximum value in
  // avg-column k across all visible, valid rows that ran at least
  // k+1 plies. any_ply_avg[k] gates against the all-empty case.
  double best_ply_avg[MAX_ANALYSIS_PLIES];
  bool any_ply_avg[MAX_ANALYSIS_PLIES];
  for (int k = 0; k < MAX_ANALYSIS_PLIES; k++) {
    best_ply_avg[k] = -1e300;
    any_ply_avg[k] = false;
  }
  for (int i = 0; i < visible; i++) {
    if (!rows[i].valid) {
      continue;
    }
    if (rows[i].score[0] != '\0' && rows[i].score_value > best_score) {
      best_score = rows[i].score_value;
    }
    if (rows[i].primary[0] != '\0') {
      if (!any_primary || rows[i].primary_value > best_primary) {
        best_primary = rows[i].primary_value;
        any_primary = true;
      }
    }
    if (rows[i].secondary[0] != '\0') {
      if (!any_secondary || rows[i].secondary_value > best_secondary) {
        best_secondary = rows[i].secondary_value;
        any_secondary = true;
      }
    }
    for (int k = 0; k < rows[i].ply_count && k < MAX_ANALYSIS_PLIES; k++) {
      if (!any_ply_avg[k] || rows[i].ply_avg[k] > best_ply_avg[k]) {
        best_ply_avg[k] = rows[i].ply_avg[k];
        any_ply_avg[k] = true;
      }
    }
  }
  // Leave's right edge slides left to make room for the score column.
  const int leave_to_score_gap = 1;
  const int leave_right_edge = show_score
                                   ? score_left_edge - leave_to_score_gap - 1
                                   : prim_col - leave_gap_r - 1;
  // All-or-nothing leave column. A previous per-row decision let
  // short-move rows show their leave while wider-move rows dropped
  // theirs — but the random gaps read as "this play was a bingo",
  // which is misleading. So the leave column is enabled only if the
  // widest visible move still fits beside the leave column reserved
  // for the widest leave; otherwise we hide leaves for every row.
  // (Bingos legitimately have empty leave strings — those still
  // render as a blank slot under the column.)
  const int move_budget_with_leaves =
      leave_right_edge - max_leave_w - leave_gap_l - move_col + 1;
  const bool show_leaves =
      max_leave_w > 0 && move_budget_with_leaves >= max_move_w;
  const int full_move_max =
      (show_score ? score_left_edge - 2 : prim_col - 1) - move_col;

  // Column headers above the data rows. We render the strip whenever
  // at least one label has something to say. Sim mode lights up
  // every header (leave / sc / win% / sprd / avg…); play-only mode
  // (loaded GCG with no sim results — primary_w == secondary_w == 0)
  // shows just leave + sc; endgame mode (primary_w == 0,
  // secondary_w == 4) suppresses headers entirely because its
  // W/T/L + spread labels don't fit the strip's style. Each header
  // right-aligns at the same edge as the column it labels.
  const bool has_primary_label = primary_w >= 4;
  const bool has_secondary_label = secondary_w >= 4;
  show_headers = show_headers && (has_primary_label || has_secondary_label ||
                                  show_score || max_leave_w > 0);
  if (show_headers) {
    // Find the leftmost col any header would touch (the "leave"
    // header is the leftmost; if no leave column, "sc" or "win%").
    int leftmost_header_col = INT_MAX;
    if (max_leave_w > 0) {
      const int col = leave_right_edge - 5 + 1; // "leave"
      if (col < leftmost_header_col) {
        leftmost_header_col = col;
      }
    }
    if (show_score) {
      const int len = score_w == 2 ? 2 : 3;
      const int col = score_right_edge - len + 1;
      if (col < leftmost_header_col) {
        leftmost_header_col = col;
      }
    }
    // avg cols are right-anchored past sprd, so they don't affect
    // leftmost_header_col — they sit further right than every other
    // header.
    if (has_primary_label) {
      const int col = prim_col + primary_w - 4; // "win%"
      if (col < leftmost_header_col) {
        leftmost_header_col = col;
      }
    }
    // Prefer the top border row when the title leaves enough room
    // there. title_end_col is the last col of the title's trailing
    // " "; we need a 1-col gap after it before the leftmost header.
    const bool headers_fit_on_border =
        title_end_col >= 0 && leftmost_header_col > title_end_col + 1 &&
        leftmost_header_col >= L->analysis_left + 1;
    if (headers_fit_on_border) {
      header_row = L->analysis_top;
      list_top = interior_top;
    } else {
      header_row = interior_top;
      list_top = interior_top + 1;
    }

    // Paint the header strip. The right portion (from the leftmost
    // header word out to the right interior edge) always uses the
    // inverted band — same look the on-border placement gets. The
    // LEFT portion of an inside-panel strip fades into the panel's
    // bg using DOS-style shade glyphs (░ ▒ ▓) split into thirds:
    //   ░ section at the far left, ▓ section just before the band,
    //   ▒ in the middle. The shade glyph density alone reads as
    //   banded, so we also linearly ramp the foreground color from
    //   bg (invisible) at the far left to dim_fg at the band — the
    //   dithering pattern remains visible but the overall line
    //   fades smoothly into the surrounding rows.
    {
      // Right portion: true inverted band — dark text on the
      // dim_fg-colored band, matching how header_bg / header_fg
      // chrome bars elsewhere read.
      const int band_left =
          headers_fit_on_border ? leftmost_header_col : leftmost_header_col;
      theme_apply_fg(plane, theme->bg);
      theme_apply_bg(plane, theme->dim_fg);
      for (int c = band_left; c <= interior_right; c++) {
        ncplane_putstr_yx(plane, header_row, c, " ");
      }
    }
    if (!headers_fit_on_border && interior_left < leftmost_header_col) {
      // Each cell renders a left-half-block ▌ (U+258C): fg paints
      // the cell's left half, bg paints the right half. That gives
      // us two color samples per terminal cell — twice the
      // gradient resolution of plain spaces — so the fade from
      // theme->bg to theme->dim_fg eases in smoothly even on
      // narrow strips.
      const int fade_left = interior_left;
      const int fade_right = leftmost_header_col - 1;
      const int fade_w = fade_right - fade_left + 1;
      const int sub_steps = 2 * fade_w;
      for (int c = fade_left; c <= fade_right; c++) {
        const int pos = c - fade_left;
        const int left_sub = 2 * pos;
        const int right_sub = left_sub + 1;
        const double tl =
            sub_steps > 1 ? (double)left_sub / (double)(sub_steps - 1) : 1.0;
        const double tr =
            sub_steps > 1 ? (double)right_sub / (double)(sub_steps - 1) : 1.0;
        ThemeRgb fg;
        fg.r = (uint8_t)(theme->bg.r + (theme->dim_fg.r - theme->bg.r) * tl);
        fg.g = (uint8_t)(theme->bg.g + (theme->dim_fg.g - theme->bg.g) * tl);
        fg.b = (uint8_t)(theme->bg.b + (theme->dim_fg.b - theme->bg.b) * tl);
        ThemeRgb bg;
        bg.r = (uint8_t)(theme->bg.r + (theme->dim_fg.r - theme->bg.r) * tr);
        bg.g = (uint8_t)(theme->bg.g + (theme->dim_fg.g - theme->bg.g) * tr);
        bg.b = (uint8_t)(theme->bg.b + (theme->dim_fg.b - theme->bg.b) * tr);
        theme_apply_fg(plane, fg);
        theme_apply_bg(plane, bg);
        ncplane_putstr_yx(plane, header_row, c,
                          "\xe2\x96\x8c"); // ▌ left-half block
      }
    }

    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->dim_fg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    if (show_leaves) {
      const char *leave_label = "leave";
      const int len = (int)strlen(leave_label);
      const int col = leave_right_edge - len + 1;
      if (col >= move_col) {
        ncplane_putstr_yx(plane, header_row, col, leave_label);
      }
    }
    if (show_score) {
      const char *sc_label = score_w == 2 ? "sc" : "scr";
      const int len = (int)strlen(sc_label);
      const int col = score_right_edge - len + 1;
      ncplane_putstr_yx(plane, header_row, col, sc_label);
    }
    if (show_avgs) {
      for (int ply = 0; ply < max_ply_count; ply++) {
        char hdr[8];
        snprintf(hdr, sizeof(hdr), "avg%d", ply + 1);
        const int col =
            avg_left_edge + ply * (AVG_COL_W + AVG_GAP_W) + AVG_GAP_W;
        ncplane_putstr_yx(plane, header_row, col, hdr);
      }
    }
    if (has_primary_label) {
      const char *win_label = "win%";
      const int len = (int)strlen(win_label);
      const int col = prim_col + primary_w - len;
      ncplane_putstr_yx(plane, header_row, col, win_label);
    }
    if (has_secondary_label) {
      const char *sprd_label = "sprd";
      const int len = (int)strlen(sprd_label);
      // Right-align inside the secondary column's slot, not against
      // interior_right — when the avg block is on, sec_col has
      // shifted left to make room for the avgs further right.
      const int col = sec_col + secondary_w - len;
      ncplane_putstr_yx(plane, header_row, col, sprd_label);
    }
    ncplane_set_styles(plane, 0);
  } else {
    list_top = interior_top;
  }

  // Resolve the effective cursor row for this frame. RANK column
  // pins to a row index; MOVE column pins to a specific move and
  // follows it as the sim reorders. effective_cursor is the row
  // index that should be highlighted; in MOVE mode that's the
  // anchored move's current position in rows[].
  const int effective_cursor =
      state != NULL ? effective_analysis_cursor(state) : -1;
  // Scroll window — the rank range currently painted. view_h is
  // the panel's visible row capacity; scroll_offset is the rank
  // index of the first painted row. Auto-scroll adjusts the
  // offset to keep the cursor visible (cursor + view move
  // together).
  const int view_h = visible;
  // total_rows + scrollbar_visible are computed at the top of the
  // function (so interior_right could shrink by 1). Reuse them here.
  int scroll_offset = state != NULL ? state->analysis_scroll_offset : 0;
  if (effective_cursor >= 0) {
    if (effective_cursor < scroll_offset) {
      scroll_offset = effective_cursor;
    }
    if (effective_cursor >= scroll_offset + view_h) {
      scroll_offset = effective_cursor - view_h + 1;
    }
  } else {
    // Cursor is parked on the [5] label (or otherwise inactive).
    // Snap the view back to the top so a stale scroll_offset from
    // a previous interaction doesn't leave the panel scrolled
    // when the user has nothing selected.
    scroll_offset = 0;
  }
  if (scroll_offset > total_rows - view_h) {
    scroll_offset = total_rows - view_h;
  }
  if (scroll_offset < 0) {
    scroll_offset = 0;
  }
  if (state != NULL) {
    ((TuiGameState *)state)->analysis_scroll_offset = scroll_offset;
  }
  // Docking: only when the cursor is in MOVE column AND the
  // anchored move's rank sits outside the currently-scrolled
  // window. With cursor-follow scroll the auto-scroll keeps the
  // cursor in view so this rarely fires for keyboard nav, but a
  // user-driven scroll (wheel / scrollbar drag) that moves the
  // view away from the cursor will re-engage the dock.
  const bool cursor_outside_window =
      effective_cursor >= 0 && (effective_cursor < scroll_offset ||
                                effective_cursor >= scroll_offset + view_h);
  const bool dock_active =
      state != NULL &&
      state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE &&
      cursor_outside_window && view_h >= 2;

  // Number of painted rows for this frame: capped by view_h and
  // by how many ranks remain after the scroll offset.
  const int painted_rows =
      total_rows - scroll_offset < view_h ? total_rows - scroll_offset : view_h;

  theme_apply_bg(plane, theme->bg);
  int row = list_top;
  for (int slot = 0; slot < painted_rows && row <= interior_bottom; slot++) {
    // Translate visible slot index → row data index. Normally
    // data_i = scroll_offset + slot. In dock mode the last slot
    // displays the anchored move's data (which lives at
    // effective_cursor) and the slot immediately above it is
    // rendered as a divider line rather than a row.
    int data_i = scroll_offset + slot;
    if (dock_active) {
      if (slot == painted_rows - 2) {
        // Divider line — uses the dim_fg color so it reads as
        // a subdued separator, not a heavy rule.
        theme_apply_fg(plane, theme->dim_fg);
        theme_apply_bg(plane, theme->bg);
        for (int c = interior_left; c <= interior_right; c++) {
          // ─ (U+2500 BOX DRAWINGS LIGHT HORIZONTAL)
          ncplane_putstr_yx(plane, row, c, "\xe2\x94\x80");
        }
        row++;
        continue;
      }
      if (slot == painted_rows - 1) {
        data_i = effective_cursor;
      }
    }
    if (data_i < 0 || data_i >= state->last_rendered_analysis_row_count ||
        !rows[data_i].valid) {
      row++;
      continue;
    }
    char rank_str[8];
    snprintf(rank_str, sizeof(rank_str), rank_fmt, data_i + 1);

    // Rendered width of this row's move (parens are dropped at
    // render time so they don't count toward layout width).
    int rendered = 0;
    for (const char *p = rows[data_i].move; *p != '\0'; p++) {
      if (*p != '(' && *p != ')') {
        rendered++;
      }
    }

    // Leave column placement is decided globally (show_leaves):
    // either every row shows its own leave at leave_right_edge, or
    // the column is hidden entirely. Rows where the play exhausts
    // the rack (bingos / endgame outplays) have an empty leave —
    // render those as "·" so the column stays consistently
    // populated, otherwise the gaps look like rendering bugs.
    // Re-sort the candidate's leave per the user's rack-sort
    // preference so the leave column lines up with how rack
    // tiles are ordered elsewhere (rack panel, history).
    char sorted_row_leave[24];
    if (rows[data_i].leave[0] != '\0' && state != NULL && state->ld != NULL) {
      format_alphagram_for_sort(rows[data_i].leave, state->ld, state->rack_sort,
                                sorted_row_leave, sizeof(sorted_row_leave));
    } else {
      sorted_row_leave[0] = '\0';
    }
    const char *leave_str =
        sorted_row_leave[0] != '\0' ? sorted_row_leave : "\xc2\xb7";
    const int leave_len = (int)strlen(leave_str);
    const int leave_text_col =
        show_leaves ? leave_right_edge -
                          (rows[data_i].leave[0] != '\0' ? leave_len : 1) + 1
                    : 0;
    const bool show_this_leave = show_leaves;
    const int this_move_max = show_leaves ? leave_right_edge - leave_gap_l -
                                                move_col + 1 - max_leave_w
                                          : full_move_max;

    char *move_text = rows[data_i].move;
    if (this_move_max <= 0) {
      move_text[0] = '\0';
    } else if (rendered > this_move_max) {
      // Truncate against rendered width, not raw strlen — a move
      // like "H2 ISOBU(TA)NE" has strlen 14 but renders as 12
      // ("H2 ISOBUTANE" with hide_parens). Counting parens as
      // billable width would crop the move unnecessarily.
      int cnt = 0;
      char *p = move_text;
      for (; *p != '\0'; p++) {
        if (*p != '(' && *p != ')') {
          if (cnt >= this_move_max) {
            break;
          }
          cnt++;
        }
      }
      *p = '\0';
    }

    // Cursor styling: highlight is column-aware.
    //   - RANK column: invert the rank chip (bg on fg, bold)
    //     and put ">" (focused) or "." (unfocused) after the digits.
    //   - MOVE column: leave the rank chip in dim color and invert
    //     the move text instead.
    // The "cursor here" test compares against effective_cursor —
    // the row currently selected after MOVE-anchor resolution.
    const bool cursor_here = (data_i == effective_cursor);
    // The cursor is "explicit" when state->analysis_cursor is on a
    // real candidate row; -1 means the user is parked on the [5]
    // label and effective_cursor only fell through to 0 to mark
    // the implicit "previewed" row. The inverted/chevron look
    // (the active-cursor cue) only fires when the cursor is
    // explicit AND the panel is focused; otherwise the row
    // renders with the medium-bg parked-selection look.
    const bool cursor_explicit = state != NULL && state->analysis_cursor >= 0;
    const bool panel_focused =
        state != NULL && state->focused_panel == TUI_FOCUS_ANALYSIS;
    const bool focused_here = cursor_explicit && panel_focused;
    const bool cursor_on_rank =
        cursor_here && state != NULL &&
        state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_RANK;
    const bool cursor_on_move =
        cursor_here && state != NULL &&
        state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE;
    if (cursor_on_rank) {
      // Find the position of the '.' in rank_str (right-aligned;
      // walk to end and back). Then split: leading digits +
      // padding go on the chip, the '.' becomes '>' when the
      // panel is focused (stays '.' otherwise), the trailing
      // space goes plain.
      char chip[8];
      int chip_len = 0;
      int after_period_idx = 0;
      for (int k = 0; rank_str[k] != '\0'; k++) {
        if (rank_str[k] == '.') {
          after_period_idx = k + 1;
          break;
        }
      }
      for (int k = 0; k < after_period_idx - 1 && chip_len < 6; k++) {
        chip[chip_len++] = rank_str[k];
      }
      chip[chip_len++] = focused_here ? '>' : '.';
      chip[chip_len] = '\0';
      // Focused panel: full-invert (bg on fg, bold) so the active
      // cursor pops. Unfocused panel: keep the rank readable in
      // normal fg, but sit it on a medium-brightness bg (the
      // on-turn player's tile_bg, same family used for unfocused
      // thinking-turn chips in History) so the row reads as
      // "parked selection, panel not focused" — visually distinct
      // from the active cursor.
      if (focused_here) {
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->fg);
      } else {
        // Parked-cursor chip: same luminance as the player tile_bg
        // used in the History panel's pending chip, but neutral
        // gray — the Analysis row shouldn't pick up the player's
        // green / amber tint when it's just marking the top move,
        // since at that point it's a cursor cue rather than a
        // "this tile belongs to player X" indicator.
        const int candidate_idx = rows[data_i].candidate_player_idx;
        const ThemeRgb tile_bg =
            candidate_idx == 1 ? theme->tile2_bg : theme->tile1_bg;
        const uint8_t lum =
            (uint8_t)((30u * tile_bg.r + 59u * tile_bg.g + 11u * tile_bg.b) /
                      100u);
        const ThemeRgb gray_bg = {lum, lum, lum};
        theme_apply_fg(plane, theme->fg);
        theme_apply_bg(plane, gray_bg);
      }
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row, interior_left, chip);
      ncplane_set_styles(plane, 0);
      theme_apply_fg(plane, theme->dim_fg);
      theme_apply_bg(plane, theme->bg);
      if (rank_str[after_period_idx] != '\0') {
        ncplane_putstr_yx(plane, row, interior_left + after_period_idx,
                          rank_str + after_period_idx);
      }
    } else {
      theme_apply_fg(plane, theme->dim_fg);
      ncplane_putstr_yx(plane, row, interior_left, rank_str);
    }
    // Record the row's screen rectangle for click-to-cursor.
    if (analysis_row_map_count <
        (int)(sizeof(analysis_row_map) / sizeof(analysis_row_map[0]))) {
      AnalysisRowMap *m = &analysis_row_map[analysis_row_map_count++];
      m->top_row = row;
      m->bottom_row = row;
      m->left_col = interior_left;
      m->right_col = interior_right;
      m->move_left_col = move_col;
      m->idx = data_i;
    }

    if (this_move_max > 0 && move_text[0] != '\0') {
      if (cursor_on_move) {
        // Invert the move text (bg on fg, bold). The inverted
        // background by itself is enough of a selection cue —
        // no chevron is appended on the move side. render_move_styled
        // would re-apply its own colors, so paint a backing
        // rectangle in inverse colors first and then write the
        // move text with the inverted palette.
        const int move_render_w = this_move_max;
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->fg);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        for (int c = 0; c < move_render_w; c++) {
          ncplane_putstr_yx(plane, row, move_col + c, " ");
        }
        ncplane_putstr_yx(plane, row, move_col, move_text);
        ncplane_set_styles(plane, 0);
        theme_apply_bg(plane, theme->bg);
      } else {
        theme_apply_fg(plane, theme->fg);
        render_move_styled(plane, row, move_col, move_text,
                           /*hide_parens=*/true,
                           /*hide_playthrough_parens=*/false);
      }
    }

    if (show_this_leave) {
      theme_apply_fg(plane, theme->dim_fg);
      ncplane_putstr_yx(plane, row, leave_text_col, leave_str);
    }

    if (show_score && rows[data_i].score[0] != '\0') {
      const int sl = (int)strlen(rows[data_i].score);
      const int sc_col = score_right_edge - sl + 1;
      const bool is_best = (rows[data_i].score_value == best_score);
      theme_apply_fg(plane, theme->fg);
      if (is_best) {
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      }
      ncplane_putstr_yx(plane, row, sc_col, rows[data_i].score);
      if (is_best) {
        ncplane_set_styles(plane, 0);
      }
    }

    // Per-ply averages. Ply 0 is the candidate-player's move (on-
    // turn at evaluation time); subsequent plies alternate. Color
    // each column by whose turn that ply was, using the same per-
    // player accent the player pill uses.
    if (show_avgs && rows[data_i].ply_count > 0) {
      const int candidate_idx = rows[data_i].candidate_player_idx;
      for (int ply = 0; ply < rows[data_i].ply_count; ply++) {
        const int ply_player = (candidate_idx + ply) % 2;
        const ThemeRgb ply_color =
            ply_player == 1 ? theme->on_turn_fg_p2 : theme->on_turn_fg;
        const int col =
            avg_left_edge + ply * (AVG_COL_W + AVG_GAP_W) + AVG_GAP_W;
        // Pick "12" vs "12.3" so the value fits in AVG_COL_W cells.
        char buf[16];
        const double v = rows[data_i].ply_avg[ply];
        if (v >= 100.0 || v <= -10.0) {
          snprintf(buf, sizeof(buf), "%*.0f", AVG_COL_W, v);
        } else {
          snprintf(buf, sizeof(buf), "%*.1f", AVG_COL_W, v);
        }
        const bool is_best = ply < MAX_ANALYSIS_PLIES && any_ply_avg[ply] &&
                             v == best_ply_avg[ply];
        theme_apply_fg(plane, ply_color);
        ncplane_set_styles(plane, is_best ? NCSTYLE_BOLD : 0);
        ncplane_putstr_yx(plane, row, col, buf);
      }
      ncplane_set_styles(plane, 0);
      theme_apply_fg(plane, theme->fg);
    }

    // Primary column (win% or W/T/L). Right-justified within its slot
    // so single-char W/T/L lines up with the right edge.
    {
      const int len = (int)strlen(rows[data_i].primary);
      const int col = sec_col - primary_secondary_gap - len;
      theme_apply_fg(plane, theme->fg);
      const bool is_best = any_primary && rows[data_i].primary[0] != '\0' &&
                           rows[data_i].primary_value == best_primary;
      const bool bold = primary_bold || is_best;
      if (bold) {
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      }
      ncplane_putstr_yx(plane, row, col, rows[data_i].primary);
      if (bold) {
        ncplane_set_styles(plane, 0);
      }
    }

    // Secondary column (equity or spread). Stays in the dim color
    // even when bolded — the spread column reads as supplementary
    // info next to the white win%, and switching it to full white
    // when bolded made it shout louder than win%.
    {
      const int len = (int)strlen(rows[data_i].secondary);
      // Mirror the header: right-align within sec_col's slot rather
      // than against interior_right, so the column tracks sec_col
      // when it shifts left to make room for the avg block.
      const int col = sec_col + secondary_w - len;
      const bool is_best = any_secondary && rows[data_i].secondary[0] != '\0' &&
                           rows[data_i].secondary_value == best_secondary;
      theme_apply_fg(plane, theme->dim_fg);
      if (is_best) {
        ncplane_set_styles(plane, NCSTYLE_BOLD);
      }
      ncplane_putstr_yx(plane, row, col, rows[data_i].secondary);
      if (is_best) {
        ncplane_set_styles(plane, 0);
      }
    }

    row++;
  }
  if (state != NULL) {
    atomic_store(&((TuiGameState *)state)->analysis_visible_rows,
                 analysis_row_map_count);
  }

  // Scrollbar — right edge of the panel interior. Renders a track
  // across the scroll-window rows with a thumb proportional to
  // visible/total, using LEFT N/8 BLOCK chars on the track and
  // LOWER N/8 BLOCK chars for the thumb's fractional top/bottom
  // edges (gives ~1/8-row visual precision).
  if (scrollbar_visible && state != NULL && view_h > 0 && total_rows > 0) {
    const int scrollbar_col = interior_right + 1;
    const int track_top = list_top;
    const int track_bottom = list_top + view_h - 1;
    const int track_h = view_h;
    // Thumb position in 1/8 row units relative to the track.
    const int total_eighths = track_h * 8;
    int thumb_top_8 =
        (int)((long long)scroll_offset * total_eighths / total_rows);
    int thumb_bot_8 =
        (int)((long long)(scroll_offset + view_h) * total_eighths / total_rows);
    if (thumb_bot_8 > total_eighths) {
      thumb_bot_8 = total_eighths;
    }
    if (thumb_top_8 < 0) {
      thumb_top_8 = 0;
    }
    // Ensure the thumb always shows at least 1/8 of a cell so the
    // user can see something even when total_rows >> view_h.
    if (thumb_bot_8 - thumb_top_8 < 1) {
      thumb_bot_8 = thumb_top_8 + 1;
      if (thumb_bot_8 > total_eighths) {
        thumb_bot_8 = total_eighths;
        thumb_top_8 = thumb_bot_8 - 1;
      }
    }
    // Single thumb color so every pixel of the bar reads as
    // exactly the same shade regardless of which glyph variant
    // the cell uses (full block, lower-N, inverted upper-N).
    // Pulls from the theme so dark / light / etc. palettes all
    // theme the bar appropriately; only the *uniformity* across
    // cells is the constraint here.
    const ThemeRgb thumb_color = theme->fg;
    static const char *const lower_blocks[9] = {
        "\xe2\x96\x8f", // ▏ track (LEFT ONE EIGHTH) — used as track sliver
        "\xe2\x96\x81", // ▁ lower 1/8
        "\xe2\x96\x82", // ▂ lower 2/8
        "\xe2\x96\x83", // ▃ lower 3/8
        "\xe2\x96\x84", // ▄ lower 4/8 (half)
        "\xe2\x96\x85", // ▅ lower 5/8
        "\xe2\x96\x86", // ▆ lower 6/8
        "\xe2\x96\x87", // ▇ lower 7/8
        "\xe2\x96\x88", // █ full
    };
    for (int r = 0; r < track_h; r++) {
      const int cell_top_8 = r * 8;
      const int cell_bot_8 = cell_top_8 + 8;
      int overlap_top_8 = thumb_top_8 > cell_top_8 ? thumb_top_8 : cell_top_8;
      int overlap_bot_8 = thumb_bot_8 < cell_bot_8 ? thumb_bot_8 : cell_bot_8;
      const int in_cell_top = overlap_top_8 - cell_top_8;
      const int in_cell_bot = overlap_bot_8 - cell_top_8;
      const int fill_eighths =
          overlap_bot_8 > overlap_top_8 ? overlap_bot_8 - overlap_top_8 : 0;
      if (fill_eighths <= 0) {
        // Cell entirely outside the thumb. Paint as panel bg so
        // the track shares the same color as the non-thumb
        // portions of partial cells — gives a clean seam at the
        // thumb's edges. The thumb is the only visible mark of
        // the scrollbar; clicks elsewhere in the column still
        // hit-test via the published geometry.
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, theme->bg);
        ncplane_putstr_yx(plane, track_top + r, scrollbar_col, " ");
        continue;
      }
      if (fill_eighths == 8) {
        // Cell entirely inside the thumb → full block.
        theme_apply_fg(plane, thumb_color);
        theme_apply_bg(plane, theme->bg);
        ncplane_putstr_yx(plane, track_top + r, scrollbar_col, lower_blocks[8]);
        continue;
      }
      if (in_cell_top == 0) {
        // Thumb fills FROM THE TOP downward to in_cell_bot/8. To
        // get top-filled-N/8 we render a lower-(8-N) block with
        // fg=panel_bg, bg=thumb_color. ON pixels (bottom portion)
        // render in panel_bg so the cell's non-thumb region
        // matches the surrounding track exactly — no dim-gray
        // band appears at the thumb's upper edge.
        const int inv_idx = 8 - in_cell_bot;
        theme_apply_fg(plane, theme->bg);
        theme_apply_bg(plane, thumb_color);
        ncplane_putstr_yx(plane, track_top + r, scrollbar_col,
                          lower_blocks[inv_idx]);
      } else if (in_cell_bot == 8) {
        // Thumb fills FROM THE BOTTOM upward. ON pixels = thumb,
        // OFF pixels = panel_bg (track).
        const int idx = 8 - in_cell_top;
        theme_apply_fg(plane, thumb_color);
        theme_apply_bg(plane, theme->bg);
        ncplane_putstr_yx(plane, track_top + r, scrollbar_col,
                          lower_blocks[idx]);
      } else {
        // Thumb sits entirely in the middle of the cell (rare —
        // happens only when total_rows is large enough that the
        // thumb is shorter than 1/8 of a cell). Render a half-
        // block thumb centered visually.
        theme_apply_fg(plane, thumb_color);
        theme_apply_bg(plane, theme->bg);
        ncplane_putstr_yx(plane, track_top + r, scrollbar_col, "\xe2\x96\x84");
      }
    }
    // Publish geometry so main.c's input handlers can hit-test
    // mouse clicks against the scrollbar.
    TuiGameState *mut = (TuiGameState *)state;
    atomic_store(&mut->analysis_scrollbar_top, track_top);
    atomic_store(&mut->analysis_scrollbar_bottom, track_bottom);
    atomic_store(&mut->analysis_scrollbar_col, scrollbar_col);
    atomic_store(&mut->analysis_scrollbar_total, total_rows);
    atomic_store(&mut->analysis_scrollbar_view, view_h);
  } else if (state != NULL) {
    // Hidden scrollbar — publish zero so input handlers know not
    // to try to hit-test against stale geometry.
    TuiGameState *mut = (TuiGameState *)state;
    atomic_store(&mut->analysis_scrollbar_total, 0);
    atomic_store(&mut->analysis_scrollbar_view, 0);
  }
}

// Populate the row cache from the sim leaderboard. Returns the
// number of rows filled (≤ max_rows).
static int fill_analysis_rows_from_sim(const TuiGameState *state,
                                       AnalysisRow *rows, int max_rows) {
  SimResults *results = state->sim_results;
  if (results == NULL ||
      !sim_results_lock_and_sort_display_simmed_plays(results)) {
    return 0;
  }
  const int num_plays = sim_results_get_number_of_plays(results);
  int n = num_plays < max_rows ? num_plays : max_rows;
  const Board *board = game_get_board(state->game);
  // Read the rack from the live game (the on-turn player's rack at
  // the moment we're rendering, which during a sim is still the
  // pre-play rack since play_move runs only after the sim returns).
  // The sim_results internal rack is also valid in principle, but
  // sourcing it from the game directly removes a memory-ordering
  // dependency on the simmer's writes from another thread.
  const int on_turn = game_get_player_on_turn_index(state->game);
  const Rack *sim_rack = player_get_rack(game_get_player(state->game, on_turn));
  for (int i = 0; i < n; i++) {
    rows[i].valid = false;
    rows[i].move[0] = '\0';
    rows[i].leave[0] = '\0';
    rows[i].score[0] = '\0';
    rows[i].primary[0] = '\0';
    rows[i].secondary[0] = '\0';
    rows[i].ply_count = 0;
    const SimmedPlay *play = sim_results_get_display_simmed_play(results, i);
    if (play == NULL) {
      continue;
    }
    const Move *move = simmed_play_get_move(play);
    const double win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(play)) * 100.0;
    const double eq_pts = stat_get_mean(simmed_play_get_equity_stat(play));
    // When the value rounds to 100.0%, drop the decimal so the cell
    // shows "  100%" instead of "100.0%". Same 6-col width, but
    // preserves the leading-space pad that the leave column relies
    // on for its 1-col visual gap from the win%.
    if (win_pct >= 99.95) {
      snprintf(rows[i].primary, sizeof(rows[i].primary), "  100%%");
    } else {
      snprintf(rows[i].primary, sizeof(rows[i].primary), "%5.1f%%", win_pct);
    }
    snprintf(rows[i].secondary, sizeof(rows[i].secondary), "%+6.1f", eq_pts);
    const int play_score = equity_to_int(move_get_score(move));
    snprintf(rows[i].score, sizeof(rows[i].score), "%d", play_score);
    rows[i].score_value = play_score;
    rows[i].primary_value = win_pct;
    rows[i].secondary_value = eq_pts;

    // Per-ply average move-score (mean of every iteration's move-
    // score at that ply, including 0 for pass / exchange). Capped at
    // MAX_ANALYSIS_PLIES so the row stays small.
    const int sim_plies = sim_results_get_num_plies(results);
    const int plies_to_store =
        sim_plies < MAX_ANALYSIS_PLIES ? sim_plies : MAX_ANALYSIS_PLIES;
    rows[i].ply_count = plies_to_store;
    rows[i].candidate_player_idx = on_turn;
    for (int ply = 0; ply < plies_to_store; ply++) {
      const Stat *score_stat = simmed_play_get_score_stat(play, ply);
      rows[i].ply_avg[ply] = score_stat ? stat_get_mean(score_stat) : 0.0;
    }

    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, board, move, state->ld, false);
    size_t mlen = 0;
    char *mdump = string_builder_dump(sb, &mlen);
    if (mdump != NULL) {
      const size_t copy =
          mlen < sizeof(rows[i].move) ? mlen : sizeof(rows[i].move) - 1;
      memcpy(rows[i].move, mdump, copy);
      rows[i].move[copy] = '\0';
      free(mdump);
    }
    string_builder_destroy(sb);

    if (sim_rack != NULL) {
      StringBuilder *lsb = string_builder_create();
      string_builder_add_move_leave(lsb, sim_rack, move, state->ld);
      size_t llen = 0;
      char *ldump = string_builder_dump(lsb, &llen);
      if (ldump != NULL) {
        const size_t copy =
            llen < sizeof(rows[i].leave) ? llen : sizeof(rows[i].leave) - 1;
        memcpy(rows[i].leave, ldump, copy);
        rows[i].leave[copy] = '\0';
        free(ldump);
      }
      string_builder_destroy(lsb);
    }
    rows[i].valid = true;
  }
  sim_results_unlock_display_infos(results);
  return n;
}

// Populate the row cache from the most recent endgame snapshot. The
// snapshot owns its own Board, Rack, and Move array (captured when
// the solve finished, before play_move shifted the live state), so
// this is safe to call any time the snapshot is marked valid.
static int fill_analysis_rows_from_endgame(const TuiGameState *state,
                                           AnalysisRow *rows, int max_rows) {
  const TuiEndgameSnapshot *snap = &state->endgame_snapshot;
  if (!snap->valid || snap->num_entries <= 0 || snap->board == NULL) {
    return 0;
  }
  const int n = snap->num_entries < max_rows ? snap->num_entries : max_rows;
  for (int i = 0; i < n; i++) {
    rows[i].valid = false;
    rows[i].move[0] = '\0';
    rows[i].leave[0] = '\0';
    rows[i].score[0] = '\0';
    rows[i].primary[0] = '\0';
    rows[i].secondary[0] = '\0';
    rows[i].ply_count = 0;
    const Move *move = snap->moves[i];
    if (move == NULL) {
      continue;
    }
    const int value = snap->values[i];
    // No W/L/T column for endgame anymore — the signed spread
    // already conveys win/loss, and the W/L being a single char
    // got eaten by the leave column visually.
    rows[i].primary[0] = '\0';
    snprintf(rows[i].secondary, sizeof(rows[i].secondary), "%+d", value);

    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, snap->board, move, state->ld, false);
    size_t mlen = 0;
    char *mdump = string_builder_dump(sb, &mlen);
    if (mdump != NULL) {
      const size_t copy =
          mlen < sizeof(rows[i].move) ? mlen : sizeof(rows[i].move) - 1;
      memcpy(rows[i].move, mdump, copy);
      rows[i].move[copy] = '\0';
      free(mdump);
    }
    string_builder_destroy(sb);

    if (snap->solve_rack != NULL) {
      StringBuilder *lsb = string_builder_create();
      string_builder_add_move_leave(lsb, snap->solve_rack, move, state->ld);
      size_t llen = 0;
      char *ldump = string_builder_dump(lsb, &llen);
      if (ldump != NULL) {
        const size_t copy =
            llen < sizeof(rows[i].leave) ? llen : sizeof(rows[i].leave) - 1;
        memcpy(rows[i].leave, ldump, copy);
        rows[i].leave[copy] = '\0';
        free(ldump);
      }
      string_builder_destroy(lsb);
    }
    rows[i].valid = true;
  }
  return n;
}

// Populate state->last_rendered_analysis_rows for the current
// frame. Called once at the top of tui_game_render (before
// render_board, so the on-board candidate preview can resolve
// the cursor against this frame's row order even after a sim
// re-sort). Picks source the same way render_analysis_panel
// does: saved snapshot when the History cursor sits on a
// committed entry, otherwise live sim or endgame results.
static void populate_frame_analysis_rows(const TuiGameState *cstate) {
  TuiGameState *state = (TuiGameState *)cstate;
  if (state == NULL) {
    return;
  }
  state->last_rendered_analysis_row_count = 0;
  const TuiAnalysisSnapshot *snap = NULL;
  if (state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *e = &state->history[state->history_cursor];
    if (!e->pending && e->analysis_snapshot.valid) {
      snap = &e->analysis_snapshot;
    }
  }
  if (snap != NULL) {
    const int n =
        snap->num_rows < ANALYSIS_ROW_CAP ? snap->num_rows : ANALYSIS_ROW_CAP;
    memcpy(state->last_rendered_analysis_rows, snap->rows,
           sizeof(AnalysisRow) * (size_t)n);
    state->last_rendered_analysis_row_count = n;
    return;
  }
  const bool bag_empty =
      state->game != NULL && bag_get_letters(game_get_bag(state->game)) == 0;
  const bool use_endgame = bag_empty && state->endgame_snapshot.valid &&
                           state->endgame_snapshot.num_entries > 0;
  if (use_endgame) {
    state->last_rendered_analysis_row_count = fill_analysis_rows_from_endgame(
        state, state->last_rendered_analysis_rows, ANALYSIS_ROW_CAP);
  } else if (state->sim_results != NULL) {
    // Gate sim row population on sim_results_turn_idx — the
    // reset paths flip it to -1 to signal "the sim_results
    // object's contents aren't valid for the current game".
    // The engine doesn't zero sim_results itself, so without
    // this check the prior game's leaderboard keeps rendering
    // (and burning per-frame CPU) after annotation start.
    const int sim_turn_idx = atomic_load(&state->sim_results_turn_idx);
    if (sim_turn_idx >= 0) {
      state->last_rendered_analysis_row_count = fill_analysis_rows_from_sim(
          state, state->last_rendered_analysis_rows, ANALYSIS_ROW_CAP);
    }
  }

  // Fallback: nothing computed (loaded GCG turn that wasn't simmed
  // by this TUI), but the cursor is parked on a committed entry —
  // surface the played move + leave + score as a single
  // "Analysis" row so the panel isn't blank while reviewing.
  if (state->last_rendered_analysis_row_count == 0 &&
      state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *e = &state->history[state->history_cursor];
    if (!e->pending && e->move_str[0] != '\0') {
      AnalysisRow *row = &state->last_rendered_analysis_rows[0];
      memset(row, 0, sizeof(*row));
      snprintf(row->move, sizeof(row->move), "%s", e->move_str);
      snprintf(row->leave, sizeof(row->leave), "%s", e->leave_str);
      snprintf(row->score, sizeof(row->score), "%d", e->score);
      row->score_value = e->score;
      row->candidate_player_idx = e->player_idx;
      row->valid = true;
      state->last_rendered_analysis_row_count = 1;
    }
  }
}

void tui_capture_analysis_snapshot(const TuiGameState *state,
                                   TuiAnalysisSnapshot *out) {
  if (state == NULL || out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  const bool bag_empty =
      state->game != NULL && bag_get_letters(game_get_bag(state->game)) == 0;
  const bool use_endgame = bag_empty && state->endgame_snapshot.valid &&
                           state->endgame_snapshot.num_entries > 0;
  if (use_endgame) {
    out->is_sim = false;
    out->num_rows =
        fill_analysis_rows_from_endgame(state, out->rows, ANALYSIS_ROW_CAP);
    out->endgame_depth = state->endgame_snapshot.depth;
    out->endgame_exhaustive = state->endgame_snapshot.exhaustive;
    if (state->endgame_ctx != NULL) {
      out->endgame_nodes = endgame_ctx_get_nodes_searched(state->endgame_ctx);
    }
    out->valid = out->num_rows > 0;
  } else if (state->sim_results != NULL) {
    out->is_sim = true;
    out->num_rows =
        fill_analysis_rows_from_sim(state, out->rows, ANALYSIS_ROW_CAP);
    out->sim_plies = sim_results_get_num_plies(state->sim_results);
    out->sim_iterations = sim_results_get_iteration_count(state->sim_results);
    out->sim_nodes = out->sim_iterations * (uint64_t)(out->sim_plies + 1);
    out->valid = out->num_rows > 0;
  }
}

static void render_analysis_panel(struct ncplane *plane, const Theme *theme,
                                  const TuiGameState *state, const Layout *L) {
  if (!L->has_analysis) {
    return;
  }
  const int height = L->analysis_bottom - L->analysis_top + 1;
  const int width = L->analysis_right - L->analysis_left + 1;
  if (height < 3 || width < 6) {
    return;
  }

  // If the History cursor is on a committed entry with a saved
  // analysis snapshot, the panel renders THAT saved leaderboard
  // (with its frozen title meta) instead of the live solve. The
  // live (in-flight pending) turn still falls through to the live
  // path so the user sees the bot's progress in real time.
  const TuiAnalysisSnapshot *snap = NULL;
  if (state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *cursor_entry =
        &state->history[state->history_cursor];
    if (!cursor_entry->pending && cursor_entry->analysis_snapshot.valid) {
      snap = &cursor_entry->analysis_snapshot;
    }
  }

  // Pick mode: when replaying a snapshot, use whatever mode it
  // captured. Otherwise, endgame when the bag has run dry (and we
  // have a saved snapshot from a completed solve); else sim. The
  // live endgame snapshot persists across turns and through game-
  // over, so the last-completed endgame analysis stays on screen
  // after the game ends — which is what you want to study a
  // finished game.
  const bool bag_empty =
      state->game != NULL && bag_get_letters(game_get_bag(state->game)) == 0;
  const bool use_endgame = snap != NULL
                               ? !snap->is_sim
                               : (bag_empty && state->endgame_snapshot.valid &&
                                  state->endgame_snapshot.num_entries > 0);

  // Title varies by mode.
  char title[64];
  if (use_endgame) {
    int depth_to_show = 0;
    uint64_t nodes = 0;
    bool searching = false;
    if (snap != NULL) {
      depth_to_show = snap->endgame_depth;
      nodes = snap->endgame_nodes;
    } else {
      const int snap_depth = state->endgame_snapshot.depth;
      searching = atomic_load(&((TuiGameState *)state)->endgame_results_active);
      // Compact "Endgame (d7/123K)" — depth on the left, total nodes
      // searched on the right (humanized the same way Sim humanizes
      // sample counts). During an active search, prefer the live
      // current-depth atomic over the snapshot's depth so the title
      // ticks up as iterative deepening progresses.
      int cur_depth = 0;
      if (state->endgame_ctx != NULL && searching) {
        int done = 0;
        int total = 0;
        int dummy_a = 0;
        int dummy_b = 0;
        endgame_ctx_get_progress(state->endgame_ctx, &cur_depth, &done, &total,
                                 &dummy_a, &dummy_b);
      }
      depth_to_show = cur_depth > 0 ? cur_depth : snap_depth;
      if (state->endgame_ctx != NULL) {
        nodes = endgame_ctx_get_nodes_searched(state->endgame_ctx);
      }
    }
    char nodes_str[16];
    nodes_str[0] = '\0';
    if (nodes > 0) {
      format_count_compact(nodes, nodes_str, sizeof(nodes_str));
    }
    if (depth_to_show > 0 && nodes_str[0] != '\0') {
      snprintf(title, sizeof(title), "Endgame (d%d/%s)", depth_to_show,
               nodes_str);
    } else if (depth_to_show > 0) {
      snprintf(title, sizeof(title), "Endgame (d%d)", depth_to_show);
    } else if (searching) {
      snprintf(title, sizeof(title), "Endgame (starting\xe2\x80\xa6)");
    } else {
      snprintf(title, sizeof(title), "Endgame");
    }
  } else if (snap != NULL) {
    if (snap->sim_plies > 0 && snap->sim_iterations > 0) {
      char nodes_str[16];
      format_count_compact(snap->sim_nodes, nodes_str, sizeof(nodes_str));
      snprintf(title, sizeof(title), "Sim (%dp/%s)", snap->sim_plies,
               nodes_str);
    } else {
      snprintf(title, sizeof(title), "Sim");
    }
  } else if (state->sim_results != NULL) {
    const int sim_turn_idx_title =
        atomic_load(&((TuiGameState *)state)->sim_results_turn_idx);
    const int plies = sim_results_get_num_plies(state->sim_results);
    const uint64_t iters = sim_results_get_iteration_count(state->sim_results);
    if (sim_turn_idx_title >= 0 && plies > 0 && iters > 0) {
      // Report node count instead of sample count so the unit lines
      // up with what Endgame reports. Each iteration visits the root
      // plus `plies` plies, so nodes ≈ iters * (plies + 1).
      const uint64_t nodes = iters * (uint64_t)(plies + 1);
      char nodes_str[16];
      format_count_compact(nodes, nodes_str, sizeof(nodes_str));
      snprintf(title, sizeof(title), "Sim (%dp/%s)", plies, nodes_str);
    } else {
      // sim_results exists but no real sim ran (e.g. loaded GCG
      // viewer or post-reset state). The panel is in fallback
      // "just-the-played-move" mode, so it's a Plays log rather
      // than analysis output.
      snprintf(title, sizeof(title), "Plays");
    }
  } else {
    snprintf(title, sizeof(title), "Plays");
  }
  // play_only_fallback: not a saved snapshot, not endgame, not an
  // active sim — we're showing just the played move (or "(no
  // analysis yet)" if even that's missing). Used below to drop
  // the win% and sprd columns: there's no probabilistic data to
  // populate them, and showing empty columns is visual noise.
  // sim_results contents persist across game resets (the engine
  // doesn't clear them — sim_results_reset wants a MoveList we
  // don't have here). Use sim_results_turn_idx as the "is the
  // current sim_results actually OURS" gate: the reset paths
  // (annotation start, game load, etc.) flip it to -1, after
  // which we should treat the panel as empty even if the
  // ply/iteration counters are still non-zero from a prior run.
  const int sim_turn_idx =
      atomic_load(&((TuiGameState *)state)->sim_results_turn_idx);
  const bool sim_has_data =
      sim_turn_idx >= 0 && state->sim_results != NULL &&
      sim_results_get_num_plies(state->sim_results) > 0 &&
      sim_results_get_iteration_count(state->sim_results) > 0;
  const bool play_only_fallback = snap == NULL && !use_endgame && !sim_has_data;
  const bool analysis_focused = state->focused_panel == TUI_FOCUS_ANALYSIS;
  // badge_secondary: the cursor has navigated off the label onto a
  // specific candidate row, so "[5>" → "[5]" while focus stays
  // bright. Mirrors the History panel.
  const bool analysis_badge_secondary = state->analysis_cursor >= 0;
  draw_box_styled_ex(plane, theme, L->analysis_top, L->analysis_left, height,
                     width, title, TUI_FOCUS_ANALYSIS, analysis_focused,
                     analysis_badge_secondary);

  const int interior_left = L->analysis_left + 1;
  const int interior_right = L->analysis_right - 1;
  const int interior_top = L->analysis_top + 1;
  const int interior_bottom = L->analysis_bottom - 1;
  // Fill into the full interior — render_analysis_rows will decide
  // whether the header strip lives on the top border (sharing a row
  // with the title) or on the first interior row, and place data
  // rows accordingly.
  const int max_visible = interior_bottom - interior_top + 1;
  const int list_top_for_empty = interior_top;
  // Column of the cell just after the title's closing " ". draw_box
  // paints " " + title + " " starting at left_col + 2 (see ~ line
  // 620), so the next free cell on the top border is at:
  //   analysis_left + 2 + 1 + strlen(title) + 1 = analysis_left + len(title)
  //   + 4.
  const int title_end_col = L->analysis_left + (int)strlen(title) + 3;

  // Rows for this frame were prepared by populate_frame_analysis_rows
  // (called from tui_game_render before any panel renders). The
  // panel's visible window caps that list to whatever fits inside
  // the panel's interior.
  const int cap =
      max_visible < ANALYSIS_ROW_CAP ? max_visible : ANALYSIS_ROW_CAP;
  const int total_rows = state->last_rendered_analysis_row_count;
  int visible = total_rows < cap ? total_rows : cap;

  int primary_w;
  int secondary_w;
  int primary_secondary_gap;
  bool primary_bold;
  if (use_endgame) {
    primary_w = 0;             // no W/T/L column
    secondary_w = 4;           // "+999" / "-100"
    primary_secondary_gap = 0; // no primary, no inter-column gap
    primary_bold = false;
  } else if (play_only_fallback) {
    // Plays mode (loaded GCG, no sim/endgame data) — drop the
    // win% and sprd columns entirely since they have nothing to
    // show. The play row needs only move + leave + score.
    primary_w = 0;
    secondary_w = 0;
    primary_secondary_gap = 0;
    primary_bold = false;
  } else {
    primary_w = 6;             // "100.0%"
    secondary_w = 6;           // "%+6.1f" → " -19.9"
    primary_secondary_gap = 0; // %+6.1f leading pad provides the gap
    primary_bold = false;
  }

  if (visible == 0) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const char *msg = "(no analysis yet)";
    (void)use_endgame; // both modes share the same empty-state copy
    const int msg_col =
        interior_left +
        (interior_right - interior_left + 1 - (int)strlen(msg)) / 2;
    if (list_top_for_empty <= interior_bottom) {
      ncplane_putstr_yx(plane, list_top_for_empty, msg_col, msg);
    }
    return;
  }

  // render_analysis_rows mutates row.move in place (truncation + exch
  // compaction), so feed it a local copy of the prepared rows so the
  // pristine copies in state->last_rendered_analysis_rows are still
  // usable for MOVE-anchor lookups later in the frame and in input
  // handlers.
  // Copy the FULL row set, not just the visible window: with
  // scrolling the renderer indexes by absolute rank
  // (scroll_offset + slot), so anything past `visible` would be
  // read past the live data into garbage if we only copied the
  // visible portion.
  AnalysisRow rows[ANALYSIS_ROW_CAP];
  memcpy(rows, state->last_rendered_analysis_rows,
         sizeof(AnalysisRow) * (size_t)total_rows);
  render_analysis_rows(plane, theme, state, L, rows, visible, primary_w,
                       secondary_w, primary_secondary_gap, primary_bold,
                       title_end_col);
}

// EMA-smoothed FPS based on the interval between status-bar renders.
// Status-bar render happens once per top-level render call, which is
// what we actually care about — terminal frame rate after all the
// game/grid/composite work. alpha=0.1 gives a stable readout that
// still responds within a second or so when the rate genuinely shifts.
// Compact count formatter shared by the analysis-panel titles and the
// status bar's NPS readout. Same threshold ladder as a chess engine
// info line: bare integer up to 9999, then K / M / B / T with two
// decimal places past the first thousand-tier boundary so the digits
// roll smoothly rather than in big jumps.
static void format_count_compact(uint64_t n, char *buf, size_t bufsz) {
  if (n >= 1000000000000ULL) {
    snprintf(buf, bufsz, "%.2fT", (double)n / 1e12);
  } else if (n >= 1000000000ULL) {
    snprintf(buf, bufsz, "%.2fB", (double)n / 1e9);
  } else if (n >= 1000000ULL) {
    snprintf(buf, bufsz, "%.2fM", (double)n / 1e6);
  } else if (n >= 10000ULL) {
    snprintf(buf, bufsz, "%lluK", (unsigned long long)(n / 1000ULL));
  } else {
    snprintf(buf, bufsz, "%llu", (unsigned long long)n);
  }
}

// EMA-smoothed nodes-per-second based on the per-frame delta of a
// monotonically-increasing node counter. Lighter damping (α=0.3) than
// measure_fps so the readout responds quickly when a search ramps up
// or winds down. Resets gracefully when the counter resets (new
// search) or when there's no active counter (returns 0).
static double measure_nps(uint64_t nodes_now) {
  static double ema = 0.0;
  static struct timespec last;
  static uint64_t last_nodes = 0;
  static bool inited = false;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!inited) {
    inited = true;
    last = now;
    last_nodes = nodes_now;
    return 0.0;
  }
  const double dt = (double)(now.tv_sec - last.tv_sec) +
                    (double)(now.tv_nsec - last.tv_nsec) / 1e9;
  // Reset path: counter went backward (new search) or no progress.
  // Realign the baseline and decay the EMA toward 0 so the readout
  // fades when the bot stops.
  if (nodes_now < last_nodes || nodes_now == 0) {
    last = now;
    last_nodes = nodes_now;
    ema = ema * 0.7;
    return ema;
  }
  if (dt <= 0.0 || dt > 5.0) {
    last = now;
    last_nodes = nodes_now;
    return ema;
  }
  const uint64_t delta = nodes_now - last_nodes;
  last = now;
  last_nodes = nodes_now;
  const double instant = (double)delta / dt;
  if (ema <= 0.0) {
    ema = instant;
  } else {
    ema = ema * 0.7 + instant * 0.3;
  }
  return ema;
}

static double measure_fps(void) {
  static double ema = 0.0;
  static struct timespec last;
  static bool inited = false;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!inited) {
    last = now;
    inited = true;
    return 0.0;
  }
  const double dt = (double)(now.tv_sec - last.tv_sec) +
                    (double)(now.tv_nsec - last.tv_nsec) / 1e9;
  last = now;
  // Skip pathological intervals: clock jumps / long pauses (dt > 5s)
  // and the "unslept frame after a long frame" case where the main
  // loop's overrun-recovery branch runs the next render immediately
  // without sleeping, so the gap between measure_fps calls collapses
  // and 1/dt blows up to 200+ fps. The render loop targets 60 fps,
  // so any reading above 120 is a measurement artifact, not a real
  // frame rate — drop it rather than let it poison the EMA.
  if (dt <= 0.0 || dt > 5.0 || dt < 1.0 / 120.0) {
    return ema;
  }
  const double instant = 1.0 / dt;
  if (ema <= 0.0) {
    ema = instant;
  } else {
    ema = ema * 0.9 + instant * 0.1;
  }
  return ema;
}

// ── Status bar ────────────────────────────────────────────────────────────
// Pending-change banner: when the user has changed Lexicon or RIT
// via Settings but the live game is still using the values from
// game-state init, render a one-line summary just above the status
// bar so they see exactly what needs a New Game to apply. Subtle
// color (theme->dim_fg on theme->bg) since it's informational, not
// alarming.
static void render_pending_bar(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L) {
  if (L->pending_row < 0 || state == NULL) {
    return;
  }
  const int row = L->pending_row;
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
  char buf[160];
  int written = 0;
  written += snprintf(
      buf + written,
      sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
      " Next game:");
  bool any = false;
  if (strcmp(state->pending_lexicon, state->active_lexicon) != 0) {
    written += snprintf(
        buf + written,
        sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
        " lexicon %s \xe2\x86\x92 %s", state->active_lexicon,
        state->pending_lexicon);
    any = true;
  }
  if (state->pending_load_rit != state->active_load_rit) {
    written += snprintf(
        buf + written,
        sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
        "%s RIT %s \xe2\x86\x92 %s", any ? "," : "",
        state->active_load_rit ? "on" : "off",
        state->pending_load_rit ? "on" : "off");
    any = true;
  }
  snprintf(buf + written,
           sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
           " \xc2\xb7 restart to apply");
  ncplane_putstr_yx(plane, row, 0, buf);
}

// Command bar: always-on row directly above the status bar. Hosts
// the [0] focus indicator on the left and a placeholder right-side
// hint ("/ for cmd") prompting the user to enter command-input mode.
// When [0] is the focused index, the row paints on
// theme->panel_focus_border_bg with bold [0] — matching the panel
// border treatment so focus reads consistently across the chrome.
// The actual /-input + autocomplete is wired in a follow-up.
// Command palette popup: rendered above the command bar while
// slash mode is active. Lists commands whose name starts with the
// typed prefix and a short description, mirroring the Claude Code
// CLI's `/`-prompt style — typed prefix in theme->fg, remaining
// command-name letters and descriptions in a dim grey. When the
// prefix matches nothing, shows a single "No commands match"
// message instead of an empty popup. Drawn directly on the std
// plane and sized to the visible matches; the next frame's content
// rendering naturally repaints over it when the popup goes away.
static void render_command_palette(struct ncplane *plane, const Theme *theme,
                                   const TuiGameState *state, const Layout *L) {
  if (state == NULL || !state->slash_active) {
    return;
  }
  struct Cmd {
    const char *name;
    const char *desc;
  };
  static const struct Cmd cmds[] = {
      {"exit", "Quit MAGPIE TUI (alias for /quit)"},
      {"new", "Start a new game"},
      {"quit", "Quit MAGPIE TUI"},
      {"settings", "Open settings"},
  };
  static const int n_cmds = (int)(sizeof(cmds) / sizeof(cmds[0]));

  // Filter to prefix matches against the lowercase slash buffer.
  int match_idx[16];
  int n_match = 0;
  for (int i = 0;
       i < n_cmds && n_match < (int)(sizeof(match_idx) / sizeof(match_idx[0]));
       i++) {
    if (state->slash_len == 0 ||
        ((int)strlen(cmds[i].name) >= state->slash_len &&
         strncmp(cmds[i].name, state->slash_buf, (size_t)state->slash_len) ==
             0)) {
      match_idx[n_match++] = i;
    }
  }

  const int popup_rows = n_match > 0 ? n_match : 1;
  const int popup_top = L->command_bar_row - popup_rows;
  if (popup_top < 0) {
    return;
  }

  if (n_match == 0) {
    // Single-line "No commands match" message.
    char buf[128];
    snprintf(buf, sizeof(buf), " No commands match \"/%s\"", state->slash_buf);
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, 0);
    // Clear the row then write.
    for (unsigned c = 0; c < L->plane_cols; c++) {
      ncplane_putstr_yx(plane, popup_top, (int)c, " ");
    }
    ncplane_putstr_yx(plane, popup_top, 1, buf);
    return;
  }

  // Compute description column: aligns the descriptions across all
  // matching rows. Leading "/" plus the command name, plus a fixed
  // gap of 3 cells.
  int max_name = 0;
  for (int i = 0; i < n_match; i++) {
    const int w = (int)strlen(cmds[match_idx[i]].name);
    if (w > max_name) {
      max_name = w;
    }
  }
  const int name_col = 1; // 1-cell left pad
  const int desc_col = name_col + 1 /* "/" */ + max_name + 3;

  for (int i = 0; i < n_match; i++) {
    const int row = popup_top + i;
    const struct Cmd *c = &cmds[match_idx[i]];
    // Clear row to theme->bg first so we don't inherit colored
    // content from the panel that was drawn below.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, 0);
    for (unsigned col = 0; col < L->plane_cols; col++) {
      ncplane_putstr_yx(plane, row, (int)col, " ");
    }
    // Leading "/" in dim (it's the same for every row, not part of
    // the matched-prefix highlight).
    int col = name_col;
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_putstr_yx(plane, row, col++, "/");
    // Matched prefix portion in bright theme->fg.
    theme_apply_fg(plane, theme->fg);
    for (int k = 0; k < state->slash_len && c->name[k] != '\0'; k++) {
      char ch[2] = {c->name[k], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Remainder of the command name in dim grey.
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    for (int k = state->slash_len; c->name[k] != '\0'; k++) {
      char ch[2] = {c->name[k], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Description column, dim grey.
    if (desc_col < (int)L->plane_cols) {
      theme_apply_fg(plane, theme->modal_shortcut_fg);
      ncplane_putstr_yx(plane, row, desc_col, c->desc);
    }
  }
}

static void render_command_bar(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L,
                               TuiModalState modal) {
  if (L->command_bar_row < 0) {
    return;
  }
  const int row = L->command_bar_row;
  const bool focused = state != NULL && state->focused_panel == 0;
  const ThemeRgb bar_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, bar_bg);
  ncplane_set_styles(plane, 0);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
  // Left side: "[0] Command>" prompt. The ">" hangs off "Command"
  // with no space, so it reads as one prompt token like a shell.
  // Focused: grey-on-grey "[0>" chip matching every other panel's
  // focus badge. Unfocused: dim "[0]" hint.
  int col = 1;
  if (focused) {
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->fg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, col, "[0>");
  } else {
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_putstr_yx(plane, row, col, "[0]");
  }
  col += 3;
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, bar_bg);
  ncplane_putstr_yx(plane, row, col++, " ");
  ncplane_putstr_yx(plane, row, col, "Command>");
  col += 8;
  ncplane_putstr_yx(plane, row, col++, " ");

  // After the prompt we render either:
  //  - the slash-mode input (typed bold, autocomplete suffix dim),
  //  - the placeholder hint "/ to type commands" (dim italic),
  //  - or nothing if neither applies (slash mode not entered, [0]
  //    not focused).
  if (state != NULL && state->slash_active) {
    // "/" prompt, non-bold.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_set_styles(plane, 0);
    ncplane_putstr_yx(plane, row, col++, "/");
    const int typed_start = col;
    for (int i = 0; i < state->slash_len; i++) {
      char ch[2] = {state->slash_buf[i], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Live terminal cursor at slash_cursor's column. Matching
    // commands and their descriptions appear in a popup above the
    // bar (render_command_palette), so there's no inline ghost text
    // here anymore.
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_enable(nc, row, typed_start + state->slash_cursor);
    }
  } else if (modal == TUI_MODAL_NONE) {
    // Not in slash mode and no modal open — show the "/" hint so
    // the user knows the command bar is reachable. When a modal
    // is up, the "/" key won't refocus [0] anyway, so suppress
    // the hint instead of advertising an action that won't work.
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_set_styles(plane, NCSTYLE_ITALIC);
    ncplane_putstr_yx(plane, row, col, "/ to type commands");
    ncplane_set_styles(plane, 0);
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_disable(nc);
    }
  } else {
    // Modal up: nothing in the input slot. Make sure the live
    // terminal cursor (which slash mode may have enabled) is
    // disabled so it doesn't blink in the command bar while the
    // user is interacting with the modal.
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_disable(nc);
    }
  }

  // Right side: when [0] is focused (and we're not actively in
  // slash mode), surface the alphabetical commands as inline help.
  // Each entry is "<KEY> <name>", separated by " · ". The key
  // letter renders in modal_shortcut_fg, the name in theme->fg.
  if (focused && !(state != NULL && state->slash_active)) {
    struct {
      const char *key;
      const char *name;
    } cmds[] = {{"N", "new"}, {"S", "settings"}, {"Q", "quit"}};
    const int n = (int)(sizeof(cmds) / sizeof(cmds[0]));
    // Pre-compute width to right-align.
    int total = 0;
    for (int i = 0; i < n; i++) {
      if (i > 0) {
        total += 3; // " · "
      }
      total += (int)strlen(cmds[i].key) + 1 + (int)strlen(cmds[i].name);
    }
    int rcol = (int)L->plane_cols - total - 1;
    if (rcol > col + 2) {
      for (int i = 0; i < n; i++) {
        if (i > 0) {
          theme_apply_fg(plane, theme->dim_fg);
          theme_apply_bg(plane, bar_bg);
          ncplane_putstr_yx(plane, row, rcol, " \xc2\xb7 "); // " · "
          rcol += 3;
        }
        theme_apply_fg(plane, theme->modal_shortcut_fg);
        theme_apply_bg(plane, bar_bg);
        ncplane_putstr_yx(plane, row, rcol, cmds[i].key);
        rcol += (int)strlen(cmds[i].key);
        theme_apply_fg(plane, theme->fg);
        ncplane_putstr_yx(plane, row, rcol++, " ");
        ncplane_putstr_yx(plane, row, rcol, cmds[i].name);
        rcol += (int)strlen(cmds[i].name);
      }
    }
  }
}

static void render_status_bar(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L,
                              TuiModalState modal) {
  const int row = L->status_row;
  if (row < 0) {
    return;
  }

  // Inverted-grey band: dark text on dim_fg-colored bar, matching the
  // analysis-panel column header strip so chrome reads consistently.
  theme_apply_fg(plane, theme->bg);
  theme_apply_bg(plane, theme->dim_fg);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }

  // Left side: "Language · Lexicon · 60 fps · 234MB mem". FPS and the
  // process resident-set size are tacked onto the left so they sit
  // next to the other engine-state readouts; the right side is
  // reserved for transient control hints. Show fps as an integer so
  // the last digit doesn't twitch every frame.
  const double fps = measure_fps();
  char mem_str[24];
  mem_str[0] = '\0';
  uint64_t resident_bytes = 0;
#ifdef __APPLE__
  {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &count) == KERN_SUCCESS) {
      resident_bytes = (uint64_t)info.resident_size;
    }
  }
#endif
  if (resident_bytes > 0) {
    // Pick the largest unit that keeps the number compact and the
    // precision sane: GB always shows 1 decimal; MB shows 2/1/0
    // decimals depending on magnitude so we land at "9.23MB",
    // "12.3MB", "234MB", "2.4GB".
    double val;
    const char *unit;
    int decimals;
    if (resident_bytes >= (1ULL << 30)) {
      val = (double)resident_bytes / (1024.0 * 1024.0 * 1024.0);
      unit = "GB";
      decimals = 1;
    } else {
      val = (double)resident_bytes / (1024.0 * 1024.0);
      unit = "MB";
      if (val >= 100.0) {
        decimals = 0;
      } else if (val >= 10.0) {
        decimals = 1;
      } else {
        decimals = 2;
      }
    }
    snprintf(mem_str, sizeof(mem_str), " \xc2\xb7 %.*f%s mem", decimals, val,
             unit);
  }
  char dim_str[48];
  unsigned cdy_now = 0, cdx_now = 0;
  ncplane_pixel_geom(plane, NULL, NULL, &cdy_now, &cdx_now, NULL, NULL);
  if (cdy_now > 0 && cdx_now > 0) {
    snprintf(dim_str, sizeof(dim_str), " \xc2\xb7 %ux%u (%ux%u)", L->plane_cols,
             L->plane_rows, cdx_now, cdy_now);
  } else {
    snprintf(dim_str, sizeof(dim_str), " \xc2\xb7 %ux%u", L->plane_cols,
             L->plane_rows);
  }
  // FPS is normally hidden — only surfaces when we're off the 60Hz
  // target (under 55 or over 70). When shown it gets bold + error_fg
  // so it reads as a "something's wrong" callout. The chunk is
  // rendered separately from left_buf so the styling is local to the
  // " · NN fps" segment and doesn't leak into the rest of the bar.
  const int fps_int = (int)(fps + 0.5);
  const bool show_fps = fps > 0.0 && (fps_int < 55 || fps_int > 70);
  char left_buf[192];
  snprintf(left_buf, sizeof(left_buf), " %s \xc2\xb7 %s",
           language_for_lexicon(state->lexicon), state->lexicon);
  char right_buf[64];
  snprintf(right_buf, sizeof(right_buf), "%s%s", mem_str, dim_str);
  // NPS: only shown while the bot is computing. Sim mode reports
  // iters*(plies+1); endgame reports the per-worker atomic sum.
  // measure_nps EMA-smooths the per-frame delta so the readout is
  // visually stable, and decays toward 0 when the counter idles.
  uint64_t bot_nodes = 0;
  if (atomic_load(&((TuiGameState *)state)->endgame_results_active)) {
    if (state->endgame_ctx != NULL) {
      bot_nodes = endgame_ctx_get_nodes_searched(state->endgame_ctx);
    }
  } else if (atomic_load(&((TuiGameState *)state)->sim_results_active) &&
             state->sim_results != NULL) {
    const int plies = sim_results_get_num_plies(state->sim_results);
    const uint64_t iters = sim_results_get_iteration_count(state->sim_results);
    bot_nodes = iters * (uint64_t)(plies + 1);
  }
  const double nps = measure_nps(bot_nodes);
  const bool show_nps = nps >= 1.0;

  ncplane_putstr_yx(plane, row, 0, left_buf);
  if (show_fps) {
    char fps_buf[24];
    snprintf(fps_buf, sizeof(fps_buf), " \xc2\xb7 %d fps", fps_int);
    theme_apply_fg(plane, theme->error_fg);
    theme_apply_bg(plane, theme->dim_fg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr(plane, fps_buf);
    ncplane_set_styles(plane, 0);
    // Restore the inverted-grey band colors for the rest of the bar.
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->dim_fg);
  }
  if (show_nps) {
    char nps_str[16];
    format_count_compact((uint64_t)(nps + 0.5), nps_str, sizeof(nps_str));
    char nps_buf[32];
    snprintf(nps_buf, sizeof(nps_buf), " \xc2\xb7 %s nps", nps_str);
    ncplane_putstr(plane, nps_buf);
  }
  ncplane_putstr(plane, right_buf);

  // Right side: dynamic shortcut hint depending on what modal is open.
  // Key names use leading caps for consistency with the rest of the
  // chrome (e.g. status bar's "Esc menu", command bar's "Q quit").
  const char *hint = " Esc menu ";
  switch (modal) {
  case TUI_MODAL_MAIN_MENU:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_SETTINGS:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 \xe2\x86\x90\xe2"
           "\x86\x92 adjust \xc2\xb7 Esc back ";
    break;
  case TUI_MODAL_TIME_PICKER:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_LEXICON_PICKER:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_QUIT_CONFIRM:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_NONE:
  default:
    break;
  }
  const int hint_len = (int)strlen(hint);
  // Strlen counts bytes; UTF-8 multibyte chars need to be counted as
  // single columns. Approximate by subtracting an estimated byte-overhead.
  // Each ↑/↓/←/→ is 3 bytes (E2 86 9X) but 1 col; · is 2 bytes (C2 B7)
  // but 1 col. Compute visual width by walking.
  int hint_cols = 0;
  for (const unsigned char *p = (const unsigned char *)hint; *p != '\0'; p++) {
    if ((*p & 0xC0) != 0x80) { // not a UTF-8 continuation byte
      hint_cols++;
    }
  }
  (void)hint_len;
  const int right_col = (int)L->plane_cols - hint_cols;
  if (right_col > 0) {
    ncplane_putstr_yx(plane, row, right_col, hint);
  }
}

// ── Top-level render ──────────────────────────────────────────────────────
static void render_too_small(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 0, 0, "Terminal too small. Resize.");
}

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal) {
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }

  // Invalidate modal hit-test data at the start of each frame.
  // render_modal_ex will set it valid again if a modal renders.
  modal_hit_map.valid = false;

  // Force a defensive full repaint whenever the plane's dimensions have
  // changed since the previous render — not only when *this* call did the
  // resize. main.c's NCKEY_RESIZE handler calls ncplane_resize_simple
  // before we get here, so tui_sync_plane_to_terminal sees the size
  // already matches and reports no resize; without this extra check the
  // diff cache leaves stale content (especially in the right history
  // column on terminals that uncover new cells, like Ghostty after a
  // font-size change).
  unsigned dim_y = 0;
  unsigned dim_x = 0;
  ncplane_dim_yx(plane, &dim_y, &dim_x);
  static unsigned prev_dim_y = 0;
  static unsigned prev_dim_x = 0;
  const bool plane_resized_externally =
      prev_dim_y != dim_y || prev_dim_x != dim_x;
  prev_dim_y = dim_y;
  prev_dim_x = dim_x;
  const bool resized =
      tui_sync_plane_to_terminal(plane) || plane_resized_externally;
  if (resized) {
    // Pixel-graphics planes cache an image at a specific cell offset; a
    // font-size change keeps the cell count but moves the underlying
    // pixel boundaries, so the previous image sits at the wrong place.
    // Destroying the cached planes forces them rebuilt from scratch in
    // this frame, which clears the stale image from the terminal.
    invalidate_grid_planes();
    // The glyph cache is keyed by target pixel height, which derives
    // from cdy — a font-size change makes every cached bitmap the wrong
    // size. Flush so render_board_pixel rerasterizes at the new ratio.
    tui_glyph_cache_reset(state->glyph_cache);
    // After a resize, notcurses' diff cache and the terminal's actual
    // screen state can disagree, leaving stale content (missing borders,
    // labels, premium markers, history entries) on the visible terminal.
    // Force every cell dirty by painting it with a sentinel color and
    // rendering, so the next normal render writes out *every* cell that
    // differs from the sentinel — i.e., everything.
    ncplane_dim_yx(plane, &dim_y, &dim_x);
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->bg);
    for (unsigned r = 0; r < dim_y; r++) {
      for (unsigned c = 0; c < dim_x; c++) {
        ncplane_putstr_yx(plane, (int)r, (int)c, " ");
      }
    }
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_render(nc);
    }
  }

  // Cell pixel-dim drift detection. Font tweaks that keep the
  // same row/col count can still shift the underlying cdy/cdx
  // (e.g., aspect-ratio change with the same nominal cell size).
  // Our per-tile sprixels were sized for the old cdy/cdx; the
  // terminal would scale them with nearest-neighbor to fit the
  // new cell area, producing blocky output. Destroying the planes
  // forces them rebuilt at the new dims this frame. Glyph caches
  // are also reset so the next set_size call re-rasterizes at the
  // new pixel height instead of reusing bitmaps for the old aspect.
  {
    unsigned probe_cdy = 0, probe_cdx = 0;
    ncplane_pixel_geom(plane, NULL, NULL, &probe_cdy, &probe_cdx, NULL, NULL);
    static unsigned prev_cdy = 0, prev_cdx = 0;
    if (probe_cdy > 0 && probe_cdx > 0 &&
        (probe_cdy != prev_cdy || probe_cdx != prev_cdx)) {
      if (prev_cdy != 0 || prev_cdx != 0) {
        invalidate_tile_planes();
        // Reset only the UI-thread-owned glyph caches here. The
        // pixel_glyph_cache pair is owned by the worker thread and
        // self-heals via set_size on its next compose — touching
        // it from the UI thread would race with worker reads.
        if (state->glyph_cache != NULL) {
          tui_glyph_cache_reset(state->glyph_cache);
        }
        if (state->glyph_cache_sub != NULL) {
          tui_glyph_cache_reset(state->glyph_cache_sub);
        }
      }
      prev_cdy = probe_cdy;
      prev_cdx = probe_cdx;
    }
  }

  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  // 2x mode is only honored when the host terminal supports pixel
  // graphics AND the bundled TTF actually loaded; otherwise we cap the
  // user's preference at fullwidth (scale=1). compute_effective_scale
  // then degrades further to halfwidth (scale=0) when the plane is too
  // narrow for fullwidth.
  struct notcurses *render_nc = ncplane_notcurses(plane);
  const bool pixel_ok = render_nc != NULL && notcurses_canpixel(render_nc);
  const int user_pref =
      (state->board_scale >= 2 && pixel_ok && state->glyph_cache != NULL) ? 2
                                                                          : 1;
  const Layout L = compute_layout(plane, user_pref, state);
  if (L.scale < 0) {
    render_too_small(plane, theme);
    return;
  }

  // Prepare the analysis rows once per frame, BEFORE rendering
  // the board — the on-board candidate preview resolves the
  // Analysis cursor against this prepared row list (including
  // the MOVE-column anchor lookup), so it must be in sync with
  // what render_analysis_panel is about to paint.
  populate_frame_analysis_rows(state);

  render_board(plane, theme, state, &L);
  // Grid lines are baked into each per-tile pixel buffer now (see
  // compose_tile_pixels); no separate overlay plane needed.
  // Tried layering render_board_grid_overlay on top to give
  // premium squares the same right/bottom inset, but the overlay
  // plane's transparent regions don't pass through to the
  // per-tile sprixels underneath (terminal sprixel stacking
  // replaces rather than composes), so it occludes placed tiles
  // entirely. Premium-square gridding would need a different
  // approach — e.g., per-premium pixel planes that bake in the
  // border the same way tiles do.
  (void)render_board_grid_overlay;

  // Annotation editor: when the user's typed play validates, draw
  // a Unicode directional cursor on the next square past the
  // last tile so they can see at a glance where the next typed
  // letter will land. Horizontal moves get "→", vertical "↓".
  // If the cursor would fall outside the 15×15 board (i.e., the
  // play ends at the right or bottom edge), it spills one cell
  // past column O or row 15 onto the panel border / row gutter,
  // which is the natural "row 16 / column P" position.
  // Show the directional cursor whenever we have a TILE_PLACEMENT
  // preview Move — that includes coord-only entries (tiles_length
  // == 0), partial placements that don't validate yet, and fully
  // legal plays. The arrow's job is to communicate WHERE the next
  // typed letter will land; it doesn't need the play to be legal
  // for that to be useful.
  bool arrow_drawn = false;
  if (state->edit_history_idx >= 0 && state->edit_preview_move_valid &&
      state->edit_preview_move != NULL &&
      move_get_type(state->edit_preview_move) ==
          GAME_EVENT_TILE_PLACEMENT_MOVE) {
    const Move *pm = state->edit_preview_move;
    const int dir = move_get_dir(pm);
    const bool vertical = board_is_dir_vertical(dir);
    const int row0 = move_get_row_start(pm);
    const int col0 = move_get_col_start(pm);
    const int n = move_get_tiles_length(pm);
    int next_row = vertical ? row0 + n : row0;
    int next_col = vertical ? col0 : col0 + n;
    // Skip past any board tiles sitting in the cursor's path — the
    // next *typed* letter would play through them, so the cursor
    // belongs on the first EMPTY square beyond the play's span.
    // The engine board is seeked to this turn's pre-move position
    // during editing, so it holds every other turn's tiles (e.g.
    // a perpendicular word crossing the cursor cell) but not this
    // play's preview.
    {
      const Board *brd = game_get_board(state->game);
      while (brd != NULL && next_row >= 0 && next_row < BOARD_DIM &&
             next_col >= 0 && next_col < BOARD_DIM &&
             board_get_letter(brd, next_row, next_col) !=
                 ALPHABET_EMPTY_SQUARE_MARKER) {
        if (vertical) {
          next_row++;
        } else {
          next_col++;
        }
      }
    }
    if (next_row >= 0 && next_col >= 0 && next_row <= BOARD_DIM &&
        next_col <= BOARD_DIM) {
      const int screen_top = CELL_ROW_BASE + next_row * L.board_cell_h;
      const int screen_left = CELL_COL_BASE + next_col * L.board_cell_w;
      const int player_idx = state->history[state->edit_history_idx].player_idx;
      // 2x scale: pixel-blit a fat filled-triangle arrow so it
      // really fills the tile-sized cell at the same visual
      // weight as a played tile. Below 2x, fall through to the
      // text-mode "filled cell + glyph" approach.
      // The pixel-blitted arrow plane leaves sprixel residue on
      // the terminal — destroying our plane doesn't reclaim the
      // cells the terminal painted with the pixel image, even
      // through notcurses_refresh. Until that's resolved, render
      // the arrow as a text-mode tile-bg fill + glyph at every
      // scale. The visual is less tile-weight at 2x but doesn't
      // bleed colored cells across row 8 when the arrow moves.
      // Now that the cdy/cdx invalidator at the top of
      // tui_game_render destroys per-tile planes on geometry
      // change AND we destroy + recreate the arrow plane on
      // position change, sprixel residue is contained and pixel
      // arrows are safe to use at 2x. Scales 0/1 keep the
      // text-mode fallback so 1- and 2-cell cells stay readable.
      struct notcurses *arrow_nc = ncplane_notcurses(plane);
      const bool arrow_pixel_ok =
          L.scale == 2 && arrow_nc != NULL && notcurses_canpixel(arrow_nc);
      if (arrow_pixel_ok) {
        unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
        ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
        if (cdy > 0 && cdx > 0) {
          const int tile_w = (int)cdx * L.board_cell_w;
          const int tile_h = (int)cdy * L.board_cell_h;
          // Pixel-mode terminals leave the sprixel pixels lit at
          // a plane's old position when we ncplane_move_yx it. The
          // arrow walks one cell per keystroke during a typed
          // move, which left a trail of green ghost-cells at every
          // prior arrow position. Destroy + recreate the plane
          // whenever the position changes so the terminal is
          // forced to drop the old sprixel and only the current
          // cell stays lit.
          const bool same_position =
              edit_arrow_cache.valid && edit_arrow_plane != NULL &&
              edit_arrow_cache.screen_top == screen_top &&
              edit_arrow_cache.screen_left == screen_left &&
              edit_arrow_cache.cdy == cdy && edit_arrow_cache.cdx == cdx &&
              edit_arrow_cache.scale == L.scale;
          if (!same_position) {
            invalidate_edit_arrow_plane();
          }
          const bool cache_hit =
              edit_arrow_cache.valid && edit_arrow_plane != NULL &&
              edit_arrow_cache.vertical == vertical &&
              edit_arrow_cache.player_idx == player_idx &&
              edit_arrow_cache.cdy == cdy && edit_arrow_cache.cdx == cdx &&
              edit_arrow_cache.scale == L.scale;
          if (edit_arrow_plane == NULL) {
            ncplane_options opts = {0};
            opts.y = screen_top;
            opts.x = screen_left;
            opts.rows = (unsigned)L.board_cell_h;
            opts.cols = (unsigned)L.board_cell_w;
            opts.name = "edit_arrow";
            edit_arrow_plane = ncplane_create(plane, &opts);
          }
          if (edit_arrow_plane != NULL) {
            if (!cache_hit) {
              uint8_t *buf = compose_arrow_pixels(
                  vertical, player_idx, tile_w, tile_h, state->antialias,
                  state->border_thickness,
                  state->pixel_glyph_cache != NULL ? state->pixel_glyph_cache
                                                   : state->glyph_cache,
                  theme);
              if (buf != NULL) {
                struct ncvisual_options vopts = {0};
                vopts.n = edit_arrow_plane;
                vopts.blitter = NCBLIT_PIXEL;
                vopts.leny = (unsigned)tile_h;
                vopts.lenx = (unsigned)tile_w;
                ncblit_rgba(buf, tile_w * 4, &vopts);
                tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx,
                                       (int)vopts.leny);
                free(buf);
                edit_arrow_cache.vertical = vertical;
                edit_arrow_cache.player_idx = player_idx;
                edit_arrow_cache.screen_top = screen_top;
                edit_arrow_cache.screen_left = screen_left;
                edit_arrow_cache.cdy = cdy;
                edit_arrow_cache.cdx = cdx;
                edit_arrow_cache.scale = L.scale;
                edit_arrow_cache.valid = true;
              }
            }
            arrow_drawn = true;
          }
        }
      }
      if (!arrow_drawn) {
        // Text-mode fallback. Paint the player tile-bg across the
        // cell and stamp the same heavy block-arrow glyph used at
        // 2x: ➡ U+27A1 (rightwards) / ⬇ U+2B07 (downwards). At
        // 2-cell-wide scales the glyph's East Asian Wide width
        // naturally spans the cell; at 1-cell-wide scale 0 it
        // sits in a single cell.
        const ThemeRgb tile_bg =
            player_idx == 1 ? theme->tile2_bg : theme->tile1_bg;
        const ThemeRgb tile_fg =
            player_idx == 1 ? theme->tile2_fg : theme->tile1_fg;
        theme_apply_bg(plane, tile_bg);
        theme_apply_fg(plane, tile_fg);
        for (int dr = 0; dr < L.board_cell_h; dr++) {
          for (int dc = 0; dc < L.board_cell_w; dc++) {
            ncplane_putstr_yx(plane, screen_top + dr, screen_left + dc, " ");
          }
        }
        // Plain text-presentation arrows (U+2192 → / U+2193 ↓).
        // Text-range codepoints — the terminal won't up-render
        // them as color emoji images, which is what caused every
        // emoji-range glyph choice to leave sprixel residue when
        // the arrow moved.
        //   scale 0 (cell_w=1): single arrow fills the cell.
        //   scale 1 (cell_w=2): doubled arrows fill the cell.
        //   scale 2 (cell_w=4): single arrow centered in the cell.
        // The 2x case uses a single glyph rather than doubled —
        // the doubled pair only filled 2 of 4 columns and read
        // as awkwardly skewed; one centered arrow reads cleaner
        // even though it doesn't fill the whole cell.
        const bool double_glyph = L.board_cell_w == 2;
        const char *glyph_h_single = "\xe2\x86\x92";             // →
        const char *glyph_v_single = "\xe2\x86\x93";             // ↓
        const char *glyph_h_double = "\xe2\x86\x92\xe2\x86\x92"; // →→
        const char *glyph_v_double = "\xe2\x86\x93\xe2\x86\x93"; // ↓↓
        const char *glyph =
            vertical ? (double_glyph ? glyph_v_double : glyph_v_single)
                     : (double_glyph ? glyph_h_double : glyph_h_single);
        const int glyph_w = double_glyph ? 2 : 1;
        const int glyph_row = screen_top + (L.board_cell_h - 1) / 2;
        const int glyph_col = screen_left + (L.board_cell_w - glyph_w) / 2;
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, glyph_row, glyph_col, glyph);
        ncplane_set_styles(plane, 0);
        theme_apply_bg(plane, theme->bg);
        arrow_drawn = true;
      }
    }
  }
  // No arrow this frame — drop the cached plane so the underlying
  // board (premium/empty cell paint) shows through.
  if (!arrow_drawn && edit_arrow_plane != NULL) {
    invalidate_edit_arrow_plane();
  }

  render_rack_panel(plane, theme, state, &L);
  render_bag_panel(plane, theme, state, &L);

  (void)time_per_side_seconds; // now read from state->time_per_side_seconds
  if (L.combined_pills_history) {
    draw_combined_pills_history_frame(
        plane, theme, state, &L, state->focused_panel == TUI_FOCUS_HISTORY);
  }
  render_player_pill(plane, theme, state, 0, L.pill1_top, L.pill1_left,
                     L.pill1_right, L.pills_halfwidth,
                     !L.combined_pills_history);
  render_player_pill(plane, theme, state, 1, L.pill2_top, L.pill2_left,
                     L.pill2_right, L.pills_halfwidth,
                     !L.combined_pills_history);
  render_history_panel(plane, theme, state, &L);
  render_analysis_panel(plane, theme, state, &L);

  render_pending_bar(plane, theme, state, &L);
  render_command_bar(plane, theme, state, &L, modal);
  render_command_palette(plane, theme, state, &L);
  render_status_bar(plane, theme, state, &L, modal);

  // Debug perf overlay disabled. The pipeline-instrumentation
  // counters (g_board_blit_latency_us, g_ncblit_us, g_max_frame_us,
  // g_last_tile_blits, g_sprixel_emits_delta, g_sprixel_elides_delta)
  // are still updated by render_board_pixel / main.c so a one-line
  // re-enable here is enough if we need to look at them again.

  // When a modal closes, drop its plane so the next open recreates it
  // at the right size and z-position. Cheap (one destroy) and keeps
  // the modal-open path's plane setup simple.
  if (modal == TUI_MODAL_NONE && grid_planes.modal != NULL) {
    ncplane_destroy(grid_planes.modal);
    grid_planes.modal = NULL;
  }
}

// ── Modal helpers ─────────────────────────────────────────────────────────
//
// A modal is a centered box on top of the game frame. We render the items
// vertically with the focused row using accent_fg as a highlight stripe.

// shortcuts[i] is an optional right-aligned hint shown in a mid-grey
// next to items[i] (e.g. "N", "Esc"). Pass NULL to omit shortcuts
// entirely; individual entries may also be NULL/"" for items with no
// hint. The modal uses its own clinical-grey palette (theme->modal_*)
// rather than the game-content green/amber, so menus read as system
// chrome distinct from the game surface.
// An "input-field zone" decoration for a modal row: a fixed-width,
// right-anchored-within-the-row rectangle painted in a darker bg
// so the user can see exactly where typing lands. zone_starts[i]
// is the modal-local items[]-string offset of the zone's left
// edge; zone_widths[i] is the zone width in cells. Pass NULL for
// either to disable zones for the whole modal.
static void render_modal_ex(struct ncplane *plane, const Theme *theme,
                            const char *title, const char *const *items,
                            const char *const *shortcuts, const bool *disabled,
                            const int *cursor_cols, const int *zone_starts,
                            const int *zone_widths, int item_count, int focus,
                            int width) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  // Items now sit flush against the top/bottom borders — no blank
  // padding row under the title — so height is exactly 2 + items.
  const int height = 2 + item_count;
  // Plane is height+1 × width+1 so we can paint a 1-cell drop shadow
  // along the bottom row and right column. The shadow uses half-block
  // glyphs (▀ ▌) with transparent bg, so the row under and column
  // right of the modal show through except for the thin shadow strip
  // hugging the modal's edge.
  const int plane_h = height + 1;
  const int plane_w = width + 1;
  if ((unsigned)plane_w >= plane_cols || (unsigned)plane_h >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - plane_h) / 2;
  const int left = (int)(plane_cols - plane_w) / 2;

  // Publish hit-test data for mouse-click handling. Items live at
  // rows [top+1 .. top+item_count] within columns [left+1 .. left+width-2]
  // (the 1-cell border on each side is non-clickable chrome). The
  // shadow row/col are not part of the clickable surface.
  modal_hit_map.valid = true;
  modal_hit_map.outer_top = top;
  modal_hit_map.outer_bottom = top + height - 1;
  modal_hit_map.outer_left = left;
  modal_hit_map.outer_right = left + width - 1;
  modal_hit_map.top = top + 1; // first item row
  modal_hit_map.left = left + 1;
  modal_hit_map.right = left + width - 2;
  modal_hit_map.item_count =
      item_count < MODAL_MAX_ITEMS ? item_count : MODAL_MAX_ITEMS;
  for (int i = 0; i < modal_hit_map.item_count; i++) {
    modal_hit_map.disabled[i] = disabled != NULL && disabled[i];
    modal_hit_map.left_chev_col[i] = -1;
    modal_hit_map.right_chev_col[i] = -1;
    // Scan the item text for ◀ (E2 97 80) and ▶ (E2 96 B6).
    // Each chevron occupies 1 display column. Item text renders
    // starting at modal-interior col 3, so the screen column is
    // (left + 3 + display_offset).
    if (items != NULL && items[i] != NULL) {
      const unsigned char *s = (const unsigned char *)items[i];
      int disp = 0;
      while (*s != '\0') {
        if (s[0] == 0xe2 && s[1] == 0x97 && s[2] == 0x80) {
          modal_hit_map.left_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] == 0xe2 && s[1] == 0x96 && s[2] == 0xb6) {
          modal_hit_map.right_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] >= 0x80) {
          // Other multi-byte UTF-8 glyph; advance bytes by the
          // UTF-8 length and column by 1 (assumes BMP narrow,
          // which holds for the strings the modals build).
          int len = 1;
          if ((s[0] & 0xe0) == 0xc0) {
            len = 2;
          } else if ((s[0] & 0xf0) == 0xe0) {
            len = 3;
          } else if ((s[0] & 0xf8) == 0xf0) {
            len = 4;
          }
          s += len;
          disp++;
        } else {
          s++;
          disp++;
        }
      }
    }
  }

  // Modal lives on its own child plane that always sits on top of the
  // z-stack. Otherwise the 2x pixel composite (also a child of std)
  // sits above the modal, occluding it. Box-local coords run (0,0) to
  // (plane_h-1, plane_w-1); the modal proper occupies (0..height-1,
  // 0..width-1) and the shadow occupies (height, 1..width) plus
  // (1..height, width).
  if (grid_planes.modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)plane_h;
    opts.cols = (unsigned)plane_w;
    opts.name = "modal";
    grid_planes.modal = ncplane_create(plane, &opts);
    if (grid_planes.modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(grid_planes.modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != plane_h || (int)cur_cols != plane_w) {
      ncplane_resize_simple(grid_planes.modal, (unsigned)plane_h,
                            (unsigned)plane_w);
    }
    ncplane_move_yx(grid_planes.modal, top, left);
  }
  struct ncplane *mp = grid_planes.modal;
  // Plane base is transparent: cells outside the modal proper and
  // outside the shadow strips let the underlying game frame show
  // through. The modal area fills explicitly below.
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  // Reset the plane's current channels to fully-opaque defaults. The
  // shadow-pass at the end of this function leaves bg alpha set to
  // TRANSPARENT, and notcurses' ncplane_set_{fg,bg}_rgb8 only touches
  // the RGB bits — without this reset, every frame after the first
  // would inherit the transparent bg from the previous shadow pass
  // and paint the modal interior as see-through.
  ncplane_set_channels(mp, 0);

  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }

  // Frame chrome: top/bottom rows + left/right columns paint with
  // modal_border_bg (a hair lighter than the interior modal_bg) so
  // the edge reads as a defined trim — the macOS-style hairline.
  // Box-drawing glyphs sit on this trim in modal_border_fg.
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  if (title != NULL && title[0] != '\0') {
    // Title sits on the top frame strip, so its bg is modal_border_bg
    // to match the surrounding chrome (otherwise the " Title " label
    // appears in a darker pocket cut out of the lighter strip).
    theme_apply_fg(mp, theme->modal_fg);
    theme_apply_bg(mp, theme->modal_border_bg);
    ncplane_putstr_yx(mp, 0, 2, " ");
    ncplane_putstr(mp, title);
    ncplane_putstr(mp, " ");
  }

  // Item rows. Layout per row:
  //   [ space ][ space ][ label ............... ][ shortcut ][ space ]
  // Focused row gets a full-width selection bar (modal_focus_bg) so
  // the highlight reads as a coherent strip, not just a colored label.
  for (int i = 0; i < item_count; i++) {
    const int item_row = 1 + i;
    const bool focused = (i == focus);
    const bool item_disabled = disabled != NULL && disabled[i];
    // Disabled items never use the focus highlight — they paint
    // dim text on the unfocused row background so they read as
    // "informational only, not selectable".
    const ThemeRgb row_fg = item_disabled ? theme->modal_shortcut_fg
                            : focused     ? theme->modal_focus_fg
                                          : theme->modal_fg;
    const ThemeRgb row_bg =
        (focused && !item_disabled) ? theme->modal_focus_bg : theme->modal_bg;
    const ThemeRgb shortcut_fg = focused && !item_disabled
                                     ? theme->modal_focus_fg
                                     : theme->modal_shortcut_fg;

    // Fill the row background (between the side borders).
    theme_apply_fg(mp, row_fg);
    theme_apply_bg(mp, row_bg);
    for (int c = 1; c <= right_col - 1; c++) {
      ncplane_putstr_yx(mp, item_row, c, " ");
    }

    // Per-row input-field zone (e.g. annotate-setup name field).
    // Paint a darker bg over the zone before the items text so
    // the editable region reads as a recessed input rectangle.
    const int z_start = (zone_starts != NULL) ? zone_starts[i] : -1;
    const int z_width = (zone_widths != NULL) ? zone_widths[i] : 0;
    const bool has_zone = z_start >= 0 && z_width > 0;
    if (has_zone) {
      theme_apply_fg(mp, row_fg);
      theme_apply_bg(mp, theme->bg);
      for (int z = 0; z < z_width; z++) {
        ncplane_putstr_yx(mp, item_row, 3 + z_start + z, " ");
      }
    }

    // Label / value text. When a zone is set we split the paint
    // so the zone keeps its darker bg: label region uses row_bg,
    // zone region uses theme->bg.
    if (items[i] != NULL) {
      if (has_zone) {
        const int items_len = (int)strlen(items[i]);
        // Region before the zone.
        if (z_start > 0 && z_start <= items_len) {
          char before[96];
          int n = z_start;
          if (n > (int)sizeof(before) - 1) {
            n = sizeof(before) - 1;
          }
          memcpy(before, items[i], (size_t)n);
          before[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3, before);
        }
        // Region inside the zone.
        if (z_start < items_len) {
          char inside[96];
          int end = z_start + z_width;
          if (end > items_len) {
            end = items_len;
          }
          int n = end - z_start;
          if (n > (int)sizeof(inside) - 1) {
            n = sizeof(inside) - 1;
          }
          memcpy(inside, items[i] + z_start, (size_t)n);
          inside[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, theme->bg);
          ncplane_putstr_yx(mp, item_row, 3 + z_start, inside);
        }
        // Region after the zone, if any.
        const int after_off = z_start + z_width;
        if (after_off < items_len) {
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3 + after_off, items[i] + after_off);
        }
      } else {
        ncplane_putstr_yx(mp, item_row, 3, items[i]);
      }
    }

    // Right-aligned shortcut hint, 2-space right padding.
    if (shortcuts != NULL && shortcuts[i] != NULL && shortcuts[i][0] != '\0') {
      const int sc_len = (int)strlen(shortcuts[i]);
      const int sc_col = right_col - 2 - sc_len + 1;
      if (sc_col >= 3) {
        theme_apply_fg(mp, shortcut_fg);
        theme_apply_bg(mp, row_bg);
        ncplane_putstr_yx(mp, item_row, sc_col, shortcuts[i]);
      }
    }

    // Optional block cursor for this row. cursor_cols[i] is the
    // BYTE OFFSET into items[i] where the caret sits (-1 = no
    // cursor on this row). The cell repaints with inverted
    // colors — when the caret sits inside a zone we invert the
    // zone's darker bg, otherwise we invert the row bg. When the
    // caret is past the end of the string we draw a space, same
    // color treatment.
    if (cursor_cols != NULL && items[i] != NULL && cursor_cols[i] >= 0) {
      const int items_len = (int)strlen(items[i]);
      const int co = cursor_cols[i];
      const int screen_col = 3 + co;
      if (screen_col >= 1 && screen_col <= right_col - 1) {
        char ch[2] = {' ', '\0'};
        if (co < items_len) {
          ch[0] = items[i][co];
        }
        const bool over_zone =
            has_zone && co >= z_start && co < z_start + z_width;
        const ThemeRgb cursor_fg = over_zone ? theme->bg : row_bg;
        // Invert: cursor cell bg = row_fg, cursor cell fg = the
        // bg we'd otherwise have at this cell.
        theme_apply_fg(mp, cursor_fg);
        theme_apply_bg(mp, row_fg);
        ncplane_set_styles(mp, NCSTYLE_BOLD);
        ncplane_putstr_yx(mp, item_row, screen_col, ch);
        ncplane_set_styles(mp, 0);
      }
    }
  }

  // Drop shadow: offset 1 cell right / 1 row down. Bottom strip uses
  // ▀ (upper half block) so only the half-row immediately touching
  // the modal renders shadow color; the lower half stays transparent
  // and lets whatever's underneath show through. Right strip uses ▌
  // (left half block) symmetrically. The shadow skips the top-left
  // corner cells so it visibly comes from a top-left light source.
  // Glyphs paint with bg-alpha transparent so the uncovered half-cell
  // composes with the game plane behind.
  {
    uint64_t shadow_ch = 0;
    ncchannels_set_fg_rgb8(&shadow_ch, theme->modal_shadow_fg.r,
                           theme->modal_shadow_fg.g, theme->modal_shadow_fg.b);
    ncchannels_set_bg_alpha(&shadow_ch, NCALPHA_TRANSPARENT);
    ncplane_set_channels(mp, shadow_ch);
    const int shadow_row = height; // first row past modal's bottom
    const int shadow_col = width;  // first col past modal's right
    // Half-cell offset shadow: the right strip's ▌ paints the LEFT
    // half of col=width, so its right edge sits at the middle of
    // that cell. For a sharp bottom-right corner the bottom strip
    // has to end at that same middle. Symmetrically on the left,
    // the bottom strip starts at the middle of col=0 (a half-cell
    // offset from the modal's left edge, implying light from
    // upper-left). Quadrant glyphs handle the two end caps:
    //   col=0:       ▝ (upper-right quadrant)  — right-half + top-half
    //   1..width-1:  ▀ (upper half, full width)
    //   col=width:   ▘ (upper-left quadrant)   — left-half + top-half
    ncplane_putstr_yx(mp, shadow_row, 0, "\xe2\x96\x9d"); // ▝
    for (int c = 1; c < shadow_col; c++) {
      ncplane_putstr_yx(mp, shadow_row, c, "\xe2\x96\x80"); // ▀
    }
    ncplane_putstr_yx(mp, shadow_row, shadow_col, "\xe2\x96\x98"); // ▘
    for (int r = 1; r < shadow_row; r++) {
      ncplane_putstr_yx(mp, r, shadow_col, "\xe2\x96\x8c"); // ▌
    }
  }
}

// Backwards-compatible wrapper: existing callers (menu, settings,
// pickers) never gray out items, so they pass NULL for the
// disabled mask and reach the same paint code path.
static void render_modal(struct ncplane *plane, const Theme *theme,
                         const char *title, const char *const *items,
                         const char *const *shortcuts, int item_count,
                         int focus, int width) {
  render_modal_ex(plane, theme, title, items, shortcuts, /*disabled=*/NULL,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, item_count, focus, width);
}

// Forward declaration so tui_game_render_watch_setup can format
// adjustable rows using the same arrow-marker convention as the
// Settings modal. Defined a few hundred lines below.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused);

void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[TUI_MENU_ITEM_COUNT];
  const char *shortcuts[TUI_MENU_ITEM_COUNT];
  items[TUI_MENU_NEW_GAME] = "New game";
  shortcuts[TUI_MENU_NEW_GAME] = "N";
  items[TUI_MENU_SETTINGS] = "Settings";
  shortcuts[TUI_MENU_SETTINGS] = "S";
  items[TUI_MENU_BACK] = "Back";
  shortcuts[TUI_MENU_BACK] = "Esc";
  items[TUI_MENU_QUIT] = "Quit";
  shortcuts[TUI_MENU_QUIT] = "Q";
  render_modal(plane, theme, "Menu", items, shortcuts, TUI_MENU_ITEM_COUNT,
               focus, 28);
}

void tui_game_render_startup_menu(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Per-row buffers so we can append "(coming soon)" to unbuilt
  // modes without separate string literals for each variant.
  enum { ROW_BUF = 48 };
  static char buf[TUI_STARTUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_STARTUP_ITEM_COUNT];
  const char *shortcuts[TUI_STARTUP_ITEM_COUNT];
  bool disabled[TUI_STARTUP_ITEM_COUNT];
  const char *labels[TUI_STARTUP_ITEM_COUNT] = {
      "Watch computer play",  "Load a position",           "Load a game",
      "Annotate a live game", "Play against the computer",
  };
  const char *shortcut_chars[TUI_STARTUP_ITEM_COUNT] = {"W", "P", "G", "A",
                                                        "V"};
  // Only "Watch computer play" is wired up today; the others
  // render dimmed with a "(coming soon)" tag and cursor skips
  // past them. As each mode ships, flip the corresponding entry
  // to enabled and add an Enter handler in main.c.
  const bool item_disabled[TUI_STARTUP_ITEM_COUNT] = {
      false, false, false, false, true,
  };
  for (int i = 0; i < TUI_STARTUP_ITEM_COUNT; i++) {
    if (item_disabled[i]) {
      snprintf(buf[i], ROW_BUF, "%s (coming soon)", labels[i]);
    } else {
      snprintf(buf[i], ROW_BUF, "%s", labels[i]);
    }
    items[i] = buf[i];
    shortcuts[i] = item_disabled[i] ? NULL : shortcut_chars[i];
    disabled[i] = item_disabled[i];
  }
  render_modal_ex(plane, theme, "MAGPIE", items, shortcuts, disabled,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, TUI_STARTUP_ITEM_COUNT, focus, 44);
}

// Format a Watch-setup row as "Label" left-aligned + "value"
// right-aligned within `content_w` display columns. When `focused`
// is true, the value is wrapped in ◀ ▶ markers to signal that
// Left/Right arrows will adjust it. `content_w` is the cell width
// available between the modal's 2-space left padding and the
// right border padding — the caller picks a value that matches
// the modal's `width - 4`.
static void format_setup_row(char *out, size_t out_size, int content_w,
                             const char *label, const char *value,
                             bool focused) {
  // Display width of the value, including arrow decorations when
  // focused. Each arrow is one display column despite being 3
  // bytes of UTF-8 (◀ = U+25C0, ▶ = U+25B6).
  const int value_disp = (int)strlen(value);
  const int decorated_disp = focused ? value_disp + 4 : value_disp;
  const int label_disp = (int)strlen(label);
  int pad = content_w - label_disp - decorated_disp;
  if (pad < 1) {
    pad = 1;
  }
  if (focused) {
    snprintf(out, out_size, "%s%*s\xe2\x97\x80 %s \xe2\x96\xb6", label, pad, "",
             value);
  } else {
    snprintf(out, out_size, "%s%*s%s", label, pad, "", value);
  }
}

void tui_game_render_watch_setup(struct ncplane *plane, const Theme *theme,
                                 int focus, int time_seconds,
                                 const char *language, const char *lexicon,
                                 int sim_plies, int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Resolve the time-control display string from whichever preset
  // currently matches. Falls back to a "Ns" form so a custom value
  // (e.g., loaded from config) renders sensibly.
  const int preset_idx = tui_time_picker_closest_index(time_seconds);
  const char *time_label =
      tui_time_picker_preset_seconds(preset_idx) == time_seconds
          ? tui_time_picker_preset_label(preset_idx)
          : NULL;
  char time_value[24];
  if (time_label != NULL) {
    snprintf(time_value, sizeof(time_value), "%s", time_label);
  } else if (time_seconds <= 0) {
    snprintf(time_value, sizeof(time_value), "untimed");
  } else if (time_seconds % 60 == 0) {
    snprintf(time_value, sizeof(time_value), "%d min", time_seconds / 60);
  } else {
    snprintf(time_value, sizeof(time_value), "%ds", time_seconds);
  }

  // Modal width chosen to comfortably fit the widest row. "Sim
  // candidates" + 4 digits + ◀ ▶ markers needs ~30 cols of
  // content; 56 keeps the value column visually anchored to the
  // right edge for every row.
  enum { MODAL_WIDTH = 56, CONTENT_W = MODAL_WIDTH - 4, ROW_BUF = 96 };
  static char buf[TUI_WATCH_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_WATCH_SETUP_ITEM_COUNT];
  const bool focus_time = (focus == TUI_WATCH_SETUP_TIME);
  const bool focus_lang = (focus == TUI_WATCH_SETUP_LANGUAGE);
  const bool focus_lex = (focus == TUI_WATCH_SETUP_LEXICON);
  const bool focus_plies = (focus == TUI_WATCH_SETUP_SIM_PLIES);
  const bool focus_cands = (focus == TUI_WATCH_SETUP_SIM_CANDIDATES);
  format_setup_row(buf[TUI_WATCH_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus_time);
  format_setup_row(
      buf[TUI_WATCH_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
      language != NULL && language[0] != '\0' ? language : "(none)",
      focus_lang);
  format_setup_row(buf[TUI_WATCH_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus_lex);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus_plies);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str, focus_cands);
  snprintf(buf[TUI_WATCH_SETUP_START], ROW_BUF, "Start game");
  for (int i = 0; i < TUI_WATCH_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Watch setup", items, NULL,
               TUI_WATCH_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}

// Format a row with a right-anchored fixed-width input zone.
// Layout:
//   [label][   space-padding   ][   input zone   ]
// The input zone is `zone_width` cells wide and ends at column
// content_w-1. The value (possibly empty) sits left-justified
// inside the zone, padded with spaces so the entire zone is
// covered by characters — the renderer paints a darker bg on
// the zone, and characters here keep that bg.
static void format_setup_text_row(char *out, size_t out_size, int content_w,
                                  int zone_width, const char *label,
                                  const char *value) {
  if (out_size == 0) {
    return;
  }
  for (size_t i = 0; i < out_size - 1; i++) {
    out[i] = ' ';
  }
  out[out_size - 1] = '\0';
  const int label_disp = label != NULL ? (int)strlen(label) : 0;
  int li = 0;
  while (label != NULL && label[li] != '\0' && li < content_w &&
         (size_t)li < out_size - 1) {
    out[li] = label[li];
    li++;
  }
  const int zone_start = content_w - zone_width;
  (void)label_disp;
  if (value != NULL && zone_width > 0 && zone_start >= 0) {
    const int max_chars = zone_width - 1; // leave a trailing cell for the
                                          // end-of-text caret
    for (int i = 0; value[i] != '\0' && i < max_chars &&
                    (size_t)(zone_start + i) < out_size - 1;
         i++) {
      out[zone_start + i] = value[i];
    }
  }
  if ((size_t)content_w < out_size) {
    out[content_w] = '\0';
  }
}

void tui_game_render_annotate_setup(struct ncplane *plane, const Theme *theme,
                                    int focus, const char *lexicon,
                                    const char *p1_name, const char *p2_name,
                                    int name_edit_pos) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    // Right-anchored input zone for the player-name rows. Width
    // is the visual size of the rectangle the renderer paints
    // in a darker bg; the value sits left-justified inside, and
    // the trailing cell is reserved for the block cursor when
    // the caret is at end-of-text. 24 cells fits "PlayerNameHere "
    // — plenty for tournament-style nicknames.
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_ANNOTATE_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_starts[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_widths[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  const bool focus_lex = (focus == TUI_ANNOTATE_SETUP_LEXICON);
  const bool focus_p1 = (focus == TUI_ANNOTATE_SETUP_P1_NAME);
  const bool focus_p2 = (focus == TUI_ANNOTATE_SETUP_P2_NAME);

  format_setup_row(
      buf[TUI_ANNOTATE_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
      lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)", focus_lex);
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P1_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 1",
                        p1_name != NULL ? p1_name : "");
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P2_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 2",
                        p2_name != NULL ? p2_name : "");
  snprintf(buf[TUI_ANNOTATE_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_ANNOTATE_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  // Both name rows always show the input zone (so the user can
  // see where typing will land before they focus the row). The
  // block cursor is only painted on the focused row.
  zone_starts[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_W;
  zone_starts[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_W;
  if (focus_p1) {
    cursor_cols[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_p2) {
    cursor_cols[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Annotate setup", items, /*shortcuts=*/NULL,
                  /*disabled=*/NULL, cursor_cols, zone_starts, zone_widths,
                  TUI_ANNOTATE_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}

// Render a load-style modal (text input + Enter-to-load + error
// line). Both Load-position (CGP) and Load-game (GCG) use this
// with just title + prompt differing.
static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error);

void tui_game_render_load_position(struct ncplane *plane, const Theme *theme,
                                   const char *buf, int cursor,
                                   const char *error) {
  render_load_text_modal(plane, theme, " Load position ",
                         "Type or paste a CGP format position, or drag a .cgp "
                         "file to this window.",
                         buf, cursor, error);
}

void tui_game_render_load_game(struct ncplane *plane, const Theme *theme,
                               const char *buf, int cursor, const char *error) {
  render_load_text_modal(plane, theme, " Load game ",
                         "Drag a .gcg file to this window (or type its path).",
                         buf, cursor, error);
}

static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Layout: prompt (1) + spacer (1) + INPUT_ROWS + spacer (1) +
  // Load button (1) + error line (1) inside the box, plus the
  // top/bottom borders. INPUT_ROWS includes a one-row pad below
  // the lowest line of text so the cursor doesn't sit flush
  // against the next row.
  enum {
    MODAL_WIDTH = 80,
    INPUT_ROWS = 8,
    INTERIOR_LEFT = 3,
  };
  const int interior_w = MODAL_WIDTH - 2 - INTERIOR_LEFT - 2;
  // Total content height: prompt (1) + blank (1) + INPUT_ROWS +
  // hint (1) + error (1) = INPUT_ROWS + 4.
  const int height = INPUT_ROWS + 4 + 2; // +2 for top/bottom borders
  const int width = MODAL_WIDTH;

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  if ((unsigned)width >= plane_cols || (unsigned)height >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - height) / 2;
  const int left = (int)(plane_cols - width) / 2;

  // Reuse the shared modal plane (created on first use by
  // render_modal_ex) — same z-order rules apply. Re-create if
  // not present and size to our dimensions.
  if (grid_planes.modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)height;
    opts.cols = (unsigned)width;
    opts.name = "modal";
    grid_planes.modal = ncplane_create(plane, &opts);
    if (grid_planes.modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(grid_planes.modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != height || (int)cur_cols != width) {
      ncplane_resize_simple(grid_planes.modal, (unsigned)height,
                            (unsigned)width);
    }
    ncplane_move_yx(grid_planes.modal, top, left);
  }
  struct ncplane *mp = grid_planes.modal;
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  ncplane_set_channels(mp, 0);

  // Background fill + border chrome.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  // Title inset.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 2, title != NULL ? title : " Load ");

  // Prompt row.
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  if (prompt != NULL) {
    ncplane_putstr_yx(mp, 1, INTERIOR_LEFT, prompt);
  }

  // Walk the buffer into (row, col) display coordinates.
  // For each character paint it within the input area; record
  // where the cursor lands so we can paint it inverted last.
  const int input_top = 3;
  const int input_left = INTERIOR_LEFT;
  int row_in = 0;
  int col_in = 0;
  int cursor_row = 0;
  int cursor_col = 0;
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int i = 0; buf != NULL && buf[i] != '\0'; i++) {
    if (i == cursor) {
      cursor_row = row_in;
      cursor_col = col_in;
    }
    const char ch = buf[i];
    if (ch == '\n') {
      row_in++;
      col_in = 0;
      continue;
    }
    if (row_in < INPUT_ROWS && col_in < interior_w) {
      char one[2] = {ch, '\0'};
      ncplane_putstr_yx(mp, input_top + row_in, input_left + col_in, one);
    }
    col_in++;
    if (col_in >= interior_w) {
      // Soft wrap so a long line keeps flowing into the next row.
      row_in++;
      col_in = 0;
    }
  }
  // Cursor at end-of-buffer case.
  if (buf == NULL || cursor >= (int)(buf != NULL ? strlen(buf) : 0)) {
    cursor_row = row_in;
    cursor_col = col_in;
  }
  // Render the cursor as an inverted cell so the user always
  // sees where the next inserted/deleted character will land.
  if (cursor_row < INPUT_ROWS) {
    const int cy = input_top + cursor_row;
    const int cx =
        input_left + (cursor_col < interior_w ? cursor_col : interior_w - 1);
    theme_apply_fg(mp, theme->modal_bg);
    theme_apply_bg(mp, theme->modal_fg);
    ncplane_putstr_yx(mp, cy, cx, " ");
  }

  // Hint row below the input area.
  const int hint_row = input_top + INPUT_ROWS;
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  ncplane_putstr_yx(mp, hint_row, INTERIOR_LEFT, "Enter: load  Esc: cancel");

  // Error line just below the hint, dim red.
  if (error != NULL && error[0] != '\0') {
    const int err_row = hint_row + 1;
    theme_apply_fg(mp, theme->error_fg);
    theme_apply_bg(mp, theme->modal_bg);
    char trunc[96];
    snprintf(trunc, sizeof(trunc), "%.*s", interior_w, error);
    ncplane_putstr_yx(mp, err_row, INTERIOR_LEFT, trunc);
  }
}

void tui_game_render_time_picker(struct ncplane *plane, const Theme *theme,
                                 int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const int n = tui_time_picker_preset_count();
  // Format each row as "1 minute    ultra" — left-justified label
  // followed by a blurb. render_modal takes plain strings so we
  // pre-format into per-row buffers and pass pointers into items[].
  enum { ROW_BUF = 40 };
  static char buf[8][ROW_BUF];
  const char *items[8];
  const int rows = n < 8 ? n : 8;
  for (int i = 0; i < rows; i++) {
    snprintf(buf[i], ROW_BUF, "%-12s %s", tui_time_picker_preset_label(i),
             tui_time_picker_preset_blurb(i));
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Time control", items, NULL, rows, focus, 28);
}

void tui_game_render_quit_confirm(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[2] = {"No", "Yes"};
  const char *shortcuts[2] = {"N", "Y"};
  render_modal(plane, theme, "Quit?", items, shortcuts, 2, focus, 24);
}

// Helper for an arrow-adjusted Settings row. Renders
//   "<label>   ◀ <value> ▶"   when focused
//   "<label>   <value>"       when not focused
// `value` may be a fixed string (e.g., "lowercase") or numeric.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused) {
  if (focused) {
    snprintf(out, out_size, "%-13s\xe2\x97\x80 %s \xe2\x96\xb6", label, value);
  } else {
    snprintf(out, out_size, "%-13s%s", label, value);
  }
}

static const char *premium_labels_value(TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return "lowercase";
  case TUI_PREMIUM_LABELS_PUNCT:
    return "punctuation";
  case TUI_PREMIUM_LABELS_NONE:
    return "none";
  case TUI_PREMIUM_LABELS_UPPERCASE:
  case TUI_PREMIUM_LABELS_COUNT:
  default:
    return "uppercase";
  }
}

static const char *score_subscripts_value(TuiScoreSubscripts mode) {
  switch (mode) {
  case TUI_SCORE_SUBSCRIPTS_NONZERO:
    return "nonzero";
  case TUI_SCORE_SUBSCRIPTS_ALL:
    return "all";
  case TUI_SCORE_SUBSCRIPTS_OFF:
  case TUI_SCORE_SUBSCRIPTS_COUNT:
  default:
    return "off";
  }
}

// Display label for a rack-sort enum value. Concise on purpose so it
// fits in the right-aligned value column of the Settings modal:
//   "?+alpha" / "alpha+?" / "?+vow+con" / "vow+con+?"
// Leading "?+" means blanks come first; the rest is the letter
// ordering ("alpha" = alphabetical, "vow+con" = vowels then
// consonants).
static const char *rack_sort_value(TuiRackSort sort) {
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    return "?+alpha";
  case TUI_RACK_SORT_BLANKS_VOWELS:
    return "?+vow+con";
  case TUI_RACK_SORT_VOWELS:
    return "vow+con+?";
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    return "alpha+?";
  }
}

void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              TuiScoreSubscripts score_subscripts,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase, TuiRackSort rack_sort,
                              const char *lexicon, bool load_rit) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Lexicon row has been removed — lexicon is set only via the
  // New Game / Watch setup flow. Keep the param for signature
  // stability with existing callers.
  (void)lexicon;

  // Scale row. 2x needs both pixel graphics and a loaded font; if
  // either is missing, the row reports unavailable and arrow keys
  // become no-ops at this focus. Even when 2x is supported the
  // terminal may currently be too small to fit 2x cells — in that
  // case we still show the preference (the user may want to set 2x
  // and resize) but flag that it can't render right now.
  char scale_label[96];
  const bool scale_available = pixel_supported && font_available;
  if (!scale_available) {
    snprintf(scale_label, sizeof(scale_label), "Scale        unsupported here");
  } else {
    unsigned plane_rows = 0;
    unsigned plane_cols = 0;
    ncplane_dim_yx(plane, &plane_rows, &plane_cols);
    const bool layout_fits_2x =
        compute_effective_scale(2, plane_cols, plane_rows) >= 2;
    char value_buf[32];
    if (board_scale >= 2 && !layout_fits_2x) {
      // The setting stays editable so the user can step back to 1x
      // without resizing first, but the value spells out why the
      // board is still rendering as 1x.
      snprintf(value_buf, sizeof(value_buf), "2x \xc2\xb7 too small");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dx", board_scale);
    }
    format_setting_row(scale_label, sizeof(scale_label), "Scale", value_buf,
                       focus == TUI_SETTINGS_SCALE);
  }

  // Antialiasing row — only meaningful when 2x is engaged.
  char aa_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(aa_label, sizeof(aa_label), "Antialias    n/a at 1x");
  } else {
    format_setting_row(aa_label, sizeof(aa_label), "Antialias",
                       antialias ? "on" : "off", focus == TUI_SETTINGS_AA);
  }

  // Score subscripts row — also 2x-only.
  char sub_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(sub_label, sizeof(sub_label), "Subscript    n/a at 1x");
  } else {
    format_setting_row(sub_label, sizeof(sub_label), "Subscript",
                       score_subscripts_value(score_subscripts),
                       focus == TUI_SETTINGS_SUBSCRIPTS);
  }

  // Border row.
  char border_label[96];
  if (!pixel_supported) {
    snprintf(border_label, sizeof(border_label),
             "Border       unsupported here");
  } else {
    char value_buf[16];
    if (border_thickness <= 0) {
      snprintf(value_buf, sizeof(value_buf), "off");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dpx", border_thickness);
    }
    format_setting_row(border_label, sizeof(border_label), "Border", value_buf,
                       focus == TUI_SETTINGS_BORDER);
  }

  // Premium label row.
  char premium_label[96];
  format_setting_row(premium_label, sizeof(premium_label), "Premium",
                     premium_labels_value(premium_labels),
                     focus == TUI_SETTINGS_PREMIUM);

  // Blanks row.
  char blanks_label[96];
  format_setting_row(blanks_label, sizeof(blanks_label), "Blanks",
                     blank_uppercase ? "uppercase" : "lowercase",
                     focus == TUI_SETTINGS_BLANKS);

  // Rack-sort row.
  char rack_sort_label[96];
  format_setting_row(rack_sort_label, sizeof(rack_sort_label), "Rack sort",
                     rack_sort_value(rack_sort),
                     focus == TUI_SETTINGS_RACK_SORT);

  // RIT row. Plain on/off arrow toggle like Antialias.
  char rit_label[96];
  format_setting_row(rit_label, sizeof(rit_label), "RIT",
                     load_rit ? "on" : "off", focus == TUI_SETTINGS_RIT);

  // Antialias / Subscript / Border are only meaningful at 2x — hide
  // them entirely when the board isn't rendering at 2x rather than
  // showing greyed "n/a at 1x" placeholders. settings_visible() in
  // main.c mirrors this so arrow-key navigation skips them.
  const bool effective_2x = scale_available && board_scale >= 2;
  const char *items[TUI_SETTINGS_ITEM_COUNT];
  int n = 0;
  int display_focus = 0;
  // Walk enum order; append a row if visible, and translate the
  // caller's enum-valued focus into the corresponding display index.
  for (int idx = 0; idx < TUI_SETTINGS_ITEM_COUNT; idx++) {
    const bool is_2x_only =
        (idx == TUI_SETTINGS_AA || idx == TUI_SETTINGS_SUBSCRIPTS ||
         idx == TUI_SETTINGS_BORDER);
    if (is_2x_only && !effective_2x) {
      continue;
    }
    const char *label = NULL;
    switch (idx) {
    case TUI_SETTINGS_SCALE:
      label = scale_label;
      break;
    case TUI_SETTINGS_AA:
      label = aa_label;
      break;
    case TUI_SETTINGS_SUBSCRIPTS:
      label = sub_label;
      break;
    case TUI_SETTINGS_BORDER:
      label = border_label;
      break;
    case TUI_SETTINGS_PREMIUM:
      label = premium_label;
      break;
    case TUI_SETTINGS_BLANKS:
      label = blanks_label;
      break;
    case TUI_SETTINGS_RACK_SORT:
      label = rack_sort_label;
      break;
    case TUI_SETTINGS_RIT:
      label = rit_label;
      break;
    case TUI_SETTINGS_BACK:
      label = "Back";
      break;
    default:
      continue;
    }
    if (idx == focus) {
      display_focus = n;
    }
    items[n++] = label;
  }
  render_modal(plane, theme, "Settings", items, NULL, n, display_focus, 40);
}

#include "game_render.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "theme.h"
#include "tui_resize.h"
#include <ctype.h>
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
  // History switches to two columns once the panel is at least this wide.
  // At 68 cols the panel splits into two ~33-col halves, which is just
  // wide enough to fit a fullwidth 7-tile rack inside each player pill
  // alongside the name, score, and clock.
  HISTORY_TWO_COL_THRESHOLD = 68,
  // Analysis panel sizing.
  ANALYSIS_MIN_WIDTH = 30,        // narrower than this and we skip the panel
  ANALYSIS_DEFAULT_ROWS = 20,     // height used in "below history" mode when
                                  // history overflows
  ANALYSIS_GUTTER = 1,            // gap between history and right-side analysis
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
static int compute_effective_scale(int user_pref, unsigned plane_cols,
                                   unsigned plane_rows) {
  static const int cell_w_for[3] = {1, 2, 4};
  static const int cell_h_for[3] = {1, 1, 2};
  if (user_pref < 0) {
    user_pref = 0;
  } else if (user_pref > 2) {
    user_pref = 2;
  }
  for (int s = user_pref; s >= 0; s--) {
    const int cols = CELL_COL_BASE + BOARD_DIM * cell_w_for[s] +
                     BOARD_RIGHT_BORDER + RIGHT_COL_LEFT_OFFSET +
                     RIGHT_COL_MIN_WIDTH;
    // Board widget = top border (1) + col-label row (1) + cells + bottom
    // border (1). Then rack box, bag min 3 rows, status 1. Rack sits
    // flush under the board's bottom border, no gap.
    const int rack_box_rows = (s == 2) ? 4 : 3;
    const int rows =
        BOARD_DIM * cell_h_for[s] + 3 + rack_box_rows + 3 + 1;
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
    const int rows =
        state->history[idx].end_bonus != 0 ? 4 : 2; // mirrors history_entry_rows
    if (idx % 2 == 0) {
      left += rows;
    } else {
      right += rows;
    }
  }
  return left > right ? left : right;
}

static Layout compute_layout(struct ncplane *plane, int user_scale,
                             const TuiGameState *state) {
  Layout L = {0};
  ncplane_dim_yx(plane, &L.plane_rows, &L.plane_cols);

  L.status_row = (int)L.plane_rows - 1;

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
  L.analysis_placement = ANALYSIS_NONE;
  if (state != NULL) {
    if (L.right_col_width >= ANALYSIS_THREE_COL_THRESHOLD) {
      L.analysis_placement = ANALYSIS_RIGHT_OF_HISTORY;
    } else if (L.right_col_width >= HISTORY_TWO_COL_THRESHOLD) {
      L.analysis_placement = ANALYSIS_BELOW_HISTORY;
    } else {
      L.analysis_placement = ANALYSIS_BELOW_BAG;
    }
  }

  // Three-column case: peel the analysis strip off the right of the
  // right column BEFORE computing pill geometry so pills stay
  // centered/sized to the history slice only.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    const int analysis_width =
        L.right_col_width - HISTORY_TWO_COL_THRESHOLD - ANALYSIS_GUTTER;
    L.analysis_right = L.right_col_right;
    L.analysis_left = L.right_col_right - analysis_width + 1;
    L.right_col_right = L.analysis_left - 1 - ANALYSIS_GUTTER;
    L.right_col_width = L.right_col_right - L.right_col_left + 1;
  }

  L.two_col = L.right_col_width >= HISTORY_TWO_COL_THRESHOLD;

  if (L.two_col) {
    // Pills act as column headers — one above each history subcolumn.
    // Force equal pill widths so right-aligned content (score + clock)
    // sits the same number of cells from the leftmost "P" marker in
    // both pills; otherwise on odd-width right-column space, pill2 was
    // a column wider than pill1 and the ones-digit of P2's score drifted
    // one cell further from its "P" than P1's did. Pill1 stays anchored
    // to the panel's left edge, pill2 stays anchored to the right edge,
    // and any slack ends up as a slightly wider gutter between them.
    const int min_gutter = 2;
    const int half = (L.right_col_width - min_gutter) / 2;
    L.pill1_top = 0;
    L.pill1_bottom = PILL_HEIGHT - 1;
    L.pill1_left = L.right_col_left;
    L.pill1_right = L.pill1_left + half - 1;
    L.pill2_top = L.pill1_top;
    L.pill2_bottom = L.pill1_bottom;
    L.pill2_right = L.right_col_right;
    L.pill2_left = L.pill2_right - half + 1;
    L.history_top = L.pill1_bottom + 1;
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
  L.history_bottom = L.status_row - 1;

  // Default bag region: rack_bottom+1 to status_row-1. Tightened below
  // in the BELOW_BAG case so analysis can sit beneath the bag.
  L.bag_top = L.rack_bottom + 1;
  L.bag_bottom = L.status_row - 1;

  // Three-column: analysis fills the right strip from the very top of
  // the plane down to the status bar. There's nothing else over there,
  // so there's no reason to align with the pills row.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    L.analysis_top = 0;
    L.analysis_bottom = L.status_row - 1;
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
    if (analysis_rows >= 3 && L.history_bottom - analysis_rows > L.history_top) {
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
    const int interior_width = L.board_width - 2;
    const int chars = bag_unseen_chars(state);
    int content_lines =
        interior_width > 0 ? (chars + interior_width - 1) / interior_width : 1;
    if (content_lines < 1) {
      content_lines = 1;
    }
    // 2 borders + content rows + 1 tally row.
    int bag_height = 2 + content_lines + 1;
    const int total = L.bag_bottom - L.bag_top + 1;
    // Leave room for an analysis box of at least 3 rows + 1 gap.
    const int analysis_floor = 3 + 1;
    if (bag_height > total - analysis_floor) {
      bag_height = total - analysis_floor;
    }
    if (bag_height >= 3) {
      L.bag_bottom = L.bag_top + bag_height - 1;
      const int analysis_top = L.bag_bottom + 1;
      if (analysis_top <= L.status_row - 1 &&
          L.status_row - 1 - analysis_top + 1 >= 3) {
        L.analysis_top = analysis_top;
        L.analysis_bottom = L.status_row - 1;
        L.analysis_left = 0;
        L.analysis_right = L.board_width - 1;
        L.has_analysis = true;
      }
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
                                 TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return lower;
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
    const char *glyph = halfwidth ? halfwidth_premium_glyph(" ", labels)
                                  : premium_glyph(PREMIUM_EMPTY_GLYPH,
                                                  PREMIUM_EMPTY_GLYPH, labels);
    return (PremiumMarker){glyph, theme->premium_center_fg,
                           theme->premium_center_bg};
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bs);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bs);
  if (word_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("=", labels)
                                  : premium_glyph("TW", "tw", labels);
    return (PremiumMarker){glyph, theme->premium_tws_fg, theme->premium_tws_bg};
  }
  if (word_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("-", labels)
                                  : premium_glyph("DW", "dw", labels);
    return (PremiumMarker){glyph, theme->premium_dws_fg, theme->premium_dws_bg};
  }
  if (letter_mult == 3) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("\"", labels)
                                  : premium_glyph("TL", "tl", labels);
    return (PremiumMarker){glyph, theme->premium_tls_fg, theme->premium_tls_bg};
  }
  if (letter_mult == 2) {
    const char *glyph = halfwidth ? halfwidth_premium_glyph("'", labels)
                                  : premium_glyph("DL", "dl", labels);
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
static void draw_box(struct ncplane *plane, const Theme *theme, int top_row,
                     int left_col, int height, int width, const char *title) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  const int right_col = left_col + width - 1;
  const int bottom_row = top_row + height - 1;

  ncplane_putstr_yx(plane, top_row, left_col, BOX_TL);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, top_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(plane, top_row, right_col, BOX_TR);

  for (int row = top_row + 1; row < bottom_row; row++) {
    ncplane_putstr_yx(plane, row, left_col, BOX_VT);
    ncplane_putstr_yx(plane, row, right_col, BOX_VT);
  }

  ncplane_putstr_yx(plane, bottom_row, left_col, BOX_BL);
  for (int col = left_col + 1; col < right_col; col++) {
    ncplane_putstr_yx(plane, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(plane, bottom_row, right_col, BOX_BR);

  if (title != NULL && title[0] != '\0') {
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_putstr_yx(plane, top_row, left_col + 2, " ");
    ncplane_putstr(plane, title);
    ncplane_putstr(plane, " ");
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

// Cache for the 2x board pixel composite. ncblit_rgba is the FPS
// bottleneck even when the buffer hasn't changed; tracking a signature
// lets us skip the work and rely on notcurses keeping the plane's
// previous pixel content.
typedef struct {
  uint64_t version;     // game_state.render_version at last blit
  unsigned cdy, cdx;    // notcurses cell-pixel dims at last blit
  int param_a, param_b; // scale, antialias
  bool valid;
} BlitCache;

static BlitCache board_pixel_cache;
static BlitCache rack_pixel_cache;
static BlitCache label_pixel_cache;

static void invalidate_blit_caches(void) {
  board_pixel_cache.valid = false;
  rack_pixel_cache.valid = false;
  label_pixel_cache.valid = false;
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
}

void tui_game_render_reset_grids(void) { invalidate_grid_planes(); }

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
                               const Game *game, const LetterDistribution *ld,
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
  const Board *board = game_get_board(game);
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
      theme_apply_fg(plane, is_blank ? theme->blank_tile_fg : theme->tile_fg);
      theme_apply_bg(plane, theme->tile_bg);
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
static void blit_glyph_at(uint8_t *buf, int buf_w, int buf_h, int glyph_left,
                          int glyph_top, const TuiGlyph *g, ThemeRgb fg,
                          ThemeRgb bg) {
  if (g == NULL || g->width <= 0 || g->height <= 0) {
    return;
  }
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

static void render_board_pixel(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL) {
    return;
  }
  const int rows = BOARD_DIM * L->board_cell_h;
  const int cols = BOARD_DIM * L->board_cell_w;
  struct ncplane *p =
      acquire_grid_plane(&grid_planes.board, plane, "board_pixel",
                         CELL_ROW_BASE, CELL_COL_BASE, rows, cols);
  if (p == NULL) {
    return;
  }
  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned cdy = 0;
  unsigned cdx = 0;
  unsigned mby = 0;
  unsigned mbx = 0;
  ncplane_pixel_geom(p, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }
  // Cache short-circuit. Re-encoding a 4.5MB Kitty graphics image
  // through the PTY 60 times a second on identical content is the
  // primary FPS killer; bail out if nothing's changed since the last
  // successful blit. `render_version` is bumped by the bot worker on
  // each play and by main.c on every settings flip, so the only state
  // we still need to track per-frame is the cell-pixel ratio (font
  // size) which the resize path already handles via
  // invalidate_blit_caches().
  const uint64_t cur_version =
      atomic_load(&((TuiGameState *)state)->render_version);
  const int sub_mode = (int)state->score_subscripts;
  if (board_pixel_cache.valid && board_pixel_cache.version == cur_version &&
      board_pixel_cache.cdy == cdy && board_pixel_cache.cdx == cdx &&
      board_pixel_cache.param_a == L->scale &&
      board_pixel_cache.param_b ==
          ((state->antialias ? 1 : 0) | (sub_mode << 1))) {
    return;
  }
  const int tile_w = (int)cdx * L->board_cell_w;
  const int tile_h = (int)cdy * L->board_cell_h;
  const int buf_w = tile_w * BOARD_DIM;
  const int buf_h = tile_h * BOARD_DIM;
  if (buf_w <= 0 || buf_h <= 0) {
    return;
  }
  // Glyph sizes. Without subscripts: the historical 74% target leaves
  // ~13% margin top + bottom and reads well at every cell-pixel ratio.
  // With subscripts: shrink the letter to 80% of that (~59%) and pin
  // it to the upper-left so the digit fits in the bottom-right at
  // ~42%. Both caches use the same font, just different sizes — set
  // them once per render, not per cell.
  const bool subs_on =
      sub_mode != TUI_SCORE_SUBSCRIPTS_OFF && state->glyph_cache_sub != NULL;
  // Subscript-less tiles keep the 0.74 target (centered, generous).
  // Subscript-on tiles shrink the letter to 0.50 of tile height; the
  // subscript sits in the bottom-right at 0.24 with a 0.12 right /
  // 0.16 bottom margin. The letter's center shifts up-and-left by
  // 0.08 of tile dims so descenders (Q's tail) clear the subscript.
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(state->glyph_cache, letter_px, state->antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(state->glyph_cache_sub, sub_px, state->antialias);
  }

  uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
  if (buf == NULL) {
    return;
  }

  const Board *board = game_get_board(state->game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int tx = col * tile_w;
      const int ty = row * tile_h;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      ThemeRgb bg;
      ThemeRgb fg;
      uint32_t glyph_codepoint = 0;
      uint32_t glyph_second = 0; // second char of "TW"-style premium labels
      bool is_placed_tile = false;
      int tile_score = 0;

      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker = premium_marker_for_cell(
            theme, bs, row, col, state->premium_labels, L->board_cell_w);
        bg = marker.bg;
        fg = marker.fg;
        // marker.glyph is either an ideographic space (non-premium /
        // labels=NONE) or a 2-char ASCII label like "TW". Pull the
        // ASCII characters out when present.
        if ((unsigned char)marker.glyph[0] < 0x80 &&
            (unsigned char)marker.glyph[1] < 0x80 && marker.glyph[0] != ' ') {
          glyph_codepoint = (uint32_t)marker.glyph[0];
          glyph_second = (uint32_t)marker.glyph[1];
        }
      } else {
        is_placed_tile = true;
        const bool is_blank = get_is_blanked(ml);
        const bool render_uppercase = is_blank && state->blank_uppercase;
        const MachineLetter glyph_ml =
            render_uppercase ? get_unblanked_machine_letter(ml) : ml;
        bg = theme->tile_bg;
        fg = is_blank ? theme->blank_tile_fg : theme->tile_fg;
        const char *ascii = state->ld->ld_ml_to_hl[glyph_ml];
        if (ascii != NULL && ascii[0] != '\0' &&
            (unsigned char)ascii[0] < 0x80) {
          glyph_codepoint = (uint32_t)ascii[0];
        }
        // ld_get_score returns Equity (millipoints). For blanks the
        // backing array is zero unless the LD overrode it, which is
        // exactly the behavior we want — blank scores naturally drop
        // out under the "nonzero" mode.
        tile_score = equity_to_int(ld_get_score(state->ld, ml));
      }

      fill_tile_rect(buf, buf_w, tx, ty, tile_w, tile_h, bg);

      // Layout the letter. Subscript mode pins it upper-left so the
      // bottom-right corner is free for the score digits; otherwise
      // the historical centered placement.
      const bool show_subscript =
          is_placed_tile && subs_on &&
          (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL || tile_score != 0);
      if (glyph_codepoint != 0 && glyph_second == 0) {
        const TuiGlyph *g =
            tui_glyph_cache_get(state->glyph_cache, glyph_codepoint);
        if (is_placed_tile && subs_on && g != NULL && g->width > 0 &&
            g->height > 0) {
          // Centered, but biased up-and-left to clear the subscript
          // corner. A 1-digit subscript only needs a small horizontal
          // shift; a 2-digit subscript needs more room.
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
        // Two-char label ("TW" etc.) — render side by side, splitting
        // the tile's horizontal real estate in half.
        const TuiGlyph *g1 =
            tui_glyph_cache_get(state->glyph_cache, glyph_codepoint);
        const TuiGlyph *g2 =
            tui_glyph_cache_get(state->glyph_cache, glyph_second);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx, ty, tile_w / 2, tile_h, g1,
                            fg, bg);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx + tile_w / 2, ty, tile_w / 2,
                            tile_h, g2, fg, bg);
      }

      // Score subscript: right-aligned bottom-right with a 0.12 margin
      // on the right and bottom edges. We anchor on the digit's
      // BITMAP BOTTOM (gtop + height) rather than the font baseline so
      // the visible bottom of the digit lands exactly at margin_y from
      // the tile floor — baseline-anchoring gave the appearance of no
      // margin because the user reads the digit's actual pixels, not
      // its baseline.
      if (show_subscript) {
        char digits[8];
        snprintf(digits, sizeof(digits), "%d", tile_score);
        const int margin_x = (int)((double)tile_w * 0.12);
        const int margin_y = (int)((double)tile_h * 0.16);
        const int digit_bottom = ty + tile_h - margin_y;
        int pen_right = tx + tile_w - margin_x;
        for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
          const TuiGlyph *gd =
              tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)digits[i]);
          if (gd == NULL || gd->width <= 0) {
            continue;
          }
          const int gleft = pen_right - gd->width;
          const int gtop = digit_bottom - gd->height;
          blit_glyph_at(buf, buf_w, buf_h, gleft, gtop, gd, fg, bg);
          pen_right = gleft - 1; // small kerning gap between digits
        }
      }
    }
  }

  // Tile borders. The 1x text path uses render_board_grid + a separate
  // overlay plane; at 2x the composite owns the only plane that covers
  // the board, so paint the lines directly into the same RGBA buffer
  // before we ship it.
  overlay_grid_lines(buf, buf_w, buf_h, BOARD_DIM, BOARD_DIM, tile_h, tile_w,
                     state->border_thickness, theme->bg);

  struct ncvisual_options vopts = {0};
  vopts.n = p;
  vopts.blitter = NCBLIT_PIXEL;
  vopts.leny = (unsigned)buf_h;
  vopts.lenx = (unsigned)buf_w;
  ncblit_rgba(buf, buf_w * 4, &vopts);
  free(buf);

  // Record the parameters of the blit we just completed so the next
  // frame can short-circuit if nothing's changed. param_b packs
  // antialias (bit 0) and the score_subscripts tri-state (bits 1-2)
  // so a flip of either invalidates the cache.
  board_pixel_cache.valid = true;
  board_pixel_cache.version = cur_version;
  board_pixel_cache.cdy = cdy;
  board_pixel_cache.cdx = cdx;
  board_pixel_cache.param_a = L->scale;
  board_pixel_cache.param_b = (state->antialias ? 1 : 0) | (sub_mode << 1);
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
      CELL_COL_BASE, BOARD_DIM * L->board_cell_h,
      BOARD_DIM * L->board_cell_w);
  if (p == NULL) {
    return;
  }
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
      board_pixel_cache.cdx == cdx &&
      board_pixel_cache.param_a == L->scale &&
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
static void render_board_labels_pixel(struct ncplane *plane,
                                      const Theme *theme,
                                      const TuiGameState *state,
                                      const Layout *L) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) ||
      state->glyph_cache_sub == NULL) {
    return;
  }
  const int col_rows = 1;
  const int col_cols = BOARD_DIM * L->board_cell_w;
  const int row_rows = BOARD_DIM * L->board_cell_h;
  const int row_cols = CELL_COL_BASE;

  struct ncplane *col_p =
      acquire_grid_plane(&grid_planes.labels_col, plane, "board_col_labels", 0,
                         CELL_COL_BASE, col_rows, col_cols);
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
      label_pixel_cache.cdx == cdx &&
      label_pixel_cache.param_a == L->scale &&
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
    fill_tile_rect(buf, buf_w, 0, 0, buf_w, buf_h, bg);
    const int right_margin = (int)((double)cdy * 0.22);
    const int content_right = buf_w - right_margin;
    const int slot_w =
        content_right > 0 ? content_right / 2 : 0; // per-digit slot
    for (int row = 0; row < BOARD_DIM; row++) {
      char label[4];
      snprintf(label, sizeof(label), "%2d", row + 1);
      const int cell_top = row * L->board_cell_h * icdy;
      const int cell_h_px = L->board_cell_h * icdy;
      // Center a 1-row-tall label box vertically in the 2-row cell.
      const int box_top = cell_top + (cell_h_px - icdy) / 2;
      const int baseline = box_top + (int)(cdy * 0.78);
      for (int i = 0; i < 2; i++) {
        const char ch = label[i];
        if (ch == '\0' || ch == ' ') {
          continue;
        }
        const TuiGlyph *g =
            tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)ch);
        if (g == NULL || g->width <= 0 || g->height <= 0) {
          continue;
        }
        // Pack the two slots flush against content_right so the
        // rightmost digit lines up just inside right_margin from the
        // board's left edge.
        const int slot_left = content_right - (2 - i) * slot_w;
        const int glyph_left = slot_left + (slot_w - g->width) / 2;
        const int glyph_top = baseline - g->bearing_y;
        blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
      }
    }
    struct ncvisual_options vopts = {0};
    vopts.n = row_p;
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)buf_h;
    vopts.lenx = (unsigned)buf_w;
    ncblit_rgba(buf, buf_w * 4, &vopts);
    free(buf);
  }

  label_pixel_cache.valid = true;
  label_pixel_cache.version = 0; // unused for the labels path
  label_pixel_cache.cdy = cdy;
  label_pixel_cache.cdx = cdx;
  label_pixel_cache.param_a = L->scale;
  label_pixel_cache.param_b = state->antialias ? 1 : 0;
}

// Count of letters currently on the board (regular + blanks).
static int board_tile_count(const Game *game) {
  const Board *board = game_get_board(game);
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
  snprintf(title, sizeof(title), "Board (%d)", board_tile_count(state->game));
  draw_box(plane, theme, 0, 0, height, L->board_width, title);
}

static void render_board(struct ncplane *plane, const Theme *theme,
                         const TuiGameState *state, const Layout *L) {
  render_board_box(plane, theme, state, L);
  if (L->scale >= 2 && state->glyph_cache != NULL) {
    render_board_labels_pixel(plane, theme, state, L);
    render_board_pixel(plane, theme, state, L);
    return;
  }
  // 2x-only pixel label planes are no-ops at scale < 2 but their stale
  // image data would otherwise sit on top of the text-mode layout. Drop
  // them; they'll be reacquired on the next 2x render.
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
  render_board_cells(plane, theme, state->game, state->ld,
                     state->blank_uppercase, state->premium_labels,
                     state->border_thickness, L->board_cell_w, CELL_ROW_BASE,
                     CELL_COL_BASE);
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
  render_board_cells(plane, theme, game, ld, blank_uppercase, premium_labels,
                     border_thickness, CELL_WIDTH, top, left);
}

// ── Rack panel ────────────────────────────────────────────────────────────
//
// Rack tiles scale alongside the board: at scale=2 we composite an RGBA
// strip via FreeType (same path as the board); at scale=1 we use
// fullwidth Unicode glyphs in cells; at scale=0 we collapse to single
// ASCII chars. The panel box gains one row at scale=2 so the 2-row
// tiles fit.

static void render_rack_panel_pixel(struct ncplane *plane, const Theme *theme,
                                    const TuiGameState *state, const Layout *L,
                                    int start_col, int tile_count) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL ||
      tile_count <= 0) {
    return;
  }
  const int cell_w = L->board_cell_w; // 4
  const int cell_h = L->board_cell_h; // 2
  const int rows = cell_h;
  const int cols = tile_count * cell_w;
  // Rack content sits on the rows immediately below the box top. With
  // box top at L->rack_top, the 2-row content area is rows
  // (rack_top + 1, rack_top + 2).
  struct ncplane *p =
      acquire_grid_plane(&grid_planes.rack, plane, "rack_pixel",
                         L->rack_top + 1, start_col, rows, cols);
  if (p == NULL) {
    return;
  }
  unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
  ncplane_pixel_geom(p, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }
  const uint64_t cur_version =
      atomic_load(&((TuiGameState *)state)->render_version);
  if (rack_pixel_cache.valid && rack_pixel_cache.version == cur_version &&
      rack_pixel_cache.cdy == cdy && rack_pixel_cache.cdx == cdx &&
      rack_pixel_cache.param_a == tile_count &&
      rack_pixel_cache.param_b == (state->antialias ? 1 : 0)) {
    return;
  }
  const int tile_w = (int)cdx * cell_w;
  const int tile_h = (int)cdy * cell_h;
  const int buf_w = tile_w * tile_count;
  const int buf_h = tile_h;
  // Match the board's subscript-mode sizing so rack tiles look the
  // same as placed tiles at the same scale.
  const int sub_mode = (int)state->score_subscripts;
  const bool subs_on = sub_mode != TUI_SCORE_SUBSCRIPTS_OFF &&
                       state->glyph_cache_sub != NULL;
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(state->glyph_cache, letter_px, state->antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(state->glyph_cache_sub, sub_px, state->antialias);
  }

  uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
  if (buf == NULL) {
    return;
  }

  const int player_idx = game_get_player_on_turn_index(state->game);
  const Rack *rack = player_get_rack(game_get_player(state->game, player_idx));
  const LetterDistribution *ld = state->ld;

  int tile_idx = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count && tile_idx < tile_count;
         copy++, tile_idx++) {
      const int tx = tile_idx * tile_w;
      fill_tile_rect(buf, buf_w, tx, 0, tile_w, tile_h, theme->rack_tile_bg);
      const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
      const TuiGlyph *g =
          (ascii != NULL && ascii[0] != '\0' &&
           (unsigned char)ascii[0] < 0x80)
              ? tui_glyph_cache_get(state->glyph_cache, (uint32_t)ascii[0])
              : NULL;
      // Score for shift sizing + subscript. An undesignated blank reads
      // as "?" but we always subscript it "0", so it's a 1-digit score.
      const int tile_score =
          (ml == 0) ? 0 : equity_to_int(ld_get_score(ld, (MachineLetter)ml));
      if (g != NULL && g->width > 0 && g->height > 0) {
        if (subs_on) {
          // Same centered-with-bias placement as placed board tiles —
          // digit-aware horizontal shift.
          const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
          const int shift_x = (int)((double)tile_w * shift_x_frac);
          const int shift_y = (int)((double)tile_h * 0.08);
          const int baseline = 0 + (int)(tile_h * 0.72) - shift_y;
          const int glyph_top = baseline - g->bearing_y;
          const int glyph_left = tx + (tile_w - g->width) / 2 - shift_x;
          blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g,
                        theme->rack_tile_fg, theme->rack_tile_bg);
        } else {
          blit_glyph_into_buf(buf, buf_w, buf_h, tx, 0, tile_w, tile_h, g,
                              theme->rack_tile_fg, theme->rack_tile_bg);
        }
      }

      // Subscript: an undesignated blank in the rack always gets a "0"
      // when subscripts are enabled (it makes the "?" tile's worth
      // explicit). Regular letters use ld_get_score and follow the
      // configured mode.
      if (subs_on) {
        const bool show_subscript =
            (ml == 0) || (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL) ||
            (tile_score != 0);
        if (show_subscript) {
          char digits[8];
          snprintf(digits, sizeof(digits), "%d", tile_score);
          const int margin_x = (int)((double)tile_w * 0.12);
          const int margin_y = (int)((double)tile_h * 0.16);
          const int digit_bottom = 0 + tile_h - margin_y;
          int pen_right = tx + tile_w - margin_x;
          for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
            const TuiGlyph *gd = tui_glyph_cache_get(state->glyph_cache_sub,
                                                    (uint32_t)digits[i]);
            if (gd == NULL || gd->width <= 0) {
              continue;
            }
            const int gleft = pen_right - gd->width;
            const int gtop = digit_bottom - gd->height;
            blit_glyph_at(buf, buf_w, buf_h, gleft, gtop, gd,
                          theme->rack_tile_fg, theme->rack_tile_bg);
            pen_right = gleft - 1;
          }
        }
      }
    }
  }

  // Borders along bottom + right of each tile, in theme->bg, just like
  // the board composite. Single ncblit_rgba ships the whole strip.
  overlay_grid_lines(buf, buf_w, buf_h, 1, tile_count, tile_h, tile_w,
                     state->border_thickness, theme->bg);

  struct ncvisual_options vopts = {0};
  vopts.n = p;
  vopts.blitter = NCBLIT_PIXEL;
  vopts.leny = (unsigned)buf_h;
  vopts.lenx = (unsigned)buf_w;
  ncblit_rgba(buf, buf_w * 4, &vopts);
  free(buf);

  rack_pixel_cache.valid = true;
  rack_pixel_cache.version = cur_version;
  rack_pixel_cache.cdy = cdy;
  rack_pixel_cache.cdx = cdx;
  rack_pixel_cache.param_a = tile_count;
  rack_pixel_cache.param_b = state->antialias ? 1 : 0;
}

static void render_rack_panel(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L) {
  const int box_height = L->rack_bottom - L->rack_top + 1;
  const int player_idx = game_get_player_on_turn_index(state->game);
  const Player *player = game_get_player(state->game, player_idx);
  const Rack *rack = player_get_rack(player);
  char title[32];
  snprintf(title, sizeof(title), "Rack (%d)", rack_get_total_letters(rack));
  draw_box(plane, theme, L->rack_top, 0, box_height, L->board_width, title);
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
  int col_offset = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count; copy++) {
      theme_apply_fg(plane, theme->rack_tile_fg);
      theme_apply_bg(plane, theme->rack_tile_bg);
      if (halfwidth) {
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
static void render_bag_panel(struct ncplane *plane, const Theme *theme,
                             const TuiGameState *state, const Layout *L) {
  const Bag *bag = game_get_bag(state->game);
  const int bag_count = bag_get_letters(bag);
  char title[32];
  snprintf(title, sizeof(title), "Bag (%d)", bag_count);

  const int height = L->bag_bottom - L->bag_top + 1;
  if (height < 3) {
    return;
  }
  draw_box(plane, theme, L->bag_top, 0, height, L->board_width, title);

  // Tally bag + off-turn rack into a per-ml count array (the on-turn rack
  // is "seen", so it doesn't count as unseen).
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

  // Build the dense inline "?? AAAAAAA BB ..." string.
  char line[512];
  size_t pos = 0;
  for (int ml = 0; ml < ld_size; ml++) {
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
    size_t limit = i + (size_t)interior_width;
    if (limit >= pos) {
      limit = pos;
    } else {
      // Try to break at a space.
      size_t back = limit;
      while (back > i && line[back] != ' ') {
        back--;
      }
      if (back > i) {
        limit = back;
      }
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
    while (i < pos && line[i] == ' ') {
      i++;
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
  const int long_bytes = snprintf(tally_long, sizeof(tally_long),
                                  "%d vowels \xc2\xb7 %d consonants", vowels,
                                  consonants);
  const int long_cols = long_bytes - 1;
  snprintf(tally_short, sizeof(tally_short), "%d vows/%d cons", vowels,
           consonants);
  const char *tally =
      (long_cols <= interior_width) ? tally_long : tally_short;
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
  if (game_get_player_on_turn_index(state->game) == player_idx &&
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
                               int top, int left, int right) {
  const int width = right - left + 1;
  draw_box(plane, theme, top, left, PILL_HEIGHT, width, NULL);

  const Player *player = game_get_player(state->game, player_idx);
  const bool on_turn = game_get_player_on_turn_index(state->game) == player_idx;
  const int content_row = top + 1;
  // One col of padding inside the box (was 2). The on-turn arrow lives
  // in the very first interior col so the rack has more room.
  const int content_left = left + 1;
  const int content_right = right - 1;

  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, content_row, content_left,
                    on_turn ? "\xe2\x96\xb6 " : "  ");
  theme_apply_fg(plane, theme->fg);
  // Short names — "P1" / "P2" — leave more space for the rack.
  char name[8];
  snprintf(name, sizeof(name), "P%d", player_idx + 1);
  ncplane_putstr(plane, name);

  // Right side: clock and score, separated by a single col gap. Clock
  // tops out at "99:59" (5 chars).
  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           equity_to_int(player_get_score(player)));
  const double remaining = seconds_remaining(state, player_idx);
  char clock_str[16];
  format_clock(remaining < 0 ? 0 : (int)remaining, clock_str,
               sizeof(clock_str));

  const int clock_len = (int)strlen(clock_str);
  const int clock_col = content_right - clock_len + 1;
  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, content_row, clock_col, clock_str);

  const int score_len = (int)strlen(score_str);
  const int score_col = clock_col - 1 - score_len;
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, content_row, score_col, score_str);

  // Fullwidth rack between the name and the score, on tile_bg. Each tile
  // is 2 cols wide. After "▶ P1 " (4 chars + 1 gap) the rack starts at
  // content_left + 5.
  const Rack *rack = player_get_rack(player);
  const LetterDistribution *ld = state->ld;
  const int rack_left = content_left + 5;
  const int rack_right_max = score_col - 2;
  if (rack_right_max >= rack_left + 1) {
    int rcol = rack_left;
    theme_apply_fg(plane, theme->rack_tile_fg);
    theme_apply_bg(plane, theme->rack_tile_bg);
    for (int ml = 0; ml < ld_get_size(ld) && rcol + 1 <= rack_right_max; ml++) {
      const int count = rack_get_letter(rack, (MachineLetter)ml);
      for (int copy = 0; copy < count && rcol + 1 <= rack_right_max; copy++) {
        const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, content_row, rcol, fullwidth);
        } else {
          const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, content_row, rcol, " ");
          ncplane_putstr(plane, ascii);
        }
        rcol += 2;
      }
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

static int history_entry_rows(const TuiHistoryEntry *e) {
  return e->end_bonus != 0 ? 4 : 2;
}

// Render a GCG-style move notation with played-through letters (the
// segments wrapped in parentheses) unbolded — everything outside parens
// reads as bold. The history and analysis panels share this so a
// played-through "(O)" looks consistent in both. Caller positions the
// text; this routine handles segmenting and style changes only.
static void render_move_styled(struct ncplane *plane, int row, int col,
                               const char *move_str) {
  if (move_str == NULL || *move_str == '\0') {
    return;
  }
  const char *p = move_str;
  const char *end = p + strlen(move_str);
  int x = col;
  while (p < end) {
    const char *seg_end;
    bool seg_bold;
    if (*p == '(') {
      seg_bold = false;
      seg_end = p;
      while (seg_end < end && *seg_end != ')') {
        seg_end++;
      }
      if (seg_end < end) {
        seg_end++; // include the close paren
      }
    } else {
      seg_bold = true;
      seg_end = p;
      while (seg_end < end && *seg_end != '(') {
        seg_end++;
      }
    }
    ncplane_set_styles(plane, seg_bold ? NCSTYLE_BOLD : 0);
    char buf[64];
    size_t len = (size_t)(seg_end - p);
    if (len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    memcpy(buf, p, len);
    buf[len] = '\0';
    ncplane_putstr_yx(plane, row, x, buf);
    x += (int)strlen(buf);
    p = seg_end;
  }
  ncplane_set_styles(plane, 0);
}

static void render_history_entry(struct ncplane *plane, const Theme *theme,
                                 const TuiHistoryEntry *e, int idx, int row,
                                 int interior_left, int interior_right,
                                 int row_bottom_inclusive) {
  // ── Row 1 (lighter): " 18. L1 RE(W)I(N)              +38" ──────────────
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, theme->fg);
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "%2d. ", idx + 1);
  if (e->pending) {
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
    theme_apply_fg(plane, theme->tile_fg);
    theme_apply_bg(plane, theme->tile_bg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, interior_left + digit_start, tile_part);
    ncplane_set_styles(plane, 0);
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, theme->bg);
    if (prefix[after_period] != '\0') {
      ncplane_putstr_yx(plane, row, interior_left + after_period,
                        prefix + after_period);
    }
  } else {
    ncplane_putstr_yx(plane, row, interior_left, prefix);
  }

  if (e->pending) {
    // Bot is still computing this turn — show a braille spinner where
    // the move notation will go and leave the +score column blank.
    // 10-frame cycle at ~80ms per frame derives from CLOCK_MONOTONIC
    // so the animation runs even when the renderer is otherwise idle.
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
  } else {
    render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                       e->move_str);

    char delta_str[16];
    snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
    const int delta_len = (int)strlen(delta_str);
    const int delta_col = interior_right - delta_len + 1;
    if (delta_col > interior_left + (int)strlen(prefix)) {
      ncplane_putstr_yx(plane, row, delta_col, delta_str);
    }
  }
  ncplane_set_styles(plane, 0);

  // ── Row 2 (darker): "     4:42 AEINRT                91" ───────────────
  if (row + 1 > row_bottom_inclusive) {
    return;
  }
  const int row2 = row + 1;

  theme_apply_fg(plane, theme->dim_fg);
  char clock_str[16];
  format_clock(e->clock_at_start < 0 ? 0 : e->clock_at_start, clock_str,
               sizeof(clock_str));
  char left_line[48];
  snprintf(left_line, sizeof(left_line), "    %s %s", clock_str,
           e->rack_str[0] ? e->rack_str : "—");
  ncplane_putstr_yx(plane, row2, interior_left, left_line);

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
  }

  // ── Row 3 (going-out bonus delta): "    (LNRU)               +8" ──────
  // Rendered with the same shape as a scoring play — opponent's leftover
  // rack on the left in standard GCG (LNRU) notation, bonus delta
  // right-aligned. Color matches the move row (theme->fg) so the bonus
  // visually parses as "another play."
  if (e->end_bonus == 0 || row + 2 > row_bottom_inclusive) {
    return;
  }
  const int row3 = row + 2;
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, theme->fg);
  char bonus_left[48];
  if (e->end_rack_str[0] != '\0') {
    snprintf(bonus_left, sizeof(bonus_left), "    (%s)", e->end_rack_str);
  } else {
    snprintf(bonus_left, sizeof(bonus_left), "    ");
  }
  ncplane_putstr_yx(plane, row3, interior_left, bonus_left);

  char delta3_str[16];
  snprintf(delta3_str, sizeof(delta3_str), "+%d", e->end_bonus);
  const int delta3_len = (int)strlen(delta3_str);
  const int delta3_col = interior_right - delta3_len + 1;
  if (delta3_col > interior_left + (int)strlen(bonus_left)) {
    ncplane_putstr_yx(plane, row3, delta3_col, delta3_str);
  }

  // ── Row 4 (final score): "                                    489" ────
  // Bold, right-aligned in the dim color — mirrors the total row of a
  // scoring play and represents the final game total for this player.
  if (row + 3 > row_bottom_inclusive) {
    return;
  }
  const int row4 = row + 3;
  theme_apply_fg(plane, theme->dim_fg);
  char total4_str[16];
  snprintf(total4_str, sizeof(total4_str), "%d", e->total_after + e->end_bonus);
  const int total4_len = (int)strlen(total4_str);
  const int total4_col = interior_right - total4_len + 1;
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, row4, total4_col, total4_str);
  ncplane_set_styles(plane, 0);
}

static void render_history_panel(struct ncplane *plane, const Theme *theme,
                                 const TuiGameState *state, const Layout *L) {
  const int width = L->right_col_right - L->right_col_left + 1;
  const int height = L->history_bottom - L->history_top + 1;
  if (height < 3) {
    return;
  }
  draw_box(plane, theme, L->history_top, L->right_col_left, height, width,
           NULL);

  if (state->history_count == 0) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const int center_row = L->history_top + height / 2;
    const char *msg = "No moves yet.";
    const int interior_width = width - 2;
    const int msg_col =
        L->right_col_left + 1 + (interior_width - (int)strlen(msg)) / 2;
    ncplane_putstr_yx(plane, center_row, msg_col, msg);
    return;
  }

  const int top = L->history_top + 1;       // first interior row
  const int bottom = L->history_bottom - 1; // last interior row (inclusive)
  const int rows_avail = bottom - top + 1;

  if (!L->two_col) {
    const int interior_left = L->right_col_left + 2;
    const int interior_right = L->right_col_right - 2;

    // Walk backwards to find the oldest entry that still fits, so the
    // most recent entries always show.
    int first = state->history_count;
    int rows_used = 0;
    while (first > 0) {
      const int rows = history_entry_rows(&state->history[first - 1]);
      if (rows_used + rows > rows_avail) {
        break;
      }
      rows_used += rows;
      first--;
    }
    int row = top;
    for (int idx = first; idx < state->history_count; idx++) {
      const TuiHistoryEntry *e = &state->history[idx];
      render_history_entry(plane, theme, e, idx, row, interior_left,
                           interior_right, bottom);
      row += history_entry_rows(e);
    }
    return;
  }

  // Two-column layout. Entries are assigned to columns by absolute index
  // parity (even → left, odd → right) so the going-out player's row
  // stays in whichever column they happened to land on.
  const int gutter = 2;
  const int half_width = (width - 2 - gutter) / 2;
  const int left_l = L->right_col_left + 2;
  const int left_r = left_l + half_width - 1;
  const int right_l = left_r + 1 + gutter;
  const int right_r = L->right_col_right - 2;

  // Walk backwards, tracking per-column row usage, to find the oldest
  // entry that still fits in its target column.
  int left_used = 0;
  int right_used = 0;
  int first = state->history_count;
  while (first > 0) {
    const int idx = first - 1;
    const int rows = history_entry_rows(&state->history[idx]);
    int *used = (idx % 2 == 0) ? &left_used : &right_used;
    if (*used + rows > rows_avail) {
      break;
    }
    *used += rows;
    first--;
  }

  int row_left = top;
  int row_right = top;
  for (int idx = first; idx < state->history_count; idx++) {
    const TuiHistoryEntry *e = &state->history[idx];
    const int rows = history_entry_rows(e);
    if ((idx % 2) == 0) {
      render_history_entry(plane, theme, e, idx, row_left, left_l, left_r,
                           bottom);
      row_left += rows;
    } else {
      render_history_entry(plane, theme, e, idx, row_right, right_l, right_r,
                           bottom);
      row_right += rows;
    }
  }
}

// ── Analysis panel ────────────────────────────────────────────────────────
//
// Shows the latest sim leaderboard. The header is a 3×2 tile-styled
// block holding the on-turn move number ("23.") so it visually
// matches the board's played tiles. Below it the ranked candidates
// scroll: each row shows rank, move notation, win%, and mean equity.

static void render_analysis_panel(struct ncplane *plane, const Theme *theme,
                                  const TuiGameState *state,
                                  const Layout *L) {
  if (!L->has_analysis) {
    return;
  }
  const int height = L->analysis_bottom - L->analysis_top + 1;
  const int width = L->analysis_right - L->analysis_left + 1;
  if (height < 3 || width < 6) {
    return;
  }
  draw_box(plane, theme, L->analysis_top, L->analysis_left, height, width,
           "Analysis");

  const int interior_left = L->analysis_left + 1;
  const int interior_right = L->analysis_right - 1;
  const int interior_top = L->analysis_top + 1;
  const int interior_bottom = L->analysis_bottom - 1;

  // Top play sits flush against the "Analysis" title row.
  const int list_top = interior_top;

  // Pull the (locked, sorted) display plays. Returns false when the
  // sim hasn't run yet — show the placeholder in that case.
  SimResults *results = state->sim_results;
  if (results == NULL ||
      !sim_results_lock_and_sort_display_simmed_plays(results)) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const char *msg = "(no sim data yet)";
    const int msg_col =
        interior_left + (interior_right - interior_left + 1 - (int)strlen(msg)) / 2;
    if (list_top <= interior_bottom) {
      ncplane_putstr_yx(plane, list_top, msg_col, msg);
    }
    return;
  }

  const int num_plays = sim_results_get_number_of_plays(results);
  const Board *board = game_get_board(state->game);

  // Layout per row:
  //   "  1. 8E TURNS                   67.3%  +32.5"
  // We left-anchor rank+move, right-anchor the two numeric columns.
  // Width budget for the move string is whatever's left between them.
  theme_apply_bg(plane, theme->bg);
  int row = list_top;
  for (int i = 0; i < num_plays && row <= interior_bottom; i++) {
    const SimmedPlay *play =
        sim_results_get_display_simmed_play(results, i);
    if (play == NULL) {
      continue;
    }
    const Move *move = simmed_play_get_move(play);
    const Stat *win_stat = simmed_play_get_win_pct_stat(play);
    const Stat *eq_stat = simmed_play_get_equity_stat(play);
    const double win_pct = stat_get_mean(win_stat) * 100.0;
    // equity_stat is fed via equity_to_double in
    // simmed_play_add_equity_stat, so the mean is already in points
    // (equity_to_double converts millipoints → points internally).
    const double eq_pts = stat_get_mean(eq_stat);

    char rank_str[8];
    snprintf(rank_str, sizeof(rank_str), "%2d. ", i + 1);
    char win_str[16];
    snprintf(win_str, sizeof(win_str), "%5.1f%%", win_pct);
    char eq_str[16];
    snprintf(eq_str, sizeof(eq_str), "%+6.1f", eq_pts);

    // Build move notation via the engine string-builder, same as
    // history entries.
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, board, move, state->ld, false);
    size_t move_len = 0;
    char *move_dump = string_builder_dump(sb, &move_len);
    string_builder_destroy(sb);

    const int rank_len = (int)strlen(rank_str);
    const int win_len = (int)strlen(win_str);
    const int eq_len = (int)strlen(eq_str);
    const int gap = 2; // gap between win% and eq columns

    const int eq_col = interior_right - eq_len + 1;
    const int win_col = eq_col - gap - win_len;
    const int move_col = interior_left + rank_len;
    const int move_max = win_col - 1 - move_col; // room before win%

    // Truncate move text to fit available space.
    if (move_dump != NULL) {
      if (move_max > 0 && (int)strlen(move_dump) > move_max) {
        if (move_max < (int)sizeof(char) * 4) {
          move_dump[0] = '\0';
        } else {
          move_dump[move_max] = '\0';
        }
      }
    }

    theme_apply_fg(plane, theme->dim_fg);
    ncplane_putstr_yx(plane, row, interior_left, rank_str);

    if (move_dump != NULL && move_max > 0) {
      theme_apply_fg(plane, theme->fg);
      render_move_styled(plane, row, move_col, move_dump);
    }

    if (win_col > move_col + (int)strlen(move_dump != NULL ? move_dump : "")) {
      theme_apply_fg(plane, theme->fg);
      ncplane_putstr_yx(plane, row, win_col, win_str);
      theme_apply_fg(plane, theme->dim_fg);
      ncplane_putstr_yx(plane, row, eq_col, eq_str);
    }

    free(move_dump);
    row++;
  }

  sim_results_unlock_display_infos(results);
}

// EMA-smoothed FPS based on the interval between status-bar renders.
// Status-bar render happens once per top-level render call, which is
// what we actually care about — terminal frame rate after all the
// game/grid/composite work. alpha=0.1 gives a stable readout that
// still responds within a second or so when the rate genuinely shifts.
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
  if (dt <= 0.0 || dt > 5.0) {
    // Skip pathological intervals (clock jumps, long pauses) so they
    // can't poison the running average with a one-frame spike.
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
static void render_status_bar(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L,
                              TuiModalState modal) {
  const int row = L->status_row;
  if (row < 0) {
    return;
  }

  // Paint the whole row with the header bg so it looks like a bar.
  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }

  // Left side: "Language · Lexicon · 60 fps". FPS is tacked onto the
  // left so it sits next to the other engine-state readouts; the right
  // side is reserved for transient control hints. We show an integer to
  // avoid the last digit twitching every frame.
  const double fps = measure_fps();
  char left_buf[128];
  if (fps > 0.0) {
    snprintf(left_buf, sizeof(left_buf), " %s \xc2\xb7 %s \xc2\xb7 %d fps",
             language_for_lexicon(state->lexicon), state->lexicon,
             (int)(fps + 0.5));
  } else {
    snprintf(left_buf, sizeof(left_buf), " %s \xc2\xb7 %s",
             language_for_lexicon(state->lexicon), state->lexicon);
  }
  ncplane_putstr_yx(plane, row, 0, left_buf);

  // Right side: dynamic shortcut hint depending on what modal is open.
  const char *hint = " esc for menu ";
  switch (modal) {
  case TUI_MODAL_MAIN_MENU:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 enter confirm \xc2"
           "\xb7 esc back ";
    break;
  case TUI_MODAL_SETTINGS:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 \xe2\x86\x90\xe2"
           "\x86\x92 adjust \xc2\xb7 esc back ";
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

  render_board(plane, theme, state, &L);
  // 2x bakes the grid into its RGBA composite. At 0x/1x the tiles are
  // std-plane cell text, so the grid rides a separate transparent-base
  // pixel plane on top — same look the user wants, just delivered via
  // an overlay instead of in-buffer. Notcurses re-emits a pixel plane
  // every render, so this caps the FPS around 250 with borders on;
  // accepted trade-off for the visual.
  if (L.scale < 2 && state->border_thickness > 0) {
    render_board_grid_overlay(
        plane, theme, &L, state->border_thickness,
        atomic_load(&((TuiGameState *)state)->render_version));
  }
  render_rack_panel(plane, theme, state, &L);
  render_bag_panel(plane, theme, state, &L);

  (void)time_per_side_seconds; // now read from state->time_per_side_seconds
  render_player_pill(plane, theme, state, 0, L.pill1_top, L.pill1_left,
                     L.pill1_right);
  render_player_pill(plane, theme, state, 1, L.pill2_top, L.pill2_left,
                     L.pill2_right);
  render_history_panel(plane, theme, state, &L);
  render_analysis_panel(plane, theme, state, &L);

  render_status_bar(plane, theme, state, &L, modal);

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

static void render_modal(struct ncplane *plane, const Theme *theme,
                         const char *title, const char *const *items,
                         int item_count, int focus, int width) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  const int height = 3 + item_count;
  if ((unsigned)width >= plane_cols || (unsigned)height >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - height) / 2;
  const int left = (int)(plane_cols - width) / 2;

  // Modal lives on its own child plane that always sits on top of the
  // z-stack. Otherwise the 2x pixel composite (also a child of std)
  // sits above the modal, occluding it. Box-local coords run (0,0) to
  // (height-1, width-1); the plane itself is positioned at (top, left)
  // relative to std.
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
  // Opaque background so whatever's behind the modal box doesn't bleed
  // through. (theme.bg with default alpha = solid.)
  uint64_t base_ch = 0;
  ncchannels_set_fg_rgb8(&base_ch, theme->fg.r, theme->fg.g, theme->fg.b);
  ncchannels_set_bg_rgb8(&base_ch, theme->bg.r, theme->bg.g, theme->bg.b);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);

  theme_apply_fg(mp, theme->fg);
  theme_apply_bg(mp, theme->bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }
  draw_box(mp, theme, 0, 0, height, width, title);

  for (int i = 0; i < item_count; i++) {
    const int item_row = 2 + i;
    const bool focused = (i == focus);
    if (focused) {
      theme_apply_fg(mp, theme->bg);
      theme_apply_bg(mp, theme->accent_fg);
    } else {
      theme_apply_fg(mp, theme->fg);
      theme_apply_bg(mp, theme->bg);
    }
    char line[96];
    snprintf(line, sizeof(line), "  %-*s", width - 4, items[i]);
    ncplane_putstr_yx(mp, item_row, 1, line);
  }
}

void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[TUI_MENU_ITEM_COUNT];
  items[TUI_MENU_SETTINGS] = "Settings";
  items[TUI_MENU_QUIT] = "Quit";
  items[TUI_MENU_BACK] = "Back";
  render_modal(plane, theme, "Menu", items, TUI_MENU_ITEM_COUNT, focus, 28);
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

void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              TuiScoreSubscripts score_subscripts,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase) {
  if (plane == NULL || theme == NULL) {
    return;
  }

  // Scale row. 2x needs both pixel graphics and a loaded font; if
  // either is missing, the row reports unavailable and arrow keys
  // become no-ops at this focus.
  char scale_label[96];
  const bool scale_available = pixel_supported && font_available;
  if (!scale_available) {
    snprintf(scale_label, sizeof(scale_label), "Scale        unsupported here");
  } else {
    char value_buf[8];
    snprintf(value_buf, sizeof(value_buf), "%dx", board_scale);
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

  const char *items[TUI_SETTINGS_ITEM_COUNT];
  items[TUI_SETTINGS_SCALE] = scale_label;
  items[TUI_SETTINGS_AA] = aa_label;
  items[TUI_SETTINGS_SUBSCRIPTS] = sub_label;
  items[TUI_SETTINGS_BORDER] = border_label;
  items[TUI_SETTINGS_PREMIUM] = premium_label;
  items[TUI_SETTINGS_BLANKS] = blanks_label;
  items[TUI_SETTINGS_BACK] = "Back";
  render_modal(plane, theme, "Settings", items, TUI_SETTINGS_ITEM_COUNT, focus,
               40);
}

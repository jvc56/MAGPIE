#include "game_render.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <notcurses/notcurses.h>
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
#include "game_state.h"
#include "theme.h"
#include "tui_resize.h"

// Layout (row, col) on the std plane:
//
//   0   header bar (full width)
//   1   blank
//   2   column labels       Ａ Ｂ Ｃ ... Ｏ
//   3   top border          ┏━━━━━━━...━━┓
//   4   row 1 cells         1 ┃＝　　＇　 ...┃
//   ...
//  18   row 15 cells       15 ┃＝　　＇　 ...┃
//  19   bottom border       ┗━━━━━━━...━━┛
//
// Col 0 is left margin (kept at theme->bg as a dark surround). Each
// board cell is 2 cols wide and renders as a single fullwidth glyph:
//   premium-square mark  (＝－＂＇＊) for empty premium cells
//   ideographic space 　              for empty non-premium cells
//   fullwidth letter Ａ–Ｚ            for played tiles (LD-provided)
enum {
  HEADER_ROW = 0,
  COL_LABELS_ROW = 2,
  BORDER_TOP_ROW = 3,
  CELL_ROW_BASE = 4,
  LEFT_MARGIN = 1,
  ROW_LABEL_COL = LEFT_MARGIN,
  // Two-char row label, one-char gap, then left border.
  BORDER_LEFT_COL = LEFT_MARGIN + 3,
  CELL_COL_BASE = BORDER_LEFT_COL + 1,
  CELL_WIDTH = 2,
  BORDER_RIGHT_COL = CELL_COL_BASE + BOARD_DIM * CELL_WIDTH,
  BORDER_BOTTOM_ROW = CELL_ROW_BASE + BOARD_DIM,
  SIDEBAR_LEFT = BORDER_RIGHT_COL + 4,
  // Footer occupies BORDER_BOTTOM_ROW + 2 (info line) and + 4 (quit hint).
  MIN_ROWS = BORDER_BOTTOM_ROW + 5,
  // Sidebar's longest line is "rack  X X X X X X X X" (~30 cols).
  MIN_COLS = SIDEBAR_LEFT + 32,
};

// Fullwidth column labels Ａ..Ｚ. Each is a single fullwidth glyph that
// occupies CELL_WIDTH terminal cells, so it lines up with the cells below.
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
  const char *glyph;  // fullwidth (2-cell-wide) UTF-8 string
  ThemeRgb fg;
} PremiumMarker;

static PremiumMarker premium_marker_for_cell(const Theme *theme, BonusSquare bs,
                                             int row, int col) {
  const int center = BOARD_DIM / 2;
  if (row == center && col == center) {
    return (PremiumMarker){"\xef\xbc\x8a", theme->premium_center_bg};  // ＊
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bs);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bs);
  if (word_mult == 3) {
    return (PremiumMarker){"\xef\xbc\x9d", theme->premium_tws_bg};  // ＝
  }
  if (word_mult == 2) {
    return (PremiumMarker){"\xef\xbc\x8d", theme->premium_dws_bg};  // －
  }
  if (letter_mult == 3) {
    return (PremiumMarker){"\xef\xbc\x82", theme->premium_tls_bg};  // ＂
  }
  if (letter_mult == 2) {
    return (PremiumMarker){"\xef\xbc\x87", theme->premium_dls_bg};  // ＇
  }
  return (PremiumMarker){"\xe3\x80\x80", theme->dim_fg};  // 　 ideographic space
}

static bool plane_can_fit_board(struct ncplane *plane) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  return plane_rows >= MIN_ROWS && plane_cols >= MIN_COLS;
}

static void render_header(struct ncplane *plane, const Theme *theme) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  if (plane_rows == 0 || plane_cols == 0) {
    return;
  }
  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < plane_cols; col++) {
    ncplane_putstr_yx(plane, HEADER_ROW, (int)col, " ");
  }
  ncplane_putstr_yx(plane, HEADER_ROW, 2, " MAGPIE TUI ");
}

// Fill the rectangle that holds the column labels, row labels, border, and
// cells with a single uniform background. Without this, the labels and the
// gap col between them and the border would inherit theme->bg, leaving a
// visible seam against the board's board_bg. Col 0 is intentionally left
// at theme->bg as a darker margin.
static void render_board_area_bg(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->board_bg);
  for (int row = COL_LABELS_ROW; row <= BORDER_BOTTOM_ROW; row++) {
    for (int col = LEFT_MARGIN; col <= BORDER_RIGHT_COL; col++) {
      ncplane_putstr_yx(plane, row, col, " ");
    }
  }
}

static void render_col_labels(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->board_bg);
  for (int col = 0; col < BOARD_DIM; col++) {
    ncplane_putstr_yx(plane, COL_LABELS_ROW,
                      CELL_COL_BASE + col * CELL_WIDTH,
                      fullwidth_col_labels[col]);
  }
}

static void render_row_label(struct ncplane *plane, const Theme *theme,
                             int row) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->board_bg);
  char label[4];
  snprintf(label, sizeof(label), "%2d", row + 1);
  ncplane_putstr_yx(plane, CELL_ROW_BASE + row, ROW_LABEL_COL, label);
}

static void render_border(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->board_bg);

  // Top border.
  ncplane_putstr_yx(plane, BORDER_TOP_ROW, BORDER_LEFT_COL, "\xe2\x94\x8f");
  for (int col = 0; col < BOARD_DIM * CELL_WIDTH; col++) {
    ncplane_putstr_yx(plane, BORDER_TOP_ROW, CELL_COL_BASE + col,
                      "\xe2\x94\x81");
  }
  ncplane_putstr_yx(plane, BORDER_TOP_ROW, BORDER_RIGHT_COL, "\xe2\x94\x93");

  // Bottom border.
  ncplane_putstr_yx(plane, BORDER_BOTTOM_ROW, BORDER_LEFT_COL,
                    "\xe2\x94\x97");
  for (int col = 0; col < BOARD_DIM * CELL_WIDTH; col++) {
    ncplane_putstr_yx(plane, BORDER_BOTTOM_ROW, CELL_COL_BASE + col,
                      "\xe2\x94\x81");
  }
  ncplane_putstr_yx(plane, BORDER_BOTTOM_ROW, BORDER_RIGHT_COL,
                    "\xe2\x94\x9b");

  // Side borders.
  for (int row = 0; row < BOARD_DIM; row++) {
    ncplane_putstr_yx(plane, CELL_ROW_BASE + row, BORDER_LEFT_COL,
                      "\xe2\x94\x83");
    ncplane_putstr_yx(plane, CELL_ROW_BASE + row, BORDER_RIGHT_COL,
                      "\xe2\x94\x83");
  }
}

static void render_cell(struct ncplane *plane, const Theme *theme,
                        const Board *board, const LetterDistribution *ld,
                        int row, int col) {
  const int screen_row = CELL_ROW_BASE + row;
  const int screen_col = CELL_COL_BASE + col * CELL_WIDTH;

  const MachineLetter ml = board_get_letter(board, row, col);
  const BonusSquare bs = board_get_bonus_square(board, row, col);

  if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
    const PremiumMarker marker = premium_marker_for_cell(theme, bs, row, col);
    theme_apply_fg(plane, marker.fg);
    theme_apply_bg(plane, theme->board_bg);
    ncplane_putstr_yx(plane, screen_row, screen_col, marker.glyph);
    return;
  }

  theme_apply_fg(plane, theme->tile_fg);
  theme_apply_bg(plane, theme->tile_bg);
  // LDs without a fullwidth column (catalan, german, polish) leave
  // ld_ml_to_alt_hl empty; fall back to a space + ASCII letter so the
  // cell still fills its 2-cell width.
  const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
  if (fullwidth[0] != '\0') {
    ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
  } else {
    ncplane_putstr_yx(plane, screen_row, screen_col, " ");
    ncplane_putstr(plane, ld->ld_ml_to_hl[ml]);
  }
}

static void render_board(struct ncplane *plane, const Theme *theme,
                         const TuiGameState *state) {
  render_board_area_bg(plane, theme);
  render_col_labels(plane, theme);
  render_border(plane, theme);
  const Board *board = game_get_board(state->game);
  for (int row = 0; row < BOARD_DIM; row++) {
    render_row_label(plane, theme, row);
    for (int col = 0; col < BOARD_DIM; col++) {
      render_cell(plane, theme, board, state->ld, row, col);
    }
  }
}

static void render_rack(struct ncplane *plane, const Theme *theme,
                        const LetterDistribution *ld, const Rack *rack,
                        int screen_row, int screen_col) {
  int col_offset = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count; copy++) {
      const char *letter_str = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
      theme_apply_fg(plane, theme->rack_tile_fg);
      theme_apply_bg(plane, theme->rack_tile_bg);
      ncplane_putstr_yx(plane, screen_row, screen_col + col_offset, " ");
      ncplane_putstr(plane, letter_str);
      ncplane_putstr(plane, " ");
      col_offset += 3;
      theme_apply_bg(plane, theme->bg);
      ncplane_putstr_yx(plane, screen_row, screen_col + col_offset, " ");
      col_offset += 1;
    }
  }
}

static void render_player_panel(struct ncplane *plane, const Theme *theme,
                                const TuiGameState *state, int player_index,
                                int screen_row) {
  const Player *player = game_get_player(state->game, player_index);
  const bool on_turn =
      game_get_player_on_turn_index(state->game) == player_index;

  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  char label[32];
  snprintf(label, sizeof(label), "%s Player %d",
           on_turn ? "\xe2\x96\xb6" : " ", player_index + 1);
  ncplane_putstr_yx(plane, screen_row, SIDEBAR_LEFT, label);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, screen_row + 1, SIDEBAR_LEFT, "score ");
  theme_apply_fg(plane, theme->fg);
  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           equity_to_int(player_get_score(player)));
  ncplane_putstr(plane, score_str);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, screen_row + 2, SIDEBAR_LEFT, "rack  ");
  render_rack(plane, theme, state->ld, player_get_rack(player),
              screen_row + 2, SIDEBAR_LEFT + 6);
}

static void format_clock(int seconds, char *buf, size_t buf_size) {
  const int minutes = seconds / 60;
  const int secs = seconds % 60;
  snprintf(buf, buf_size, "%d:%02d", minutes, secs);
}

static void render_footer(struct ncplane *plane, const Theme *theme,
                          const TuiGameState *state, int time_per_side_seconds,
                          int row) {
  const int bag_left = bag_get_letters(game_get_bag(state->game));

  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, row, BORDER_LEFT_COL, "bag ");
  theme_apply_fg(plane, theme->fg);
  char bag_str[16];
  snprintf(bag_str, sizeof(bag_str), "%d", bag_left);
  ncplane_putstr(plane, bag_str);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "   \xc2\xb7   ");
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr(plane, state->lexicon);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "   \xc2\xb7   ");
  char clock[16];
  format_clock(time_per_side_seconds, clock, sizeof(clock));
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr(plane, clock);
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, " per side");

  ncplane_putstr_yx(plane, row + 2, BORDER_LEFT_COL,
                    "(q or Esc to quit)");
}

static void render_too_small(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 0, 0, "Terminal too small. Resize.");
}

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds) {
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }

  tui_sync_plane_to_terminal(plane);

  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  if (!plane_can_fit_board(plane)) {
    render_too_small(plane, theme);
    return;
  }

  render_header(plane, theme);
  render_board(plane, theme, state);

  render_player_panel(plane, theme, state, 0, CELL_ROW_BASE);
  render_player_panel(plane, theme, state, 1, CELL_ROW_BASE + 5);

  render_footer(plane, theme, state, time_per_side_seconds,
                BORDER_BOTTOM_ROW + 2);
}

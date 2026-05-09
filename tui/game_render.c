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

enum {
  BOARD_TOP_ROW = 3,
  BOARD_LEFT_COL = 4,  // 2 chars row label + 1 space
  CELL_WIDTH = 2,
  SIDEBAR_LEFT = 4 + BOARD_DIM * 2 + 4,
};

static ThemeRgb premium_bg_for_cell(const Theme *theme, BonusSquare bs, int row,
                                    int col) {
  const int center = BOARD_DIM / 2;
  if (row == center && col == center) {
    return theme->premium_center_bg;
  }
  const uint8_t word_mult = bonus_square_get_word_multiplier(bs);
  const uint8_t letter_mult = bonus_square_get_letter_multiplier(bs);
  if (word_mult == 3) {
    return theme->premium_tws_bg;
  }
  if (word_mult == 2) {
    return theme->premium_dws_bg;
  }
  if (letter_mult == 3) {
    return theme->premium_tls_bg;
  }
  if (letter_mult == 2) {
    return theme->premium_dls_bg;
  }
  return theme->board_bg;
}

static void render_header(struct ncplane *plane, const Theme *theme) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < plane_cols; col++) {
    ncplane_putstr_yx(plane, 0, (int)col, " ");
  }
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI ");
}

static void render_col_labels(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  for (int col = 0; col < BOARD_DIM; col++) {
    char label[2] = {(char)('A' + col), '\0'};
    ncplane_putstr_yx(plane, BOARD_TOP_ROW - 1,
                      BOARD_LEFT_COL + col * CELL_WIDTH, label);
  }
}

static void render_row_label(struct ncplane *plane, const Theme *theme,
                             int row) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  char label[4];
  snprintf(label, sizeof(label), "%2d", row + 1);
  ncplane_putstr_yx(plane, BOARD_TOP_ROW + row, 1, label);
}

static void render_cell(struct ncplane *plane, const Theme *theme,
                        const Board *board, const LetterDistribution *ld,
                        int row, int col) {
  const int screen_row = BOARD_TOP_ROW + row;
  const int screen_col = BOARD_LEFT_COL + col * CELL_WIDTH;

  const MachineLetter ml = board_get_letter(board, row, col);
  const BonusSquare bs = board_get_bonus_square(board, row, col);

  if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
    const ThemeRgb bg = premium_bg_for_cell(theme, bs, row, col);
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, bg);
    ncplane_putstr_yx(plane, screen_row, screen_col, "  ");
    return;
  }

  // Played tile.
  theme_apply_fg(plane, theme->tile_fg);
  theme_apply_bg(plane, theme->tile_bg);
  const char *letter_str = ld->ld_ml_to_hl[ml];
  // Render as " X" (right-aligned within the 2-char cell) for visual room.
  ncplane_putstr_yx(plane, screen_row, screen_col, " ");
  ncplane_putstr(plane, letter_str);
}

static void render_board(struct ncplane *plane, const Theme *theme,
                         const TuiGameState *state) {
  render_col_labels(plane, theme);
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
  // Iterate ml in score order? In the engine, ml=0 is BLANK, ml=1.. are
  // language letters in the order they appear in the LD file (typically
  // alphabetical for Latin scripts). Iterating 0..size shows blanks first,
  // then alphabetical, which is the conventional rack-display order.
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
      // Gap between tiles.
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
           on_turn ? "▶" : " ", player_index + 1);
  ncplane_putstr_yx(plane, screen_row, SIDEBAR_LEFT, label);

  // Score on the next line.
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, screen_row + 1, SIDEBAR_LEFT, "score ");
  theme_apply_fg(plane, theme->fg);
  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           equity_to_int(player_get_score(player)));
  ncplane_putstr(plane, score_str);

  // Rack on the line after.
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
  ncplane_putstr_yx(plane, row, BOARD_LEFT_COL, "bag ");
  theme_apply_fg(plane, theme->fg);
  char bag_str[16];
  snprintf(bag_str, sizeof(bag_str), "%d", bag_left);
  ncplane_putstr(plane, bag_str);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "   ·   ");
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr(plane, state->lexicon);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "   ·   ");
  char clock[16];
  format_clock(time_per_side_seconds, clock, sizeof(clock));
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr(plane, clock);
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, " per side");

  ncplane_putstr_yx(plane, row + 2, BOARD_LEFT_COL,
                    "(q or Esc to quit)");
}

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds) {
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  render_header(plane, theme);
  render_board(plane, theme, state);

  render_player_panel(plane, theme, state, 0, BOARD_TOP_ROW);
  render_player_panel(plane, theme, state, 1, BOARD_TOP_ROW + 5);

  render_footer(plane, theme, state, time_per_side_seconds,
                BOARD_TOP_ROW + BOARD_DIM + 1);
}

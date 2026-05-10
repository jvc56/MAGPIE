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

// Layout overview:
//
//   left column (cols 0..BOARD_RIGHT_COL):
//     row 0:                column labels   Ａ Ｂ Ｃ ... Ｏ
//     rows 1..15:           board cells (with 2-char "%2d" row label at col 0)
//     row 17..19:           rack panel (┌─┐ box, fullwidth tiles inside)
//     rows 20..23:          bag panel (┌─┐ box)
//
//   right column (cols RIGHT_COL_LEFT..RIGHT_COL_RIGHT):
//     rows 0..2:            Player 1 pill
//     rows 3..5:            Player 2 pill
//     rows 6..23:           history panel
//
// Board cells are 2 cols wide, fullwidth UTF-8 glyphs:
//   empty premium  =  fullwidth bonus mark    (＝－＂＇＊)
//   empty cell     =  ideographic space       (　)
//   played tile    =  fullwidth letter        (Ａ-Ｚ from LD)
//
// Column labels are fullwidth Ａ-Ｏ to line up with the cells. Row labels
// stay halfwidth ("%2d"). Rack tiles are fullwidth letters on tile_bg, no
// gap. If racks ever appear inline in history, render them halfwidth.

enum {
  // Board geometry.
  COL_LABELS_ROW = 0,
  CELL_ROW_BASE = 1,
  CELL_BOTTOM_ROW = CELL_ROW_BASE + BOARD_DIM - 1,
  CELL_WIDTH = 2,
  ROW_LABEL_COL = 0,
  // 2-char row label, 1-col gap, then cells.
  CELL_COL_BASE = ROW_LABEL_COL + 3,
  CELL_RIGHT_COL = CELL_COL_BASE + BOARD_DIM * CELL_WIDTH - 1,
  BOARD_RIGHT_COL = CELL_RIGHT_COL,
  BOARD_WIDTH = BOARD_RIGHT_COL + 1,

  // Rack panel sits below the board with one row of gap.
  RACK_TOP_ROW = CELL_BOTTOM_ROW + 2,
  RACK_HEIGHT = 3,
  RACK_BOTTOM_ROW = RACK_TOP_ROW + RACK_HEIGHT - 1,

  // Bag panel sits below rack.
  BAG_TOP_ROW = RACK_BOTTOM_ROW + 1,
  BAG_HEIGHT = 4,
  BAG_BOTTOM_ROW = BAG_TOP_ROW + BAG_HEIGHT - 1,

  // Right column: two stacked player pills, then history.
  RIGHT_COL_LEFT = BOARD_RIGHT_COL + 3,
  RIGHT_COL_WIDTH = 44,
  RIGHT_COL_RIGHT = RIGHT_COL_LEFT + RIGHT_COL_WIDTH - 1,
  PILL_HEIGHT = 3,
  PILL1_TOP_ROW = 0,
  PILL2_TOP_ROW = PILL1_TOP_ROW + PILL_HEIGHT,
  HISTORY_TOP_ROW = PILL2_TOP_ROW + PILL_HEIGHT,
  HISTORY_BOTTOM_ROW = BAG_BOTTOM_ROW,

  MIN_ROWS = BAG_BOTTOM_ROW + 1,
  MIN_COLS = RIGHT_COL_RIGHT + 1,
};

// Light box-drawing characters (UTF-8).
#define BOX_TL "\xe2\x94\x8c"  // ┌
#define BOX_TR "\xe2\x94\x90"  // ┐
#define BOX_BL "\xe2\x94\x94"  // └
#define BOX_BR "\xe2\x94\x98"  // ┘
#define BOX_HZ "\xe2\x94\x80"  // ─
#define BOX_VT "\xe2\x94\x82"  // │
#define BOX_LT "\xe2\x94\x9c"  // ├
#define BOX_RT "\xe2\x94\xa4"  // ┤

// Fullwidth column labels Ａ..Ｚ — one fullwidth glyph per CELL_WIDTH.
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
  const char *glyph;  // fullwidth UTF-8 (2-cell-wide)
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
  return (PremiumMarker){"\xe3\x80\x80", theme->dim_fg};  //
}

static bool plane_can_fit(struct ncplane *plane) {
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  return plane_rows >= MIN_ROWS && plane_cols >= MIN_COLS;
}

static void render_too_small(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 0, 0, "Terminal too small. Resize.");
}

// ── Generic light-box border ──────────────────────────────────────────────
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

// ── Board ─────────────────────────────────────────────────────────────────
static void render_col_labels(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  for (int col = 0; col < BOARD_DIM; col++) {
    ncplane_putstr_yx(plane, COL_LABELS_ROW,
                      CELL_COL_BASE + col * CELL_WIDTH,
                      fullwidth_col_labels[col]);
  }
}

static void render_row_label(struct ncplane *plane, const Theme *theme,
                             int row) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  char label[4];
  snprintf(label, sizeof(label), "%2d", row + 1);
  ncplane_putstr_yx(plane, CELL_ROW_BASE + row, ROW_LABEL_COL, label);
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
  // ld_ml_to_alt_hl empty; fall back to a space + ASCII letter.
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
  render_col_labels(plane, theme);
  const Board *board = game_get_board(state->game);
  for (int row = 0; row < BOARD_DIM; row++) {
    render_row_label(plane, theme, row);
    for (int col = 0; col < BOARD_DIM; col++) {
      render_cell(plane, theme, board, state->ld, row, col);
    }
  }
}

// ── Rack panel ────────────────────────────────────────────────────────────
static void render_rack_tiles(struct ncplane *plane, const Theme *theme,
                              const LetterDistribution *ld, const Rack *rack,
                              int screen_row, int screen_col) {
  // Each tile is one fullwidth glyph (2 cells), no gap. Iterate ml in LD
  // order so tiles appear roughly alphabetical with blanks first.
  int col_offset = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count; copy++) {
      const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
      theme_apply_fg(plane, theme->rack_tile_fg);
      theme_apply_bg(plane, theme->rack_tile_bg);
      if (fullwidth[0] != '\0') {
        ncplane_putstr_yx(plane, screen_row, screen_col + col_offset,
                          fullwidth);
      } else {
        // No fullwidth available (catalan, german, polish): pad with
        // ASCII letter centered in 2-cell width.
        const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
        ncplane_putstr_yx(plane, screen_row, screen_col + col_offset, " ");
        ncplane_putstr(plane, ascii);
      }
      col_offset += 2;
    }
  }
}

static void render_rack_panel(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state) {
  const int player_idx = game_get_player_on_turn_index(state->game);
  const Player *player = game_get_player(state->game, player_idx);
  const Rack *rack = player_get_rack(player);

  draw_box(plane, theme, RACK_TOP_ROW, 0, RACK_HEIGHT, BOARD_WIDTH, "Rack");

  // Center the rack within the panel interior.
  const int interior_width = BOARD_WIDTH - 2;
  const int total_letters = rack_get_total_letters(rack);
  const int rack_pixels = total_letters * 2;
  int start_col = 1 + (interior_width - rack_pixels) / 2;
  if (start_col < 1) {
    start_col = 1;
  }
  render_rack_tiles(plane, theme, state->ld, rack, RACK_TOP_ROW + 1,
                    start_col);
}

// ── Bag panel ─────────────────────────────────────────────────────────────
static void format_clock(int seconds, char *buf, size_t buf_size) {
  if (seconds < 0) {
    snprintf(buf, buf_size, "--:--");
    return;
  }
  const int minutes = seconds / 60;
  const int secs = seconds % 60;
  snprintf(buf, buf_size, "%d:%02d", minutes, secs);
}

static void render_bag_panel(struct ncplane *plane, const Theme *theme,
                             const TuiGameState *state) {
  const Bag *bag = game_get_bag(state->game);
  const int bag_count = bag_get_letters(bag);
  char title[32];
  snprintf(title, sizeof(title), "Bag (%d)", bag_count);

  draw_box(plane, theme, BAG_TOP_ROW, 0, BAG_HEIGHT, BOARD_WIDTH, title);

  // Render unseen letters: count each letter remaining in the bag plus on
  // the off-turn player's rack (the on-turn rack is "seen" by us). Walk the
  // bag's tile array directly via bag_get_letter helpers.
  const LetterDistribution *ld = state->ld;
  const int ld_size = ld_get_size(ld);

  // Tally bag + off-turn rack into a per-ml count array.
  int counts[64] = {0};
  for (int ml = 0; ml < ld_size && ml < (int)(sizeof(counts) / sizeof(int));
       ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  const int off_turn = 1 - game_get_player_on_turn_index(state->game);
  const Rack *off_rack = player_get_rack(game_get_player(state->game, off_turn));
  for (int ml = 0; ml < ld_size && ml < (int)(sizeof(counts) / sizeof(int));
       ml++) {
    counts[ml] += rack_get_letter(off_rack, (MachineLetter)ml);
  }

  // Build the dense inline string: "?? AAAAAAA BB ..." with two spaces
  // between letter groups.
  char line[256];
  size_t pos = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (counts[ml] == 0) {
      continue;
    }
    const char *letter = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
    if (pos > 0 && pos + 1 < sizeof(line)) {
      line[pos++] = ' ';
    }
    for (int i = 0; i < counts[ml] && pos + 2 < sizeof(line); i++) {
      const size_t letter_len = strlen(letter);
      if (pos + letter_len < sizeof(line)) {
        memcpy(line + pos, letter, letter_len);
        pos += letter_len;
      }
    }
  }
  line[pos < sizeof(line) ? pos : sizeof(line) - 1] = '\0';

  // Render the inline string, wrapping at the panel interior width.
  const int interior_left = 2;
  const int interior_width = BOARD_WIDTH - 4;
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  int line_row = BAG_TOP_ROW + 1;
  size_t i = 0;
  while (i < pos && line_row < BAG_BOTTOM_ROW) {
    char chunk[256];
    size_t chunk_len = 0;
    // Take up to interior_width chars. Try to break at a space.
    size_t limit = i + interior_width;
    if (limit >= pos) {
      limit = pos;
    } else {
      size_t back = limit;
      while (back > i && line[back] != ' ') {
        back--;
      }
      if (back > i) {
        limit = back;
      }
    }
    chunk_len = limit - i;
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
  for (int ml = 1; ml < ld_size; ml++) {  // skip blank
    if (ld->is_vowel[ml]) {
      vowels += counts[ml];
    } else {
      consonants += counts[ml];
    }
  }
  char tally[64];
  snprintf(tally, sizeof(tally), "%d vowels \xc2\xb7 %d consonants", vowels,
           consonants);
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, BAG_BOTTOM_ROW - 1, interior_left, tally);
}

// ── Player pill ───────────────────────────────────────────────────────────
static void render_player_pill(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, int player_idx,
                               int top_row, int time_per_side_seconds) {
  const Player *player = game_get_player(state->game, player_idx);
  const bool on_turn = game_get_player_on_turn_index(state->game) == player_idx;

  draw_box(plane, theme, top_row, RIGHT_COL_LEFT, PILL_HEIGHT, RIGHT_COL_WIDTH,
           NULL);

  // On-turn marker + name on the content row.
  const int content_row = top_row + 1;
  const int content_left = RIGHT_COL_LEFT + 2;
  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, content_row, content_left,
                    on_turn ? "\xe2\x96\xb6 " : "  ");
  theme_apply_fg(plane, theme->fg);
  char name[32];
  snprintf(name, sizeof(name), "Player %d", player_idx + 1);
  ncplane_putstr(plane, name);

  // Score, right-of-name; clock, right-aligned.
  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           equity_to_int(player_get_score(player)));
  char clock_str[16];
  format_clock(time_per_side_seconds, clock_str, sizeof(clock_str));

  const int right = RIGHT_COL_LEFT + RIGHT_COL_WIDTH - 2;
  const int clock_len = (int)strlen(clock_str);
  const int clock_col = right - clock_len + 1;
  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  ncplane_putstr_yx(plane, content_row, clock_col, clock_str);

  const int score_len = (int)strlen(score_str);
  const int score_col = clock_col - 4 - score_len;
  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, content_row, score_col, score_str);
}

// ── History panel ─────────────────────────────────────────────────────────
static void render_history_panel(struct ncplane *plane, const Theme *theme) {
  const int top = HISTORY_TOP_ROW;
  const int height = HISTORY_BOTTOM_ROW - HISTORY_TOP_ROW + 1;
  draw_box(plane, theme, top, RIGHT_COL_LEFT, height, RIGHT_COL_WIDTH,
           "History");

  // Empty placeholder for now — bot worker will populate this later.
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  const int center_row = top + height / 2;
  const int interior_width = RIGHT_COL_WIDTH - 2;
  const char *msg = "No moves yet.";
  const int msg_col =
      RIGHT_COL_LEFT + 1 + (interior_width - (int)strlen(msg)) / 2;
  ncplane_putstr_yx(plane, center_row, msg_col, msg);
}

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds) {
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }

  tui_sync_plane_to_terminal(plane);

  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  if (!plane_can_fit(plane)) {
    render_too_small(plane, theme);
    return;
  }

  render_board(plane, theme, state);
  render_rack_panel(plane, theme, state);
  render_bag_panel(plane, theme, state);

  render_player_pill(plane, theme, state, 0, PILL1_TOP_ROW,
                     time_per_side_seconds);
  render_player_pill(plane, theme, state, 1, PILL2_TOP_ROW,
                     time_per_side_seconds);
  render_history_panel(plane, theme);
}

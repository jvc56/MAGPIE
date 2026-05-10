#include "game_render.h"

#include <ctype.h>
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
#include "../src/util/string_util.h"
#include "game_state.h"
#include "theme.h"
#include "tui_resize.h"

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
// Board cells are 2 cols wide, fullwidth UTF-8 glyphs (＝－＂＇＊ for premium
// squares, fullwidth Ａ-Ｚ from LD for tiles, ideographic space for empty
// cells). Column labels are fullwidth Ａ-Ｏ. Rack tiles are fullwidth
// letters on tile_bg, no gap. Everything else is halfwidth ASCII; if a rack
// ever appears inline in a history row, render that one halfwidth too.

enum {
  CELL_WIDTH = 2,
  ROW_LABEL_COL = 0,
  CELL_COL_BASE = ROW_LABEL_COL + 3,
  BOARD_WIDTH = CELL_COL_BASE + BOARD_DIM * CELL_WIDTH,  // = 33
  COL_LABELS_ROW = 0,
  CELL_ROW_BASE = 1,
  CELL_BOTTOM_ROW = CELL_ROW_BASE + BOARD_DIM - 1,  // = 15
  RACK_HEIGHT = 3,
  PILL_HEIGHT = 3,
  RIGHT_COL_LEFT_OFFSET = 3,    // gap from board's right edge
  RIGHT_COL_MIN_WIDTH = 32,     // narrowest the right column can be
  STATUS_BAR_HEIGHT = 1,
  // Minimum: pills (6) + history (4) covers right; board (16) + rack (3) +
  // bag (4) = 23 + status (1) covers left. Take the larger.
  MIN_ROWS_REQUIRED = 24,
  MIN_COLS_REQUIRED = BOARD_WIDTH + RIGHT_COL_LEFT_OFFSET + RIGHT_COL_MIN_WIDTH,
};

typedef struct {
  unsigned plane_rows;
  unsigned plane_cols;

  // Status bar at the very bottom of the plane.
  int status_row;

  // Left column.
  int board_right_col;  // = BOARD_WIDTH - 1
  int rack_top, rack_bottom;
  int bag_top, bag_bottom;

  // Right column.
  int right_col_left, right_col_right;
  int pill1_top, pill1_bottom;
  int pill2_top, pill2_bottom;
  int history_top, history_bottom;
} Layout;

static Layout compute_layout(struct ncplane *plane) {
  Layout L = {0};
  ncplane_dim_yx(plane, &L.plane_rows, &L.plane_cols);

  L.status_row = (int)L.plane_rows - 1;

  L.board_right_col = BOARD_WIDTH - 1;

  L.rack_top = CELL_BOTTOM_ROW + 2;  // = 17 (1-row gap below board)
  L.rack_bottom = L.rack_top + RACK_HEIGHT - 1;  // = 19

  L.bag_top = L.rack_bottom + 1;  // = 20
  L.bag_bottom = L.status_row - 1;  // expands to fill

  L.right_col_left = BOARD_WIDTH + RIGHT_COL_LEFT_OFFSET;
  L.right_col_right = (int)L.plane_cols - 1;

  L.pill1_top = 0;
  L.pill1_bottom = PILL_HEIGHT - 1;  // 2
  L.pill2_top = L.pill1_bottom + 1;  // 3
  L.pill2_bottom = L.pill2_top + PILL_HEIGHT - 1;  // 5

  L.history_top = L.pill2_bottom + 1;  // 6
  L.history_bottom = L.status_row - 1;  // expands to fill

  return L;
}

// ── Box drawing chars ─────────────────────────────────────────────────────
#define BOX_TL "\xe2\x94\x8c"  // ┌
#define BOX_TR "\xe2\x94\x90"  // ┐
#define BOX_BL "\xe2\x94\x94"  // └
#define BOX_BR "\xe2\x94\x98"  // ┘
#define BOX_HZ "\xe2\x94\x80"  // ─
#define BOX_VT "\xe2\x94\x82"  // │

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
  const char *glyph;
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

// ── Board ─────────────────────────────────────────────────────────────────
static void render_board(struct ncplane *plane, const Theme *theme,
                         const TuiGameState *state) {
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  for (int col = 0; col < BOARD_DIM; col++) {
    ncplane_putstr_yx(plane, COL_LABELS_ROW,
                      CELL_COL_BASE + col * CELL_WIDTH,
                      fullwidth_col_labels[col]);
  }

  const Board *board = game_get_board(state->game);
  for (int row = 0; row < BOARD_DIM; row++) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    char label[4];
    snprintf(label, sizeof(label), "%2d", row + 1);
    ncplane_putstr_yx(plane, CELL_ROW_BASE + row, ROW_LABEL_COL, label);

    for (int col = 0; col < BOARD_DIM; col++) {
      const int screen_row = CELL_ROW_BASE + row;
      const int screen_col = CELL_COL_BASE + col * CELL_WIDTH;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker =
            premium_marker_for_cell(theme, bs, row, col);
        theme_apply_fg(plane, marker.fg);
        theme_apply_bg(plane, theme->board_bg);
        ncplane_putstr_yx(plane, screen_row, screen_col, marker.glyph);
        continue;
      }
      theme_apply_fg(plane, theme->tile_fg);
      theme_apply_bg(plane, theme->tile_bg);
      const char *fullwidth = state->ld->ld_ml_to_alt_hl[ml];
      if (fullwidth[0] != '\0') {
        ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
      } else {
        ncplane_putstr_yx(plane, screen_row, screen_col, " ");
        ncplane_putstr(plane, state->ld->ld_ml_to_hl[ml]);
      }
    }
  }
}

// ── Rack panel ────────────────────────────────────────────────────────────
static void render_rack_panel(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L) {
  draw_box(plane, theme, L->rack_top, 0, RACK_HEIGHT, BOARD_WIDTH, "Rack");

  const int player_idx = game_get_player_on_turn_index(state->game);
  const Player *player = game_get_player(state->game, player_idx);
  const Rack *rack = player_get_rack(player);
  const LetterDistribution *ld = state->ld;

  const int interior_width = BOARD_WIDTH - 2;
  const int total_letters = rack_get_total_letters(rack);
  const int rack_width = total_letters * CELL_WIDTH;
  int start_col = 1 + (interior_width - rack_width) / 2;
  if (start_col < 1) {
    start_col = 1;
  }

  int col_offset = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy = 0; copy < count; copy++) {
      const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
      theme_apply_fg(plane, theme->rack_tile_fg);
      theme_apply_bg(plane, theme->rack_tile_bg);
      if (fullwidth[0] != '\0') {
        ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                          fullwidth);
      } else {
        const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
        ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset, " ");
        ncplane_putstr(plane, ascii);
      }
      col_offset += CELL_WIDTH;
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
  draw_box(plane, theme, L->bag_top, 0, height, BOARD_WIDTH, title);

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
  // the vowel/consonant tally).
  const int interior_left = 2;
  const int interior_width = BOARD_WIDTH - 4;
  const int content_top = L->bag_top + 1;
  const int content_bottom = L->bag_bottom - 2;  // last row before tally
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
  char tally[64];
  snprintf(tally, sizeof(tally), "%d vowels \xc2\xb7 %d consonants", vowels,
           consonants);
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, L->bag_bottom - 1, interior_left, tally);
}

// ── Player pill ───────────────────────────────────────────────────────────
static void render_player_pill(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, int player_idx,
                               int top_row, int time_per_side_seconds,
                               const Layout *L) {
  const int width = L->right_col_right - L->right_col_left + 1;
  draw_box(plane, theme, top_row, L->right_col_left, PILL_HEIGHT, width, NULL);

  const Player *player = game_get_player(state->game, player_idx);
  const bool on_turn = game_get_player_on_turn_index(state->game) == player_idx;
  const int content_row = top_row + 1;
  const int content_left = L->right_col_left + 2;

  theme_apply_fg(plane, on_turn ? theme->accent_fg : theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, content_row, content_left,
                    on_turn ? "\xe2\x96\xb6 " : "  ");
  theme_apply_fg(plane, theme->fg);
  char name[32];
  snprintf(name, sizeof(name), "Player %d", player_idx + 1);
  ncplane_putstr(plane, name);

  char score_str[16];
  snprintf(score_str, sizeof(score_str), "%d",
           equity_to_int(player_get_score(player)));
  char clock_str[16];
  format_clock(time_per_side_seconds, clock_str, sizeof(clock_str));

  const int right = L->right_col_right - 1;
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
static void render_history_panel(struct ncplane *plane, const Theme *theme,
                                 const Layout *L) {
  const int width = L->right_col_right - L->right_col_left + 1;
  const int height = L->history_bottom - L->history_top + 1;
  if (height < 3) {
    return;
  }
  draw_box(plane, theme, L->history_top, L->right_col_left, height, width,
           "History");

  // Empty placeholder until the bot worker lands.
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  const int center_row = L->history_top + height / 2;
  const int interior_width = width - 2;
  const char *msg = "No moves yet.";
  const int msg_col =
      L->right_col_left + 1 + (interior_width - (int)strlen(msg)) / 2;
  ncplane_putstr_yx(plane, center_row, msg_col, msg);
}

// ── Status bar ────────────────────────────────────────────────────────────
static void render_status_bar(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L) {
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

  // Left side: "Language · Lexicon"
  char left_buf[128];
  snprintf(left_buf, sizeof(left_buf), " %s \xc2\xb7 %s",
           language_for_lexicon(state->lexicon), state->lexicon);
  ncplane_putstr_yx(plane, row, 0, left_buf);

  // Right side: shortcuts.
  const char *right_buf = "q/Esc quit ";
  const int right_len = (int)strlen(right_buf);
  const int right_col = (int)L->plane_cols - right_len;
  if (right_col > 0) {
    ncplane_putstr_yx(plane, row, right_col, right_buf);
  }
}

// ── Top-level render ──────────────────────────────────────────────────────
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

  const Layout L = compute_layout(plane);
  if (L.plane_rows < MIN_ROWS_REQUIRED || L.plane_cols < MIN_COLS_REQUIRED) {
    render_too_small(plane, theme);
    return;
  }

  render_board(plane, theme, state);
  render_rack_panel(plane, theme, state, &L);
  render_bag_panel(plane, theme, state, &L);

  render_player_pill(plane, theme, state, 0, L.pill1_top,
                     time_per_side_seconds, &L);
  render_player_pill(plane, theme, state, 1, L.pill2_top,
                     time_per_side_seconds, &L);
  render_history_panel(plane, theme, &L);

  render_status_bar(plane, theme, state, &L);
}

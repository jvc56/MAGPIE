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
  RIGHT_COL_LEFT_OFFSET = 1,    // gap from board's right edge
  RIGHT_COL_MIN_WIDTH = 32,     // narrowest the right column can be
  // History switches to two columns once the panel is at least this wide.
  // At 68 cols the panel splits into two ~33-col halves, which is just
  // wide enough to fit a fullwidth 7-tile rack inside each player pill
  // alongside the name, score, and clock.
  HISTORY_TWO_COL_THRESHOLD = 68,
  STATUS_BAR_HEIGHT = 1,
  MIN_ROWS_REQUIRED = 24,
  MIN_COLS_REQUIRED = BOARD_WIDTH + RIGHT_COL_LEFT_OFFSET + RIGHT_COL_MIN_WIDTH,
};

typedef struct {
  unsigned plane_rows;
  unsigned plane_cols;

  int status_row;

  int board_right_col;  // = BOARD_WIDTH - 1
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
} Layout;

static Layout compute_layout(struct ncplane *plane) {
  Layout L = {0};
  ncplane_dim_yx(plane, &L.plane_rows, &L.plane_cols);

  L.status_row = (int)L.plane_rows - 1;

  L.board_right_col = BOARD_WIDTH - 1;
  L.rack_top = CELL_BOTTOM_ROW + 2;
  L.rack_bottom = L.rack_top + RACK_HEIGHT - 1;
  L.bag_top = L.rack_bottom + 1;
  L.bag_bottom = L.status_row - 1;

  L.right_col_left = BOARD_WIDTH + RIGHT_COL_LEFT_OFFSET;
  L.right_col_right = (int)L.plane_cols - 1;
  L.right_col_width = L.right_col_right - L.right_col_left + 1;
  L.two_col = L.right_col_width >= HISTORY_TWO_COL_THRESHOLD;

  if (L.two_col) {
    // Pills act as column headers — one above each history subcolumn.
    const int gutter = 2;
    const int half = (L.right_col_width - gutter) / 2;
    L.pill1_top = 0;
    L.pill1_bottom = PILL_HEIGHT - 1;
    L.pill1_left = L.right_col_left;
    L.pill1_right = L.pill1_left + half - 1;
    L.pill2_top = L.pill1_top;
    L.pill2_bottom = L.pill1_bottom;
    L.pill2_left = L.pill1_right + 1 + gutter;
    L.pill2_right = L.right_col_right;
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

// Live clock: how many seconds remain for player_idx, accounting for the
// time elapsed in the current on-turn player's turn so the display ticks
// in real time. Caller must hold state->mutex.
static double seconds_remaining(const TuiGameState *state, int player_idx) {
  double used = state->seconds_used[player_idx];
  if (game_get_player_on_turn_index(state->game) == player_idx &&
      !game_over(state->game)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    used += (double)(now.tv_sec - state->turn_started.tv_sec) +
            (double)(now.tv_nsec - state->turn_started.tv_nsec) / 1e9;
  }
  return (double)state->time_per_side_seconds - used;
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
          // ASCII fallback for LDs without a fullwidth column.
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
// Both players use the same color scheme — a lighter gray (theme->fg) for
// the top row, a darker gray (theme->dim_fg) for the bottom. Selective
// bold marks the position and played-tile letters in the move, plus the
// running total on the bottom row.

static void render_history_entry(struct ncplane *plane, const Theme *theme,
                                 const TuiHistoryEntry *e, int idx,
                                 int row, int interior_left,
                                 int interior_right, int row_bottom_inclusive) {
  // ── Row 1 (lighter): " 18. L1 RE(W)I(N)              +38" ──────────────
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, theme->fg);
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "%2d. ", idx + 1);
  ncplane_putstr_yx(plane, row, interior_left, prefix);

  bool inside_paren = false;
  for (const char *p = e->move_str; *p != '\0'; p++) {
    if (*p == '(') {
      inside_paren = true;
    }
    ncplane_set_styles(plane, inside_paren ? 0 : NCSTYLE_BOLD);
    const char buf[2] = {*p, '\0'};
    ncplane_putstr(plane, buf);
    if (*p == ')') {
      inside_paren = false;
    }
  }
  ncplane_set_styles(plane, 0);

  char delta_str[16];
  snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
  const int delta_len = (int)strlen(delta_str);
  const int delta_col = interior_right - delta_len + 1;
  if (delta_col > interior_left + (int)strlen(prefix)) {
    ncplane_putstr_yx(plane, row, delta_col, delta_str);
  }

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

  const int rows_per_entry = 2;
  const int top = L->history_top + 1;             // first interior row
  const int bottom = L->history_bottom - 1;       // last interior row (inclusive)
  const int rows_avail = bottom - top + 1;
  if (!L->two_col) {
    const int interior_left = L->right_col_left + 2;
    const int interior_right = L->right_col_right - 2;
    const int max_visible = rows_avail / rows_per_entry;
    int first = state->history_count - max_visible;
    if (first < 0) {
      first = 0;
    }
    int row = top;
    for (int idx = first; idx < state->history_count; idx++) {
      render_history_entry(plane, theme, &state->history[idx], idx, row,
                           interior_left, interior_right, bottom);
      row += rows_per_entry;
      if (row > bottom) {
        break;
      }
    }
    return;
  }

  // Two-column layout. Split the panel interior in half.
  const int gutter = 2;
  const int half_width = (width - 2 - gutter) / 2;
  const int left_l = L->right_col_left + 2;
  const int left_r = left_l + half_width - 1;
  const int right_l = left_r + 1 + gutter;
  const int right_r = L->right_col_right - 2;

  const int max_visible = (rows_avail / rows_per_entry) * 2;  // both columns
  int first = state->history_count - max_visible;
  if (first < 0) {
    first = 0;
  }
  int row_left = top;
  int row_right = top;
  for (int idx = first; idx < state->history_count; idx++) {
    const bool to_left = ((idx - first) % 2) == 0;
    if (to_left) {
      if (row_left > bottom) {
        break;
      }
      render_history_entry(plane, theme, &state->history[idx], idx, row_left,
                           left_l, left_r, bottom);
      row_left += rows_per_entry;
    } else {
      if (row_right > bottom) {
        break;
      }
      render_history_entry(plane, theme, &state->history[idx], idx, row_right,
                           right_l, right_r, bottom);
      row_right += rows_per_entry;
    }
  }
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

  // Right side: shortcut hint.
  const char *right_buf = "esc for menu ";
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

  (void)time_per_side_seconds;  // now read from state->time_per_side_seconds
  render_player_pill(plane, theme, state, 0, L.pill1_top, L.pill1_left,
                     L.pill1_right);
  render_player_pill(plane, theme, state, 1, L.pill2_top, L.pill2_left,
                     L.pill2_right);
  render_history_panel(plane, theme, state, &L);

  render_status_bar(plane, theme, state, &L);
}

// ── Menu modal ────────────────────────────────────────────────────────────
void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  static const char *const items[] = {"Resume", "Quit"};
  const int item_count = (int)(sizeof(items) / sizeof(items[0]));
  const int width = 24;
  const int height = 3 + item_count;  // top + title gap + items + bottom
  if ((unsigned)width >= plane_cols || (unsigned)height >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - height) / 2;
  const int left = (int)(plane_cols - width) / 2;

  // Solid panel: paint interior with bg first, then box on top.
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  for (int r = top; r < top + height; r++) {
    for (int c = left; c < left + width; c++) {
      ncplane_putstr_yx(plane, r, c, " ");
    }
  }
  draw_box(plane, theme, top, left, height, width, "Menu");

  for (int i = 0; i < item_count; i++) {
    const int item_row = top + 2 + i;
    const bool focused = (i == focus);
    if (focused) {
      theme_apply_fg(plane, theme->bg);
      theme_apply_bg(plane, theme->accent_fg);
    } else {
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, theme->bg);
    }
    char line[32];
    snprintf(line, sizeof(line), "  %-*s", width - 4, items[i]);
    ncplane_putstr_yx(plane, item_row, left + 1, line);
  }
}

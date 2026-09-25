#include "render_bag.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "render_common.h"
#include "render_view.h"
#include "tui_ui_types.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
void render_bag_panel(struct ncplane *plane, const Theme *theme,
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

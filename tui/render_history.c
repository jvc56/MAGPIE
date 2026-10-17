#include "render_history.h"

#include "../src/ent/letter_distribution.h"
#include "config.h"
#include "game_state.h"
#include "render_common.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "theme.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

enum { HISTORY_ERROR_MAX_LINES = 6, SPINNER_FRAMES = 10 };
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
    while (taken < width && p[taken] != '\0') {
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
// A kept phony cross-word adds one informational row after them.
static int history_error_rows(const TuiHistoryEntry *e, int width) {
  const int hook_rows = e->phony_hooks[0] != '\0' ? 1 : 0;
  if (e->error_str[0] == '\0') {
    return hook_rows;
  }
  int ew = width - 2;
  if (ew < 4) {
    ew = 4;
  }
  char lines[HISTORY_ERROR_MAX_LINES][128];
  int n = wrap_error_lines(e->error_str, ew, lines, HISTORY_ERROR_MAX_LINES);
  return (n < 1 ? 1 : n) + hook_rows;
}
static int history_entry_rows(const TuiHistoryEntry *e, int width) {
  int rows = (e->end_bonus != 0 || e->challenged_off) ? 4 : 2;
  // Revalidation error: word-wrapped rows tucked under the entry's
  // move/rack pair. Lets the user see the impossible-move
  // explanation inline rather than hunting for a status line.
  rows += history_error_rows(e, width);
  return rows;
}
// Body (rows 1-2 minus the rank prefix) of an event entry — a time
// penalty, a time forfeit, or a challenged-off phony. Mirrors the
// engine's game-event presentation (GAME_EVENT_TIME_PENALTY /
// GAME_EVENT_PHONY_TILES_RETURNED): an event label where the move
// notation would go, the (negative) score adjustment in the delta
// column, and the resulting cumulative total on row 2 alongside the
// clock at the moment of the event.
static void render_clock_event_entry(struct ncplane *plane, const Theme *theme,
                                     const TuiHistoryEntry *e, int row,
                                     int interior_left, int interior_right,
                                     int row_bottom_inclusive,
                                     const char *prefix, ThemeRgb player_fg,
                                     ThemeRgb player_dim_fg,
                                     bool clocks_active) {
  // Adjustment-carrying events show their delta + new total; the
  // forfeit is text-only. Labels render in the error color when the
  // event is "bad news" (game over / turn lost); the time penalty
  // stays in the player's palette like any other scored event.
  const char *label = "time penalty";
  bool show_adjustment = true;
  bool error_color = false;
  switch (e->kind) {
  case TUI_HISTORY_ENTRY_TIME_FORFEIT:
    label = "lost on time";
    show_adjustment = false;
    error_color = true;
    break;
  case TUI_HISTORY_ENTRY_TIME_PENALTY:
  default:
    break;
  }
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, error_color ? theme->error_fg : player_fg);
  ncplane_putstr_yx(plane, row, interior_left + (int)strlen(prefix), label);
  if (show_adjustment) {
    theme_apply_fg(plane, player_fg);
    char delta_str[16];
    (void)snprintf(delta_str, sizeof(delta_str), "%d", e->score); // negative
    const int delta_len = (int)strlen(delta_str);
    const int delta_col = interior_right - delta_len + 1;
    if (delta_col > interior_left + (int)strlen(prefix)) {
      ncplane_putstr_yx(plane, row, delta_col, delta_str);
    }
  }

  // Row 2: the player's clock at the event (negative = overtime
  // depth) + the cumulative total after the adjustment.
  if (row + 1 > row_bottom_inclusive) {
    return;
  }
  const int row2 = row + 1;
  theme_apply_fg(plane, player_dim_fg);
  if (clocks_active) {
    char clock_str[16];
    format_clock(e->clock_at_end, clock_str, sizeof(clock_str));
    char left_line[32];
    (void)snprintf(left_line, sizeof(left_line), "%*s%s", (int)strlen(prefix),
                   "", clock_str);
    ncplane_putstr_yx(plane, row2, interior_left, left_line);
  }
  if (show_adjustment) {
    char total_str[16];
    (void)snprintf(total_str, sizeof(total_str), "%d", e->total_after);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row2, total_col, total_str);
    ncplane_set_styles(plane, 0);
  }
}

// Background for the selection bar of the entry being edited: the
// player's accent scaled so its brightest channel hits 50, with each
// channel floored at 24 so the dimmer channels still read as "tinted
// grey" rather than "pure black with one bright channel".
static ThemeRgb history_edit_row_bg(ThemeRgb player_fg) {
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
  return row_bg;
}

// Draws row 1 of the entry being edited: the tinted selection bar with
// its move and leave input zones, the score, and the text cursor.
static void render_history_move_editor(
    struct ncplane *plane, const Theme *theme, const TuiGameState *state,
    int row, int interior_left, int interior_right, const ThemeRgb player_fg,
    const ThemeRgb player_dim_fg, const char *prefix) {
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
  const ThemeRgb row_bg = history_edit_row_bg(player_fg);
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
                              state->rack_sort, leave_disp, sizeof(leave_disp));
  }
  const bool leave_empty = leave_disp[0] == '\0';
  // Focused but not typed in: the derived leave shows dim as a
  // placeholder, which typing replaces (an empty field keeps it).
  char leave_placeholder[24];
  leave_placeholder[0] = '\0';
  if (leave_focused && state->edit_leave_len == 0 &&
      state->edit_move_leave[0] != '\0') {
    format_alphagram_for_sort(state->edit_move_leave, state->ld,
                              state->rack_sort, leave_placeholder,
                              sizeof(leave_placeholder));
  }
  const int placeholder_len = (int)strlen(leave_placeholder);
  // Ensure the zone is wide enough for the cursor when focused
  // — at least 1 cell wider than the text so an end-of-buffer
  // cursor has somewhere to sit. Caps at 8 cells (max practical
  // leave length + cursor).
  int leave_zone_w = leave_empty ? 1 : (int)strlen(leave_disp);
  if (leave_focused) {
    leave_zone_w =
        (state->edit_leave_len > placeholder_len ? state->edit_leave_len
                                                 : placeholder_len) +
        1;
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
      // not focused. An untyped focused field shows the derived
      // leave dim instead.
      for (int j = 0; j < placeholder_len; j++) {
        const int col = leave_zone_left + j;
        if (col > leave_zone_right) {
          break;
        }
        char ch[2] = {leave_placeholder[j], '\0'};
        theme_apply_bg(plane, zone_bg);
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, row, col, ch);
      }
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
      } else if (state->edit_leave_cursor < placeholder_len) {
        ch[0] = leave_placeholder[state->edit_leave_cursor];
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
    (void)snprintf(score_str, sizeof(score_str), "+%d", state->edit_move_score);
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
}

// Draws row 2 of the entry being edited: the tinted selection bar with
// the rack input zone (edit buffer plus cursor) overlaid on the rack.
static void render_history_rack_editor(
    struct ncplane *plane, const Theme *theme, const TuiGameState *state,
    const TuiHistoryEntry *e, int interior_left, int interior_right,
    bool clocks_active, const LetterDistribution *ld, const ThemeRgb player_fg,
    const ThemeRgb player_dim_fg, const char *prefix, int row2) {
  int rack_col = interior_left + (int)strlen(prefix);
  if (clocks_active) {
    char clock_str[16];
    format_clock(e->clock_at_start, clock_str, sizeof(clock_str));
    rack_col += (int)strlen(clock_str) + 1;
  }
  // Same player-tinted row bg as row 1.
  ThemeRgb row_bg = history_edit_row_bg(player_fg);
  // Play-vs-computer: the rack row is a read-only display of the
  // human's full rack, not an editable field. Paint the whole row
  // black so it reads as "not editable" (vs the player-tinted
  // editable rack row in annotation mode).
  const bool pvc_readonly = state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER;
  if (pvc_readonly) {
    row_bg.r = 0;
    row_bg.g = 0;
    row_bg.b = 0;
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
  if (pvc_readonly) {
    // Read-only: always show the human's full real rack (set on the
    // pending entry at turn start), alphagrammed, regardless of what's
    // typed on the board.
    if (e->rack_str[0] != '\0' && ld != NULL) {
      format_alphagram_for_sort(e->rack_str, state->ld, state->rack_sort,
                                display_buf, sizeof(display_buf));
    }
  } else if (state->edit_field == TUI_EDIT_FIELD_RACK &&
             state->edit_rack_len > 0) {
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
  if (state->edit_field == TUI_EDIT_FIELD_RACK && !pvc_readonly) {
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

// Rows 3-4 of a challenged-off phony: the event label with the
// cancelling adjustment, then the corrected running total.
static void render_history_challenged_rows(
    struct ncplane *plane, const Theme *theme, const TuiHistoryEntry *e,
    int row, int interior_left, int interior_right, int row_bottom_inclusive,
    const char *prefix, const ThemeRgb player_fg,
    const ThemeRgb player_dim_fg) {
  if (row + 2 > row_bottom_inclusive) {
    return;
  }
  const int challenge_row = row + 2;
  ncplane_set_styles(plane, 0);
  theme_apply_bg(plane, theme->bg);
  theme_apply_fg(plane, theme->error_fg);
  char challenge_left[32];
  (void)snprintf(challenge_left, sizeof(challenge_left), "%*schallenged off",
                 (int)strlen(prefix), "");
  ncplane_putstr_yx(plane, challenge_row, interior_left, challenge_left);
  char delta_chal_str[16];
  (void)snprintf(delta_chal_str, sizeof(delta_chal_str), "%d", -e->score);
  const int delta_chal_len = (int)strlen(delta_chal_str);
  const int delta_chal_col = interior_right - delta_chal_len + 1;
  if (delta_chal_col > interior_left + (int)strlen(challenge_left)) {
    theme_apply_fg(plane, player_fg);
    ncplane_putstr_yx(plane, challenge_row, delta_chal_col, delta_chal_str);
  }
  if (row + 3 > row_bottom_inclusive) {
    return;
  }
  // No clock on the resolution row — the player's next turn shows
  // the same value as its start clock.
  const int resolve_row = row + 3;
  theme_apply_fg(plane, player_dim_fg);
  char corrected_str[16];
  (void)snprintf(corrected_str, sizeof(corrected_str), "%d",
                 e->total_after - e->score);
  const int corrected_len = (int)strlen(corrected_str);
  const int corrected_col = interior_right - corrected_len + 1;
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, resolve_row, corrected_col, corrected_str);
  ncplane_set_styles(plane, 0);
}

// Rows 3-4 of the move that ended the game: the opponent's leftover
// rack with the going-out bonus, then the final clock and score.
static void render_history_end_bonus_rows(
    struct ncplane *plane, const Theme *theme, const TuiHistoryEntry *e,
    int row, int interior_left, int interior_right, int row_bottom_inclusive,
    const char *prefix, const ThemeRgb player_fg, const ThemeRgb player_dim_fg,
    bool clocks_active, const LetterDistribution *ld, TuiRackSort rack_sort) {
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
      (void)snprintf(sorted_end, sizeof(sorted_end), "%s", e->end_rack_str);
    }
    (void)snprintf(bonus_left, sizeof(bonus_left), "    (%s)", sorted_end);
  } else {
    (void)snprintf(bonus_left, sizeof(bonus_left), "    ");
  }
  theme_apply_fg(plane, opponent_fg);
  ncplane_putstr_yx(plane, row3, interior_left, bonus_left);

  char delta3_str[16];
  (void)snprintf(delta3_str, sizeof(delta3_str), "+%d", e->end_bonus);
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
    format_clock(e->clock_at_end, end_clock_str, sizeof(end_clock_str));
    char end_line[32];
    (void)snprintf(end_line, sizeof(end_line), "%*s%s", (int)strlen(prefix), "",
                   end_clock_str);
    theme_apply_fg(plane, player_fg);
    ncplane_putstr_yx(plane, row4, interior_left, end_line);
  }

  theme_apply_fg(plane, player_dim_fg);
  char total4_str[16];
  (void)snprintf(total4_str, sizeof(total4_str), "%d",
                 e->total_after + e->end_bonus);
  const int total4_len = (int)strlen(total4_str);
  const int total4_col = interior_right - total4_len + 1;
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, row4, total4_col, total4_str);
  ncplane_set_styles(plane, 0);
}

// "*" right after the move text (where render_move_styled left the
// cursor) when the play's main word is a phony the annotator kept.
static void render_phony_mark(struct ncplane *plane, const TuiHistoryEntry *e) {
  if (e->phony_main) {
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr(plane, "*");
    ncplane_set_styles(plane, 0);
  }
}

// Row 1 of a finalized entry: the move (masked for a concealed exchange),
// its score, and the leave.
static void
render_history_played_move(struct ncplane *plane, const Theme *theme,
                           const TuiHistoryEntry *e, int row, int interior_left,
                           int interior_right, int leave_col_w,
                           const LetterDistribution *ld, TuiRackSort rack_sort,
                           bool conceal_tiles, const ThemeRgb player_fg,
                           const ThemeRgb player_dim_fg, const char *prefix) {
  // All paren groups in the engine's move notation are playthrough
  // (the lowercase-inside-parens case is a playthrough blank, not a
  // newly-played blank — newly-played blanks render as lowercase
  // letters with no parens). Drop them all and render the content
  // non-bold so playthrough tiles read as background context.
  if (conceal_tiles && e->move_str[0] == '-') {
    // Concealed exchange: hide which tiles were swapped, showing one
    // dot per tile (the count matches the rack-concealment style).
    char masked[48];
    int p = 0;
    masked[p++] = '-';
    for (int i = 1; e->move_str[i] != '\0' && p + 4 < (int)sizeof(masked);
         i++) {
      masked[p++] = '\xe2';
      masked[p++] = '\x80';
      masked[p++] = '\xa2'; // U+2022 BULLET
    }
    masked[p] = '\0';
    theme_apply_fg(plane, player_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, 0);
    ncplane_putstr_yx(plane, row, interior_left + (int)strlen(prefix), masked);
  } else {
    render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                       e->move_str, /*hide_parens=*/true,
                       /*hide_playthrough_parens=*/false);
    render_phony_mark(plane, e);
  }

  char delta_str[16];
  (void)snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
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
    const char *leave_text =
        (empty || conceal_tiles) ? "\xc2\xb7" : sorted_leave;
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

// Row 1 of a turn the bot is still computing: a braille spinner where the
// move will go.
static void render_history_bot_spinner(struct ncplane *plane,
                                       const TuiGameState *state,
                                       const TuiHistoryEntry *e,
                                       bool clocks_active) {
  // Bot is still computing this turn — show a braille spinner where
  // the move notation will go and leave the +score column blank.
  // 10-frame cycle at ~80ms per frame derives from CLOCK_MONOTONIC
  // so the animation runs even when the renderer is otherwise idle.
  //
  // Skip the spinner in CGP / non-bot mode: no one is "thinking,"
  // and showing a perpetual spinner reads as the app being busy.
  // Also skip it on the human's own pending turn in play-vs-computer —
  // it's the human's move to make, not the bot computing.
  const bool human_pending = state != NULL &&
                             state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                             e->player_idx == state->human_player_idx;
  if (clocks_active && !human_pending) {
    static const char *const spinner_frames[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
        "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
        "\xe2\xa0\x87", "\xe2\xa0\x8f",
    };
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t ms =
        (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    const int frame = (int)((ms / 80) % SPINNER_FRAMES);
    ncplane_putstr(plane, spinner_frames[frame]);
  }
}

// Row 1 of a pending annotation row whose move has been committed: the
// move, score, and leave, drawn as a finalized entry would be.
static void render_history_committed_move(
    struct ncplane *plane, const Theme *theme, const TuiHistoryEntry *e,
    int row, int interior_left, int interior_right,
    const LetterDistribution *ld, TuiRackSort rack_sort, bool conceal_tiles,
    const ThemeRgb player_fg, const ThemeRgb player_dim_fg,
    const char *prefix) {
  // Annotation in progress: the user has committed a move
  // into this still-pending row (via Enter on the move field).
  // Render the move text + leave + "+score" the same way a
  // finalized entry would, so closing the editor doesn't make
  // the committed text disappear.
  render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                     e->move_str, /*hide_parens=*/true,
                     /*hide_playthrough_parens=*/false);
  render_phony_mark(plane, e);
  char delta_str[16];
  (void)snprintf(delta_str, sizeof(delta_str), "+%d", e->score);
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
    const char *leave_text =
        (empty || conceal_tiles) ? "\xc2\xb7" : sorted_leave;
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
}

// Draws the rank prefix of a pending turn with its "N." chunk in tile
// colors, so it reads like a played tile next to the spinner.
static void render_history_pending_prefix(
    struct ncplane *plane, const Theme *theme, const TuiHistoryEntry *e,
    int row, int interior_left, const ThemeRgb player_fg, const char *prefix) {
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
  theme_apply_fg(plane, e->player_idx == 1 ? theme->tile2_fg : theme->tile1_fg);
  theme_apply_bg(plane, e->player_idx == 1 ? theme->tile2_bg : theme->tile1_bg);
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  ncplane_putstr_yx(plane, row, interior_left + digit_start, tile_part);
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, player_fg);
  theme_apply_bg(plane, theme->bg);
  if (prefix[after_period] != '\0') {
    ncplane_putstr_yx(plane, row, interior_left + after_period,
                      prefix + after_period);
  }
}

// Sum of the committed scores (plus end bonuses) of `player_idx`'s
// entries before history index `idx`, skipping challenged-off plays.
static int history_player_total_before(const TuiGameState *state, int idx,
                                       int player_idx) {
  int total_before = 0;
  for (int prev = 0; prev < idx; prev++) {
    const TuiHistoryEntry *pe = &state->history[prev];
    if (pe->player_idx == player_idx && !pe->pending) {
      if (!pe->challenged_off) {
        total_before += pe->score + pe->end_bonus;
      }
    }
  }
  return total_before;
}

// Draws the rank prefix of the entry under the history cursor as an
// inverted "N>" chip ("N." when the panel is unfocused, non-inverted while
// the entry is being edited).
static void render_history_cursor_prefix(struct ncplane *plane,
                                         const Theme *theme, int row,
                                         int interior_left,
                                         bool history_focused, bool editing,
                                         const ThemeRgb player_fg,
                                         const char *prefix) {
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
  // Play-vs-computer conceals the computer's private tiles (rack + leave)
  // in the History panel until the game ends — the played move and score
  // are public, but the rack/leave are not. Revealed at game over.
  const bool conceal_tiles =
      state != NULL && state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
      e->player_idx != state->human_player_idx && state->game != NULL &&
      !tui_game_state_play_over(state);
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
  (void)snprintf(prefix, sizeof(prefix), "%*d. ",
                 rank_digits > 0 ? rank_digits : 1, idx + 1);
  if (cursor_here) {
    render_history_cursor_prefix(plane, theme, row, interior_left,
                                 history_focused, editing, player_fg, prefix);
  } else if (e->pending) {
    render_history_pending_prefix(plane, theme, e, row, interior_left,
                                  player_fg, prefix);
  } else {
    ncplane_putstr_yx(plane, row, interior_left, prefix);
  }

  // Clock events (time penalty / time forfeit) have no move, rack, or
  // leave — render their dedicated two-row body and stop.
  if (e->kind != TUI_HISTORY_ENTRY_MOVE) {
    render_clock_event_entry(plane, theme, e, row, interior_left,
                             interior_right, row_bottom_inclusive, prefix,
                             player_fg, player_dim_fg, clocks_active);
    return;
  }

  if (editing) {
    render_history_move_editor(plane, theme, state, row, interior_left,
                               interior_right, player_fg, player_dim_fg,
                               prefix);
  } else if (e->pending && e->move_str[0] != '\0') {
    render_history_committed_move(plane, theme, e, row, interior_left,
                                  interior_right, ld, rack_sort, conceal_tiles,
                                  player_fg, player_dim_fg, prefix);
  } else if (e->pending) {
    render_history_bot_spinner(plane, state, e, clocks_active);
  } else {
    render_history_played_move(plane, theme, e, row, interior_left,
                               interior_right, leave_col_w, ld, rack_sort,
                               conceal_tiles, player_fg, player_dim_fg, prefix);
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
  const char *rack_disp = "\xe2\x80\x94"; // em dash
  if (conceal_tiles) {
    rack_disp = "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80"
                "\xa2\xe2\x80\xa2\xe2\x80\xa2"; // seven bullets
  } else if (sorted_rack[0] != '\0') {
    rack_disp = sorted_rack;
  } else if (e->pending) {
    rack_disp = "";
  }
  // Row 2 indent matches the prefix length so the secondary
  // info (clock + rack, or just rack in CGP mode) aligns with
  // where the move started on row 1 ("4. 14F XU" → "   2:45 EGIPS").
  if (clocks_active) {
    char clock_str[16];
    format_clock(e->clock_at_start, clock_str, sizeof(clock_str));
    (void)snprintf(left_line, sizeof(left_line), "%*s%s %s",
                   (int)strlen(prefix), "", clock_str, rack_disp);
  } else {
    (void)snprintf(left_line, sizeof(left_line), "%*s%s", (int)strlen(prefix),
                   "", rack_disp);
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
    render_history_rack_editor(plane, theme, state, e, interior_left,
                               interior_right, clocks_active, ld, player_fg,
                               player_dim_fg, prefix, row2);
    // The editor leaves its last colors applied (the rack cursor's black
    // on white when RACK has focus), which hid the total drawn below;
    // restore row 2's.
    theme_apply_fg(plane, player_dim_fg);
    theme_apply_bg(plane, theme->bg);
  }

  if (!e->pending) {
    char total_str[16];
    (void)snprintf(total_str, sizeof(total_str), "%d", e->total_after);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    if (total_col > interior_left + (int)strlen(left_line)) {
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr_yx(plane, row2, total_col, total_str);
      ncplane_set_styles(plane, 0);
    }
  } else if (!editing && e->move_str[0] != '\0') {
    // Pending row with a committed move — show the cumulative
    // total the same way a finalized row does. Sum prior same-
    // player committed scores and add this entry's score.
    const int total_before =
        history_player_total_before(state, idx, e->player_idx);
    char total_str[16];
    (void)snprintf(total_str, sizeof(total_str), "%d", total_before + e->score);
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
    const int total_before =
        history_player_total_before(state, idx, e->player_idx);
    char total_str[16];
    (void)snprintf(total_str, sizeof(total_str), "%d",
                   total_before + state->edit_move_score);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    theme_apply_fg(plane, player_dim_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row2, total_col, total_str);
    ncplane_set_styles(plane, 0);
  }

  // ── Rows 3-4 (challenged-off phony): the auto-challenge outcome,
  // folded into the play's own entry so the two-column history keeps
  // its index-parity column assignment. Row 3 carries the event label
  // and the cancelling adjustment; row 4 the clock at resolution and
  // the corrected running total (back to where it was before the
  // play).
  if (e->challenged_off) {
    render_history_challenged_rows(plane, theme, e, row, interior_left,
                                   interior_right, row_bottom_inclusive, prefix,
                                   player_fg, player_dim_fg);
    return;
  }

  render_history_end_bonus_rows(
      plane, theme, e, row, interior_left, interior_right, row_bottom_inclusive,
      prefix, player_fg, player_dim_fg, clocks_active, ld, rack_sort);
}
// Render the entry's revalidation error message on the row just
// past the entry's main body. `err_row` is the absolute screen
// row to draw on; the message is truncated to fit between
// interior_left and interior_right. Caller has already reserved
// the row via history_entry_rows. Two-column callers reserve the
// row per-entry, so two adjacent entries' errors can coexist on
// the same screen row (each in its own column).
// Draws the entry's kept phony cross-words ("phony hook: ZE*") at
// `row`, dim: the play stands, this is just information. Returns the
// rows drawn (0 or 1).
static int render_history_hook_row(struct ncplane *plane, const Theme *theme,
                                   const TuiHistoryEntry *e, int row,
                                   int interior_left, int width) {
  if (e->phony_hooks[0] == '\0') {
    return 0;
  }
  char text[96];
  (void)snprintf(text, sizeof(text), "phony hook: %s", e->phony_hooks);
  if ((int)strlen(text) > width) {
    text[width > 0 ? width : 0] = '\0';
  }
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  ncplane_putstr_yx(plane, row, interior_left, text);
  return 1;
}
// Render the entry's (word-wrapped) error message starting at
// `err_row`. The first line carries the "⚠ " glyph prefix;
// continuation lines indent 2 cells to align under the message.
// Returns the number of rows drawn (matches history_error_rows).
static int render_history_error_row(struct ncplane *plane, const Theme *theme,
                                    const TuiHistoryEntry *e, int err_row,
                                    int interior_left, int interior_right) {
  if (e == NULL) {
    return 0;
  }
  const int width = interior_right - interior_left + 1;
  if (width <= 0) {
    return 0;
  }
  if (e->error_str[0] == '\0') {
    return render_history_hook_row(plane, theme, e, err_row, interior_left,
                                   width);
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
  return n + render_history_hook_row(plane, theme, e, err_row + n,
                                     interior_left, width);
}
void render_history_panel(struct ncplane *plane, const Theme *theme,
                          const TuiGameState *state, const Layout *L) {
  TuiHitMaps *hit = tui_hit_maps();
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
  hit->history_row_map_count = 0;
  hit->history_panel_top = L->history_top;
  hit->history_panel_bottom = L->history_bottom;
  hit->history_panel_left = L->right_col_left;
  hit->history_panel_right = L->right_col_right;
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
      const int err_rows = history_error_rows(e, interior_width);
      if (err_rows > 0) {
        render_history_error_row(plane, theme, e, row + entry_rows - err_rows,
                                 interior_left, interior_right);
      }
      if (hit->history_row_map_count < (int)(sizeof(hit->history_row_map) /
                                             sizeof(hit->history_row_map[0]))) {
        HistoryRowMap *m = &hit->history_row_map[hit->history_row_map_count++];
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
      if (err_rows > 0) {
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
      if (err_rows > 0) {
        render_history_error_row(plane, theme, e, row_right + rows - err_rows,
                                 right_l, right_r);
      }
      row_right += rows;
    }
    if (hit->history_row_map_count <
        (int)(sizeof(hit->history_row_map) / sizeof(hit->history_row_map[0]))) {
      HistoryRowMap *m = &hit->history_row_map[hit->history_row_map_count++];
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

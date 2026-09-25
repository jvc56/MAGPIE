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
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "analysis_rows.h"
#include "frame_dump.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "mach_compat.h"
#include "pixel_compose.h"
#include "render_bag.h"
#include "render_board.h"
#include "render_common.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "render_pills.h"
#include "render_planes.h"
#include "render_rack.h"
#include "render_view.h"
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

// Three tile scales:
//   0 = halfwidth (1 col × 1 row per cell, single ASCII glyph)
//   1 = fullwidth (2 cols × 1 row, fullwidth Unicode glyph)
//   2 = double    (4 cols × 2 rows, FreeType pixel composite)
// compute_effective_scale picks the largest scale ≤ user_pref that fits
// the current plane. Returns -1 when even halfwidth is too cramped.

// ── Pixel-graphics grid overlay ───────────────────────────────────────────
//
// On terminals that support pixel graphics (Kitty graphics protocol or
// Sixel — ghostty/iTerm2/kitty/foot/modern xterm), draw an RGBA bitmap
// of N-pixel borders in theme->bg over a child plane positioned at a
// region of cells. Pixels with alpha=0 composite through to the std plane,
// so the cells' glyph content stays readable underneath. On terminals
// without pixel support, the calls are no-ops via notcurses_canpixel.

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

// Last measured keypress-to-pixels latency (microseconds). -1 = none yet.
static _Atomic long g_input_lag_us = -1;
void tui_debug_set_input_lag_us(long us) { atomic_store(&g_input_lag_us, us); }

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

// Tear down every cached tile plane: the board's per-cell planes, the
// rack's per-slot planes and the edit-arrow plane. Used when scale
// flips back to 1x and at game-reset / state-destroy.
static void invalidate_tile_planes(void) {
  render_board_invalidate_tile_planes();
  invalidate_rack_tile_planes();
  invalidate_edit_arrow_plane();
}

static void invalidate_blit_caches(void) {
  render_board_invalidate_blit_caches();
  render_rack_invalidate_blit_cache();
}

static void invalidate_grid_planes(void) {
  tui_planes_destroy_all();
  invalidate_blit_caches();
  invalidate_tile_planes();
}

void tui_game_render_reset_grids(void) { invalidate_grid_planes(); }

// ── Rack panel ────────────────────────────────────────────────────────────
//
// Rack tiles scale alongside the board: at scale=2 we composite an RGBA
// strip via FreeType (same path as the board); at scale=1 we use
// fullwidth Unicode glyphs in cells; at scale=0 we collapse to single
// ASCII chars. The panel box gains one row at scale=2 so the 2-row
// tiles fit.

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
    snprintf(delta_str, sizeof(delta_str), "%d", e->score); // negative
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
    snprintf(left_line, sizeof(left_line), "%*s%s", (int)strlen(prefix), "",
             clock_str);
    ncplane_putstr_yx(plane, row2, interior_left, left_line);
  }
  if (show_adjustment) {
    char total_str[16];
    snprintf(total_str, sizeof(total_str), "%d", e->total_after);
    const int total_len = (int)strlen(total_str);
    const int total_col = interior_right - total_len + 1;
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row2, total_col, total_str);
    ncplane_set_styles(plane, 0);
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

  // Clock events (time penalty / time forfeit) have no move, rack, or
  // leave — render their dedicated two-row body and stop.
  if (e->kind != TUI_HISTORY_ENTRY_MOVE) {
    render_clock_event_entry(plane, theme, e, row, interior_left,
                             interior_right, row_bottom_inclusive, prefix,
                             player_fg, player_dim_fg, clocks_active);
    return;
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
  } else if (e->pending) {
    // Bot is still computing this turn — show a braille spinner where
    // the move notation will go and leave the +score column blank.
    // 10-frame cycle at ~80ms per frame derives from CLOCK_MONOTONIC
    // so the animation runs even when the renderer is otherwise idle.
    //
    // Skip the spinner in CGP / non-bot mode: no one is "thinking,"
    // and showing a perpetual spinner reads as the app being busy.
    // Also skip it on the human's own pending turn in play-vs-computer —
    // it's the human's move to make, not the bot computing.
    const bool human_pending =
        state != NULL && state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
        e->player_idx == state->human_player_idx;
    if (clocks_active && !human_pending) {
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
      ncplane_putstr_yx(plane, row, interior_left + (int)strlen(prefix),
                        masked);
    } else {
      render_move_styled(plane, row, interior_left + (int)strlen(prefix),
                         e->move_str, /*hide_parens=*/true,
                         /*hide_playthrough_parens=*/false);
    }

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
  const char *rack_disp = conceal_tiles ? "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80"
                                          "\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2"
                                          "\x80\xa2\xe2\x80\xa2"
                          : sorted_rack[0] != '\0' ? sorted_rack
                          : e->pending             ? ""
                                                   : "\xe2\x80\x94";
  // Row 2 indent matches the prefix length so the secondary
  // info (clock + rack, or just rack in CGP mode) aligns with
  // where the move started on row 1 ("4. 14F XU" → "   2:45 EGIPS").
  if (clocks_active) {
    char clock_str[16];
    format_clock(e->clock_at_start, clock_str, sizeof(clock_str));
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
      format_clock(e->clock_at_start, clock_str, sizeof(clock_str));
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
        if (!pe->challenged_off) {
          total_before += pe->score + pe->end_bonus;
        }
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
        if (!pe->challenged_off) {
          total_before += pe->score + pe->end_bonus;
        }
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

  // ── Rows 3-4 (challenged-off phony): the auto-challenge outcome,
  // folded into the play's own entry so the two-column history keeps
  // its index-parity column assignment. Row 3 carries the event label
  // and the cancelling adjustment; row 4 the clock at resolution and
  // the corrected running total (back to where it was before the
  // play).
  if (e->challenged_off) {
    if (row + 2 > row_bottom_inclusive) {
      return;
    }
    const int challenge_row = row + 2;
    ncplane_set_styles(plane, 0);
    theme_apply_bg(plane, theme->bg);
    theme_apply_fg(plane, theme->error_fg);
    char challenge_left[32];
    snprintf(challenge_left, sizeof(challenge_left), "%*schallenged off",
             (int)strlen(prefix), "");
    ncplane_putstr_yx(plane, challenge_row, interior_left, challenge_left);
    char delta_chal_str[16];
    snprintf(delta_chal_str, sizeof(delta_chal_str), "%d", -e->score);
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
    snprintf(corrected_str, sizeof(corrected_str), "%d",
             e->total_after - e->score);
    const int corrected_len = (int)strlen(corrected_str);
    const int corrected_col = interior_right - corrected_len + 1;
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, resolve_row, corrected_col, corrected_str);
    ncplane_set_styles(plane, 0);
    return;
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
    format_clock(e->clock_at_end, end_clock_str, sizeof(end_clock_str));
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
      if (e->error_str[0] != '\0') {
        const int err_rows = history_error_rows(e, interior_width);
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
  TuiHitMaps *hit = tui_hit_maps();
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
  hit->analysis_row_map_count = 0;
  hit->analysis_panel_top = L->analysis_top;
  hit->analysis_panel_bottom = L->analysis_bottom;
  hit->analysis_panel_left = L->analysis_left;
  hit->analysis_panel_right = L->analysis_right;
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
    if (hit->analysis_row_map_count < (int)(sizeof(hit->analysis_row_map) /
                                            sizeof(hit->analysis_row_map[0]))) {
      AnalysisRowMap *m = &hit->analysis_row_map[hit->analysis_row_map_count++];
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
                 hit->analysis_row_map_count);
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
  if (!resume_active_for_cursor(state) && state->history_cursor >= 0 &&
      state->history_cursor < state->history_count) {
    const TuiHistoryEntry *cursor_entry =
        &state->history[state->history_cursor];
    if (!cursor_entry->pending && cursor_entry->analysis_snapshot.valid) {
      snap = &cursor_entry->analysis_snapshot;
    }
  }

  // Pick mode: when replaying a snapshot, use whatever mode it
  // captured. Otherwise, endgame when the analyzed position's bag has
  // run dry (and we have a saved snapshot from a completed solve);
  // else sim. The live endgame snapshot persists across turns and
  // through game-over, so the last-completed endgame analysis stays
  // on screen after the game ends — which is what you want to study a
  // finished game.
  const Game *src_game = analysis_source_game(state);
  const bool bag_empty =
      src_game != NULL && bag_get_letters(game_get_bag(src_game)) == 0;
  const bool use_endgame = snap != NULL
                               ? (!snap->is_sim && !snap->is_peg)
                               : (bag_empty && state->endgame_snapshot.valid &&
                                  state->endgame_snapshot.num_entries > 0);
  const bool use_peg =
      snap != NULL
          ? snap->is_peg
          : (!use_endgame && tui_position_in_peg_range(src_game) &&
             state->peg_poll != NULL &&
             atomic_load(&((TuiGameState *)state)->peg_results_turn_idx) >= 0);

  // Title varies by mode.
  char title[64];
  if (use_peg) {
    // "PEG (2p 5/16)" — fidelity (plies) of the ranking shown, plus
    // done/field progress through the current stage. The live meta
    // was refreshed by this frame's row build from the same poll
    // snapshot the rows came from.
    if (snap != NULL) {
      if (snap->peg_fidelity > 0) {
        snprintf(title, sizeof(title), "PEG (%dp%s)", snap->peg_fidelity,
                 snap->peg_done ? "" : " partial");
      } else {
        snprintf(title, sizeof(title), "PEG");
      }
    } else {
      const TuiPegLiveMeta *meta = &state->peg_live_meta;
      const bool searching =
          atomic_load(&((TuiGameState *)state)->peg_results_active);
      if (meta->valid && searching && meta->field_size > 0) {
        // Fidelity 0 is the greedy seed stage — label it instead of
        // showing a meaningless "0p".
        if (meta->fidelity > 0) {
          snprintf(title, sizeof(title), "PEG (%dp %d/%d)", meta->fidelity,
                   meta->cands_done, meta->field_size);
        } else {
          snprintf(title, sizeof(title), "PEG (seed %d/%d)", meta->cands_done,
                   meta->field_size);
        }
      } else if (meta->valid && meta->fidelity > 0) {
        snprintf(title, sizeof(title), "PEG (%dp)", meta->fidelity);
      } else if (searching) {
        snprintf(title, sizeof(title), "PEG (starting\xe2\x80\xa6)");
      } else {
        snprintf(title, sizeof(title), "PEG");
      }
    }
  } else if (use_endgame) {
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
  const bool play_only_fallback =
      snap == NULL && !use_endgame && !use_peg && !sim_has_data;
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

// Render-health "fps": derived from the peak notcurses_render duration
// over the recent window (g_max_frame_us), NOT the frame-to-frame
// interval. The main loop now renders conditionally — it deliberately
// idles when nothing on screen changed — so an interval-based rate would
// read a misleading 1-2 fps on a static screen even though every render
// is instant. Reporting 1 / worst-recent-render-time (capped at the 60fps
// target) answers the question that actually matters — "are renders fast
// enough to feel smooth" — and only dips when a frame genuinely takes a
// long time to emit (e.g. a heavy 2x pixel-board re-blit).
static double measure_fps(void) {
  const long peak_us = atomic_load(&g_max_frame_us);
  if (peak_us <= 0) {
    return 60.0; // no slow frame measured yet — renders are instant
  }
  double fps = 1e6 / (double)peak_us;
  if (fps > 60.0) {
    fps = 60.0;
  }
  return fps;
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
      {"copy", "Copy current position to clipboard as CGP"},
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
  // Hide nps while the human is on turn in play-vs-computer — there's no
  // bot search running, so a lingering/decaying nps readout is just noise.
  const bool human_on_turn =
      state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER && state->game != NULL &&
      !tui_game_state_play_over(state) &&
      game_get_player_on_turn_index(state->game) == state->human_player_idx;
  const bool show_nps = nps >= 1.0 && !human_on_turn;

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
  // Keypress-to-pixels latency (the time from a keystroke dirtying a
  // frame to that frame rendering). Shown in ms once we've measured one.
  const long input_lag_us = atomic_load(&g_input_lag_us);
  if (input_lag_us >= 0) {
    char lag_buf[32];
    snprintf(lag_buf, sizeof(lag_buf), " \xc2\xb7 %ld ms lag",
             (input_lag_us + 500) / 1000);
    ncplane_putstr(plane, lag_buf);
  }
  // Transient notice (e.g. "Copied CGP"). Expires via notice_expires_at;
  // the once-a-second clock render tick repaints the bar without it
  // within a second of expiry.
  if (state->notice_buf[0] != '\0' && (state->notice_expires_at.tv_sec != 0 ||
                                       state->notice_expires_at.tv_nsec != 0)) {
    struct timespec notice_now;
    clock_gettime(CLOCK_MONOTONIC, &notice_now);
    const bool notice_live =
        notice_now.tv_sec < state->notice_expires_at.tv_sec ||
        (notice_now.tv_sec == state->notice_expires_at.tv_sec &&
         notice_now.tv_nsec < state->notice_expires_at.tv_nsec);
    if (notice_live) {
      char notice_seg[80];
      snprintf(notice_seg, sizeof(notice_seg), " \xc2\xb7 %s",
               state->notice_buf);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr(plane, notice_seg);
      ncplane_set_styles(plane, 0);
    }
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
  TuiGridPlanes *planes = tui_grid_planes();
  TuiHitMaps *hit = tui_hit_maps();
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }

  // Invalidate modal hit-test data at the start of each frame.
  // render_modal_ex will set it valid again if a modal renders.
  hit->modal_hit_map.valid = false;

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
  // Keep the plane synced to the terminal, but DON'T let this call's
  // return value drive the (expensive) full repaint below. On terminals
  // where the ioctl size and notcurses' clamped plane size persistently
  // disagree, tui_sync_plane_to_terminal returns true every frame; using
  // it to trigger the repaint re-rasterized all ~225 board sprixels each
  // frame (~250ms — the 4fps). A genuine resize still shows up as a
  // dimension change (plane_resized_externally) on the next frame.
  tui_sync_plane_to_terminal(plane);
  const bool resized = plane_resized_externally;
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
    ncplane_erase(plane);
    return;
  }

  // Clear the plane before redrawing. At 2x the board widget (box,
  // labels, and the 15x15 grid) is painted with per-cell pixel
  // sprixels; a blanket ncplane_erase would mark the cells UNDER those
  // sprixels dirty every frame, forcing notcurses to re-emit all ~225
  // board sprixels (~250ms — the source of input lag in 2x mode).
  // Instead erase only the regions OUTSIDE the board widget so its
  // cells stay unchanged and the sprixels elide; the board renderers
  // overwrite the box / labels with identical content, leaving nothing
  // under the board dirty. At 1x there are no sprixels, so a plain
  // full erase is both correct and cheap.
  if (L.scale >= 2) {
    unsigned total_rows = 0;
    unsigned total_cols = 0;
    ncplane_dim_yx(plane, &total_rows, &total_cols);
    // The board widget AND the rack panel (directly below it) are the
    // two big pixel-sprixel clusters in the left column. Protect both
    // from the erase so their sprixels elide; the box / title / tiles
    // are overwritten in place by their renderers. The bag panel below
    // the rack is plain text (changes as tiles are drawn) so it stays
    // in the erased zone.
    const int widget_w = L.board_width;
    const int widget_last_row = L.rack_bottom;
    // Right of the board + rack column (rows 0..widget_last_row).
    if ((int)total_cols > widget_w) {
      ncplane_erase_region(plane, 0, widget_w, widget_last_row + 1,
                           (int)total_cols - widget_w);
    }
    // Everything below the rack (bag panel, status/command bars).
    if ((int)total_rows > widget_last_row + 1) {
      ncplane_erase_region(plane, widget_last_row + 1, 0,
                           (int)total_rows - (widget_last_row + 1),
                           (int)total_cols);
    }
  } else {
    ncplane_erase(plane);
  }

  // Prepare the analysis rows once per frame, BEFORE rendering
  // the board — the on-board candidate preview resolves the
  // Analysis cursor against this prepared row list (including
  // the MOVE-column anchor lookup), so it must be in sync with
  // what render_analysis_panel is about to paint.
  populate_frame_analysis_rows(state);

  render_board(plane, theme, state, &L);
  // The text-mode (1x) board layout also drops the rack's and the edit
  // arrow's 2x pixel planes, whose stale images would otherwise sit on top.
  if (!(L.scale >= 2 && state->glyph_cache != NULL)) {
    invalidate_rack_tile_planes();
    invalidate_edit_arrow_plane();
  }
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
  if (modal == TUI_MODAL_NONE && planes->modal != NULL) {
    ncplane_destroy(planes->modal);
    planes->modal = NULL;
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
  TuiGridPlanes *planes = tui_grid_planes();
  TuiHitMaps *hit = tui_hit_maps();
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
  hit->modal_hit_map.valid = true;
  hit->modal_hit_map.outer_top = top;
  hit->modal_hit_map.outer_bottom = top + height - 1;
  hit->modal_hit_map.outer_left = left;
  hit->modal_hit_map.outer_right = left + width - 1;
  hit->modal_hit_map.top = top + 1; // first item row
  hit->modal_hit_map.left = left + 1;
  hit->modal_hit_map.right = left + width - 2;
  hit->modal_hit_map.item_count =
      item_count < MODAL_MAX_ITEMS ? item_count : MODAL_MAX_ITEMS;
  for (int i = 0; i < hit->modal_hit_map.item_count; i++) {
    hit->modal_hit_map.disabled[i] = disabled != NULL && disabled[i];
    hit->modal_hit_map.left_chev_col[i] = -1;
    hit->modal_hit_map.right_chev_col[i] = -1;
    // Scan the item text for ◀ (E2 97 80) and ▶ (E2 96 B6).
    // Each chevron occupies 1 display column. Item text renders
    // starting at modal-interior col 3, so the screen column is
    // (left + 3 + display_offset).
    if (items != NULL && items[i] != NULL) {
      const unsigned char *s = (const unsigned char *)items[i];
      int disp = 0;
      while (*s != '\0') {
        if (s[0] == 0xe2 && s[1] == 0x97 && s[2] == 0x80) {
          hit->modal_hit_map.left_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] == 0xe2 && s[1] == 0x96 && s[2] == 0xb6) {
          hit->modal_hit_map.right_chev_col[i] = left + 3 + disp;
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
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)plane_h;
    opts.cols = (unsigned)plane_w;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != plane_h || (int)cur_cols != plane_w) {
      ncplane_resize_simple(planes->modal, (unsigned)plane_h,
                            (unsigned)plane_w);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
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
                                                        "C"};
  // All modes are wired up. Any future unbuilt mode would render
  // dimmed with a "(coming soon)" tag and the cursor would skip past
  // it — flip its entry to true to do so.
  const bool item_disabled[TUI_STARTUP_ITEM_COUNT] = {
      false, false, false, false, false,
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
  TuiGridPlanes *planes = tui_grid_planes();
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
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)height;
    opts.cols = (unsigned)width;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != height || (int)cur_cols != width) {
      ncplane_resize_simple(planes->modal, (unsigned)height, (unsigned)width);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
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

void tui_play_setup_enabled_rows(UiOvertimeRule overtime_rule, int time_seconds,
                                 UiChallengeRule challenge_rule,
                                 bool out_enabled[TUI_PLAY_SETUP_ITEM_COUNT]) {
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    out_enabled[item_idx] = true;
  }
  if (challenge_rule != UI_CHALLENGE_PENALTY) {
    out_enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY] = false;
  }
  if (time_seconds <= 0) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME] = false;
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
    return;
  }
  if (overtime_rule != UI_OVERTIME_MAX) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
  }
  if (overtime_rule == UI_OVERTIME_FLAG) {
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
  }
}

void tui_game_render_play_setup(
    struct ncplane *plane, const Theme *theme, int focus,
    const char *human_name, const char *computer_name, int first_move,
    int name_edit_pos, int time_seconds, UiOvertimeRule overtime_rule,
    int overtime_cap_minutes, UiTimePenaltyRate time_penalty_rate,
    UiChallengeRule challenge_rule, UiChallengePenalty challenge_penalty,
    const char *language, const char *lexicon, int sim_plies,
    int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_PLAY_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_PLAY_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_starts[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_widths[TUI_PLAY_SETUP_ITEM_COUNT];
  bool enabled[TUI_PLAY_SETUP_ITEM_COUNT];
  bool disabled[TUI_PLAY_SETUP_ITEM_COUNT];
  tui_play_setup_enabled_rows(overtime_rule, time_seconds, challenge_rule,
                              enabled);
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    disabled[item_idx] = !enabled[item_idx];
  }
  const bool focus_human = (focus == TUI_PLAY_SETUP_HUMAN_NAME);
  const bool focus_comp = (focus == TUI_PLAY_SETUP_COMPUTER_NAME);

  format_setup_text_row(buf[TUI_PLAY_SETUP_HUMAN_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Your name",
                        human_name != NULL ? human_name : "");
  format_setup_text_row(buf[TUI_PLAY_SETUP_COMPUTER_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Computer name",
                        computer_name != NULL ? computer_name : "");
  const char *first_value = first_move == TUI_PLAY_FIRST_HUMAN      ? "Human"
                            : first_move == TUI_PLAY_FIRST_COMPUTER ? "Computer"
                                                                    : "Random";
  format_setup_row(buf[TUI_PLAY_SETUP_FIRST_MOVE], ROW_BUF, CONTENT_W,
                   "First move", first_value,
                   focus == TUI_PLAY_SETUP_FIRST_MOVE);

  // Time control — same preset resolution as the Watch-setup modal.
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
  format_setup_row(buf[TUI_PLAY_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus == TUI_PLAY_SETUP_TIME);

  // Overtime rule + its dependents. Disabled rows render their value
  // dimmed without the ◀ ▶ adjusters (the cap only matters under
  // "max overtime"; penalties don't exist under "flag at 0:00").
  const char *overtime_value =
      overtime_rule == UI_OVERTIME_FLAG  ? "flag at 0:00"
      : overtime_rule == UI_OVERTIME_MAX ? "max overtime"
                                         : "unlimited";
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME], ROW_BUF, CONTENT_W, "Overtime",
                   overtime_value,
                   focus == TUI_PLAY_SETUP_OVERTIME &&
                       enabled[TUI_PLAY_SETUP_OVERTIME]);
  // Disabled rows show a plain-ASCII "n/a" — format_setup_row pads by
  // byte length, so a multi-byte glyph (em dash) would right-align two
  // columns short.
  char cap_value[24];
  if (enabled[TUI_PLAY_SETUP_OVERTIME_CAP]) {
    snprintf(cap_value, sizeof(cap_value), "%d min", overtime_cap_minutes);
  } else {
    snprintf(cap_value, sizeof(cap_value), "n/a");
  }
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME_CAP], ROW_BUF, CONTENT_W,
                   "Overtime cap", cap_value,
                   focus == TUI_PLAY_SETUP_OVERTIME_CAP &&
                       enabled[TUI_PLAY_SETUP_OVERTIME_CAP]);
  const char *penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_TIME_PENALTY]) {
    penalty_value = time_penalty_rate == UI_TIME_PENALTY_1_PER_SEC
                        ? "1 pt/sec"
                        : "10 pts/min";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_TIME_PENALTY], ROW_BUF, CONTENT_W,
                   "Time penalty", penalty_value,
                   focus == TUI_PLAY_SETUP_TIME_PENALTY &&
                       enabled[TUI_PLAY_SETUP_TIME_PENALTY]);

  // Challenge rule + its penalty variant (the variant row only
  // applies under the "penalty" rule).
  const char *challenge_value =
      challenge_rule == UI_CHALLENGE_VOID     ? "void"
      : challenge_rule == UI_CHALLENGE_SINGLE ? "single"
      : challenge_rule == UI_CHALLENGE_DOUBLE ? "double"
                                              : "penalty";
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE], ROW_BUF, CONTENT_W,
                   "Challenge", challenge_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE);
  const char *challenge_penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]) {
    challenge_penalty_value =
        challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_PLAY    ? "5 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_10_PER_PLAY ? "10 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_WORD  ? "5 pts/word"
                                                                : "10 pts/word";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE_PENALTY], ROW_BUF, CONTENT_W,
                   "Challenge penalty", challenge_penalty_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE_PENALTY &&
                       enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]);

  format_setup_row(buf[TUI_PLAY_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
                   language != NULL && language[0] != '\0' ? language
                                                           : "(none)",
                   focus == TUI_PLAY_SETUP_LANGUAGE);
  format_setup_row(buf[TUI_PLAY_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus == TUI_PLAY_SETUP_LEXICON);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus == TUI_PLAY_SETUP_SIM_PLIES);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str,
                   focus == TUI_PLAY_SETUP_SIM_CANDIDATES);
  snprintf(buf[TUI_PLAY_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_PLAY_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  zone_starts[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_W;
  zone_starts[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_W;
  if (focus_human) {
    cursor_cols[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_comp) {
    cursor_cols[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Play vs computer", items, /*shortcuts=*/NULL,
                  disabled, cursor_cols, zone_starts, zone_widths,
                  TUI_PLAY_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
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

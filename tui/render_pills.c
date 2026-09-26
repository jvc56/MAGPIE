#include "render_pills.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "game_state.h"
#include "render_common.h"
#include "render_layout.h"
#include "render_view.h"
#include "theme.h"
#include "tile_input.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <string.h>

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
void draw_combined_pills_history_frame(struct ncplane *plane,
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
// Spectator-style pill: name, halfwidth rack inline, score, clock.
// Bounds (top, left, right) are taken from the Layout so pills can sit
// side-by-side as column headers in two-col mode or stack in one-col mode.
void render_player_pill(struct ncplane *plane, const Theme *theme,
                        const TuiGameState *state, int player_idx, int top,
                        int left, int right, bool halfwidth,
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
    (void)snprintf(name, sizeof(name), "%s", state->player_names[player_idx]);
  } else {
    (void)snprintf(name, sizeof(name), "P%d", player_idx + 1);
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
  (void)snprintf(score_str, sizeof(score_str), "%d",
                 pick_render_score(state, player_idx));
  // Clocks are only meaningful for live Watch games. In CGP / load
  // mode the bot worker isn't running and the displayed times
  // don't reflect any real time control, so we hide them and
  // right-align the score against the pill's right edge.
  const bool show_clock = state->bot_started;
  int score_col;
  if (show_clock) {
    const double remaining = pick_render_clock_seconds(state, player_idx);
    // Floor toward -inf so the first second past 0:00 already reads
    // "-0:01" instead of lingering on "0:00".
    int remaining_secs = (int)remaining;
    if (remaining < 0 && (double)remaining_secs != remaining) {
      remaining_secs--;
    }
    char clock_str[16];
    format_clock(remaining_secs, clock_str, sizeof(clock_str));
    const int clock_len = (int)strlen(clock_str);
    const int clock_col = content_right - clock_len + 1;
    // An overtime (negative) clock renders in the error color so the
    // player can't miss that penalties are accruing.
    if (remaining < 0) {
      theme_apply_fg(plane, theme->error_fg);
    } else {
      theme_apply_fg(plane, on_turn ? player_accent : theme->dim_fg);
    }
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
    const LetterDistribution *ld = state->ld;
    MachineLetter pill_slots[RACK_SIZE];
    const int pill_slot_count = sort_rack_for_display(
        rack, ld, state->rack_sort, pill_slots, RACK_SIZE);
    for (int i = 0;
         i < pill_slot_count && rcol + (tile_w - 1) <= rack_right_max; i++) {
      const MachineLetter ml = pill_slots[i];
      char face[TUI_TILE_TEXT_MAX];
      tile_face_cells(ld, ml, halfwidth, face, sizeof(face));
      ncplane_putstr_yx(plane, content_row, rcol, face);
      rcol += tile_w;
    }
  }
}

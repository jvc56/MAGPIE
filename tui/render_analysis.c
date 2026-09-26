#include "render_analysis.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/endgame.h"
#include "analysis_rows.h"
#include "game_state.h"
#include "render_common.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "render_view.h"
#include "theme.h"
#include "tui_ui_types.h"
#include <limits.h>
#include <notcurses/notcurses.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

enum {
  // Width of one per-ply average column, and the gap before it.
  AVG_COL_W = 4,
  AVG_GAP_W = 1,
  // Gap between the move text and the leave column.
  LEAVE_GAP_L = 2,
};

// Rendered width of a move: the panel draws moves with hide_parens, so
// parentheses take no cells.
static int analysis_move_width(const char *move) {
  int width = 0;
  for (const char *ch = move; *ch != '\0'; ch++) {
    if (*ch != '(' && *ch != ')') {
      width++;
    }
  }
  return width;
}

// Truncates `move` in place to at most `max_width` rendered cells
// (empty when max_width <= 0). Counting rendered width rather than
// strlen matters: "H2 ISOBU(TA)NE" has strlen 14 but renders as 12
// ("H2 ISOBUTANE"), so counting parens would crop it unnecessarily.
static void analysis_fit_move(char *move, int max_width) {
  if (max_width <= 0) {
    move[0] = '\0';
    return;
  }
  int width = 0;
  char *ch = move;
  for (; *ch != '\0'; ch++) {
    if (*ch != '(' && *ch != ')') {
      if (width >= max_width) {
        break;
      }
      width++;
    }
  }
  *ch = '\0';
}

// Compacts every "(exch ABCD)" to "-ABCD" in place: the verbose form
// takes too much horizontal room and the short form is unambiguous
// next to placement moves like "7F JUTE". Returns the widest rendered
// move among the valid rows afterwards (invalid rows aren't drawn).
static int analysis_compact_exchanges(AnalysisRow *rows, int count) {
  int max_width = 0;
  for (int i = 0; i < count; i++) {
    char *s = rows[i].move;
    if (strncmp(s, "(exch ", 6) == 0) {
      const char *close_paren = strchr(s, ')');
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
    const int move_width = analysis_move_width(s);
    if (rows[i].valid && move_width > max_width) {
      max_width = move_width;
    }
  }
  return max_width;
}

// Standard-layout column geometry, fixed once per frame before the
// header strip, rows, and scrollbar are drawn. Columns right-anchor:
// [rank][move]  [leave] [score][primary][secondary][avg1 avg2 ...]
typedef struct {
  int interior_left;
  int interior_right; // excludes the scrollbar cell when it shows
  int interior_top;
  int interior_bottom;
  int move_col;
  int max_leave_w;
  int primary_w;
  int secondary_w;
  int primary_secondary_gap;
  int prim_col;
  int sec_col;
  bool show_avgs;
  int max_ply_count;
  int avg_left_edge;
  bool show_score;
  int score_w;
  int score_right_edge;
  bool show_leaves;
  int leave_right_edge;
  // Widest move that fits when the leave column is hidden.
  int full_move_max;
} AnalysisColumns;

// Per-column maxima across the visible rows, so the renderer can bold
// the row(s) achieving each. Score is an integer (exact ties are legit
// and all bold); primary/secondary compare the raw doubles, so ties at
// the displayed precision still resolve to a unique winner when the
// underlying values differ.
typedef struct {
  int best_score;
  double best_primary;
  double best_secondary;
  bool any_primary;
  bool any_secondary;
  // best_ply_avg[k] is the maximum of avg column k across rows that ran
  // at least k+1 plies; any_ply_avg[k] gates against the all-empty case.
  double best_ply_avg[MAX_ANALYSIS_PLIES];
  bool any_ply_avg[MAX_ANALYSIS_PLIES];
} AnalysisBests;

static void analysis_find_bests(const AnalysisRow *rows, int visible,
                                AnalysisBests *bests) {
  bests->best_score = INT_MIN;
  bests->best_primary = -1e300;
  bests->best_secondary = -1e300;
  bests->any_primary = false;
  bests->any_secondary = false;
  for (int k = 0; k < MAX_ANALYSIS_PLIES; k++) {
    bests->best_ply_avg[k] = -1e300;
    bests->any_ply_avg[k] = false;
  }
  for (int i = 0; i < visible; i++) {
    if (!rows[i].valid) {
      continue;
    }
    if (rows[i].score[0] != '\0' && rows[i].score_value > bests->best_score) {
      bests->best_score = rows[i].score_value;
    }
    if (rows[i].primary[0] != '\0') {
      if (!bests->any_primary || rows[i].primary_value > bests->best_primary) {
        bests->best_primary = rows[i].primary_value;
        bests->any_primary = true;
      }
    }
    if (rows[i].secondary[0] != '\0') {
      if (!bests->any_secondary ||
          rows[i].secondary_value > bests->best_secondary) {
        bests->best_secondary = rows[i].secondary_value;
        bests->any_secondary = true;
      }
    }
    for (int k = 0; k < rows[i].ply_count && k < MAX_ANALYSIS_PLIES; k++) {
      if (!bests->any_ply_avg[k] ||
          rows[i].ply_avg[k] > bests->best_ply_avg[k]) {
        bests->best_ply_avg[k] = rows[i].ply_avg[k];
        bests->any_ply_avg[k] = true;
      }
    }
  }
}

// Compact-layout form of a primary value: percent values (containing
// '.') round to an integer percent; W/T/L stay as-is, minus padding.
static void analysis_compact_primary(const char *primary, char *buf,
                                     size_t buf_size) {
  if (strchr(primary, '.') != NULL) {
    const double val = strtod(primary, NULL);
    int int_pct = (int)(val + 0.5);
    if (int_pct > 100) {
      int_pct = 100;
    }
    if (int_pct < 0) {
      int_pct = 0;
    }
    (void)snprintf(buf, buf_size, "%d%%", int_pct);
  } else {
    const char *src = primary;
    while (*src == ' ') {
      src++;
    }
    (void)snprintf(buf, buf_size, "%s", src);
  }
}

// Fades a header strip in from the panel bg to the dim_fg band over
// columns fade_left..fade_right. Each cell renders a left-half-block ▌
// (U+258C): fg paints the cell's left half, bg the right half, giving
// two color samples per cell, so the fade eases in smoothly even on
// narrow strips.
static void render_analysis_header_fade(struct ncplane *plane,
                                        const Theme *theme, int row,
                                        int fade_left, int fade_right) {
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
    ncplane_putstr_yx(plane, row, c, "\xe2\x96\x8c"); // ▌ left-half block
  }
}

// Compact layout for a panel too narrow for the standard columns: the
// spread column is dropped, and rank, the space after it, and the leave
// column are added back in priority order as they fit, beside an integer
// win% (or W/T/L) under a "win%" header.
static void render_analysis_rows_compact(struct ncplane *plane,
                                         const Theme *theme, const Layout *L,
                                         AnalysisRow *rows, int visible,
                                         bool primary_bold, int title_end_col,
                                         int interior_right, int max_move_w,
                                         int max_leave_w, int rank_digits) {
  const int interior_left = L->analysis_left + 1;
  const int interior_top = L->analysis_top + 1;
  const int interior_bottom = L->analysis_bottom - 1;
  const int interior_width = interior_right - interior_left + 1;
  int list_top =
      interior_top <= interior_bottom ? interior_top + 1 : interior_top;
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
    char compact_primary[8];
    analysis_compact_primary(rows[i].primary, compact_primary,
                             sizeof(compact_primary));
    const int len_here = (int)strlen(compact_primary);
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
  const int level3_need = rank_full + max_move_w + LEAVE_GAP_L + max_leave_w +
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

  int compact_rank_w = rank_full;
  if (level == 0) {
    compact_rank_w = 0;
  } else if (level == 1) {
    compact_rank_w = rank_short;
  }
  const int compact_move_col = interior_left + compact_rank_w;
  const bool compact_show_leave = (level >= 3);
  char rfmt[8];
  if (level >= 1) {
    (void)snprintf(rfmt, sizeof(rfmt), level == 1 ? "%%%dd." : "%%%dd. ",
                   rank_digits);
  }

  // Compact mode also gets a "win%" header. Try the panel's top
  // border row first (sharing with the title) — if the title is
  // too long for that, fall back to the first interior row and
  // shift the data rows down by one.
  {
    int compact_header_row = -1;
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
        render_analysis_header_fade(plane, theme, compact_header_row, fade_left,
                                    fade_right);
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
    char pbuf[8];
    analysis_compact_primary(rows[i].primary, pbuf, sizeof(pbuf));
    const int plen = (int)strlen(pbuf);
    const int pcol = interior_right - plen + 1;

    if (level >= 1) {
      char rstr[8];
      (void)snprintf(rstr, sizeof(rstr), rfmt, i + 1);
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
          leave_text_col - LEAVE_GAP_L - compact_move_col;
      // Count this row's rendered move width.
      const int row_rendered = analysis_move_width(rows[i].move);
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
    analysis_fit_move(move_text, move_budget);
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
}

// Paints the column-header strip (leave / sc / avgN / win% / sprd) on the
// panel's top border when the title leaves room, else on the first
// interior row. Returns the first row available for data.
static int render_analysis_headers(struct ncplane *plane, const Theme *theme,
                                   const Layout *L,
                                   const AnalysisColumns *columns,
                                   int title_end_col, bool has_primary_label,
                                   const char *secondary_label) {
  // Find the leftmost col any header would touch (the "leave"
  // header is the leftmost; if no leave column, "sc" or "win%").
  int leftmost_header_col = INT_MAX;
  if (columns->max_leave_w > 0) {
    const int col = columns->leave_right_edge - 5 + 1; // "leave"
    if (col < leftmost_header_col) {
      leftmost_header_col = col;
    }
  }
  if (columns->show_score) {
    const int len = columns->score_w == 2 ? 2 : 3;
    const int col = columns->score_right_edge - len + 1;
    if (col < leftmost_header_col) {
      leftmost_header_col = col;
    }
  }
  // avg cols are right-anchored past sprd, so they don't affect
  // leftmost_header_col — they sit further right than every other
  // header.
  if (has_primary_label) {
    const int col = columns->prim_col + columns->primary_w - 4; // "win%"
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
  const int header_row =
      headers_fit_on_border ? L->analysis_top : columns->interior_top;
  const int list_top =
      headers_fit_on_border ? columns->interior_top : columns->interior_top + 1;

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
    const int band_left = leftmost_header_col;
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->dim_fg);
    for (int c = band_left; c <= columns->interior_right; c++) {
      ncplane_putstr_yx(plane, header_row, c, " ");
    }
  }
  if (!headers_fit_on_border && columns->interior_left < leftmost_header_col) {
    render_analysis_header_fade(plane, theme, header_row,
                                columns->interior_left,
                                leftmost_header_col - 1);
  }

  theme_apply_fg(plane, theme->bg);
  theme_apply_bg(plane, theme->dim_fg);
  ncplane_set_styles(plane, NCSTYLE_BOLD);
  if (columns->show_leaves) {
    const char *leave_label = "leave";
    const int len = (int)strlen(leave_label);
    const int col = columns->leave_right_edge - len + 1;
    if (col >= columns->move_col) {
      ncplane_putstr_yx(plane, header_row, col, leave_label);
    }
  }
  if (columns->show_score) {
    const char *sc_label = columns->score_w == 2 ? "sc" : "scr";
    const int len = (int)strlen(sc_label);
    const int col = columns->score_right_edge - len + 1;
    ncplane_putstr_yx(plane, header_row, col, sc_label);
  }
  if (columns->show_avgs) {
    for (int ply = 0; ply < columns->max_ply_count; ply++) {
      char hdr[8];
      (void)snprintf(hdr, sizeof(hdr), "avg%d", ply + 1);
      const int col =
          columns->avg_left_edge + ply * (AVG_COL_W + AVG_GAP_W) + AVG_GAP_W;
      ncplane_putstr_yx(plane, header_row, col, hdr);
    }
  }
  if (has_primary_label) {
    const char *win_label = "win%";
    const int len = (int)strlen(win_label);
    const int col = columns->prim_col + columns->primary_w - len;
    ncplane_putstr_yx(plane, header_row, col, win_label);
  }
  if (secondary_label != NULL) {
    const int len = (int)strlen(secondary_label);
    // Right-align inside the secondary column's slot, not against
    // interior_right — when the avg block is on, sec_col has
    // shifted left to make room for the avgs further right.
    const int col = columns->sec_col + columns->secondary_w - len;
    ncplane_putstr_yx(plane, header_row, col, secondary_label);
  }
  ncplane_set_styles(plane, 0);
  return list_top;
}

// Draws candidate rows[data_i] on screen row `row`: the rank (or cursor
// chip), move, leave, score, per-ply averages, primary, and secondary,
// bolding column bests, and records the row's click rectangle.
static void render_analysis_row(struct ncplane *plane, const Theme *theme,
                                const TuiGameState *state, AnalysisRow *rows,
                                const AnalysisColumns *columns,
                                bool primary_bold, TuiHitMaps *hit, int row,
                                const char *rank_fmt,
                                const AnalysisBests *bests,
                                int effective_cursor, int data_i) {
  char rank_str[8];
  (void)snprintf(rank_str, sizeof(rank_str), rank_fmt, data_i + 1);

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
      columns->show_leaves
          ? columns->leave_right_edge -
                (rows[data_i].leave[0] != '\0' ? leave_len : 1) + 1
          : 0;
  const bool show_this_leave = columns->show_leaves;
  const int this_move_max =
      columns->show_leaves ? columns->leave_right_edge - LEAVE_GAP_L -
                                 columns->move_col + 1 - columns->max_leave_w
                           : columns->full_move_max;

  char *move_text = rows[data_i].move;
  analysis_fit_move(move_text, this_move_max);

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
          (uint8_t)((30U * tile_bg.r + 59U * tile_bg.g + 11U * tile_bg.b) /
                    100U);
      const ThemeRgb gray_bg = {lum, lum, lum};
      theme_apply_fg(plane, theme->fg);
      theme_apply_bg(plane, gray_bg);
    }
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, columns->interior_left, chip);
    ncplane_set_styles(plane, 0);
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    if (rank_str[after_period_idx] != '\0') {
      ncplane_putstr_yx(plane, row, columns->interior_left + after_period_idx,
                        rank_str + after_period_idx);
    }
  } else {
    theme_apply_fg(plane, theme->dim_fg);
    ncplane_putstr_yx(plane, row, columns->interior_left, rank_str);
  }
  // Record the row's screen rectangle for click-to-cursor.
  if (hit->analysis_row_map_count <
      (int)(sizeof(hit->analysis_row_map) / sizeof(hit->analysis_row_map[0]))) {
    AnalysisRowMap *m = &hit->analysis_row_map[hit->analysis_row_map_count++];
    m->top_row = row;
    m->bottom_row = row;
    m->left_col = columns->interior_left;
    m->right_col = columns->interior_right;
    m->move_left_col = columns->move_col;
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
        ncplane_putstr_yx(plane, row, columns->move_col + c, " ");
      }
      ncplane_putstr_yx(plane, row, columns->move_col, move_text);
      ncplane_set_styles(plane, 0);
      theme_apply_bg(plane, theme->bg);
    } else {
      theme_apply_fg(plane, theme->fg);
      render_move_styled(plane, row, columns->move_col, move_text,
                         /*hide_parens=*/true,
                         /*hide_playthrough_parens=*/false);
    }
  }

  if (show_this_leave) {
    theme_apply_fg(plane, theme->dim_fg);
    ncplane_putstr_yx(plane, row, leave_text_col, leave_str);
  }

  if (columns->show_score && rows[data_i].score[0] != '\0') {
    const int sl = (int)strlen(rows[data_i].score);
    const int sc_col = columns->score_right_edge - sl + 1;
    const bool is_best = (rows[data_i].score_value == bests->best_score);
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
  if (columns->show_avgs && rows[data_i].ply_count > 0) {
    const int candidate_idx = rows[data_i].candidate_player_idx;
    for (int ply = 0; ply < rows[data_i].ply_count; ply++) {
      const int ply_player = (candidate_idx + ply) % 2;
      const ThemeRgb ply_color =
          ply_player == 1 ? theme->on_turn_fg_p2 : theme->on_turn_fg;
      const int col =
          columns->avg_left_edge + ply * (AVG_COL_W + AVG_GAP_W) + AVG_GAP_W;
      // Pick "12" vs "12.3" so the value fits in AVG_COL_W cells.
      char buf[16];
      const double v = rows[data_i].ply_avg[ply];
      if (v >= 100.0 || v <= -10.0) {
        (void)snprintf(buf, sizeof(buf), "%*.0f", AVG_COL_W, v);
      } else {
        (void)snprintf(buf, sizeof(buf), "%*.1f", AVG_COL_W, v);
      }
      const bool is_best = ply < MAX_ANALYSIS_PLIES &&
                           bests->any_ply_avg[ply] &&
                           v == bests->best_ply_avg[ply];
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
    const int col = columns->sec_col - columns->primary_secondary_gap - len;
    theme_apply_fg(plane, theme->fg);
    const bool is_best = bests->any_primary &&
                         rows[data_i].primary[0] != '\0' &&
                         rows[data_i].primary_value == bests->best_primary;
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
    const int col = columns->sec_col + columns->secondary_w - len;
    const bool is_best = bests->any_secondary &&
                         rows[data_i].secondary[0] != '\0' &&
                         rows[data_i].secondary_value == bests->best_secondary;
    theme_apply_fg(plane, theme->dim_fg);
    if (is_best) {
      ncplane_set_styles(plane, NCSTYLE_BOLD);
    }
    ncplane_putstr_yx(plane, row, col, rows[data_i].secondary);
    if (is_best) {
      ncplane_set_styles(plane, 0);
    }
  }
}

// Draws the scrollbar in the cell right of the interior, spanning the
// view_h list rows: a thumb sized view_h/total_rows with 1/8-row edges,
// and publishes its geometry for the input handlers' hit tests.
static void render_analysis_scrollbar(struct ncplane *plane, const Theme *theme,
                                      TuiGameState *state,
                                      const AnalysisColumns *columns,
                                      int total_rows, int list_top, int view_h,
                                      int scroll_offset) {
  const int scrollbar_col = columns->interior_right + 1;
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
      ncplane_putstr_yx(plane, track_top + r, scrollbar_col, lower_blocks[idx]);
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
  atomic_store(&state->analysis_scrollbar_top, track_top);
  atomic_store(&state->analysis_scrollbar_bottom, track_bottom);
  atomic_store(&state->analysis_scrollbar_col, scrollbar_col);
  atomic_store(&state->analysis_scrollbar_total, total_rows);
  atomic_store(&state->analysis_scrollbar_view, view_h);
}

// Render the ranked candidates given a pre-populated row array.
// Handles the leave column auto-sizing, exchange compaction, and
// right-anchored primary/secondary columns. primary_bold gates whether
// the primary string renders in bold (true for win%, false for W/T/L
// which already pop visually).
static void render_analysis_rows(struct ncplane *plane, const Theme *theme,
                                 TuiGameState *state, const Layout *L,
                                 AnalysisRow *rows, int visible, int primary_w,
                                 int secondary_w, int primary_secondary_gap,
                                 bool primary_bold, int title_end_col,
                                 const char *secondary_label) {
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
  (void)snprintf(rank_fmt, sizeof(rank_fmt), "%%%dd. ", rank_digits);
  const int rank_w = rank_digits + 2; // digits + ". "
  const int move_col = interior_left + rank_w;

  // No explicit gap between the leave column and the primary column —
  // the win% format ("%5.1f%%") leaves an implicit leading space
  // unless the value hits exactly 100.0%, which gives a clean 1-col
  // visual gap from the leave for any realistic win percentage.
  const int leave_gap_r = 0;

  int max_leave_w = 0;
  for (int i = 0; i < visible; i++) {
    if (!rows[i].valid) {
      continue;
    }
    const int ll = (int)strlen(rows[i].leave);
    if (ll > max_leave_w) {
      max_leave_w = ll;
    }
  }
  // Moves render with hide_parens=true, so layout uses the rendered
  // width (strlen minus parens): raw strlen overestimates and gets the
  // leave column suppressed when a playthrough move's parens push the
  // max over the budget. Exchanges are compacted first, since that
  // form is what renders.
  const int max_move_w = analysis_compact_exchanges(rows, visible);

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
    render_analysis_rows_compact(plane, theme, L, rows, visible, primary_bold,
                                 title_end_col, interior_right, max_move_w,
                                 max_leave_w, rank_digits);
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
    const int s = (int)strtol(rows[i].score, NULL, 10);
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

  // Bold the row(s) that achieve each column's maximum.
  AnalysisBests bests;
  analysis_find_bests(rows, visible, &bests);
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
      leave_right_edge - max_leave_w - LEAVE_GAP_L - move_col + 1;
  const bool show_leaves =
      max_leave_w > 0 && move_budget_with_leaves >= max_move_w;
  const int full_move_max =
      (show_score ? score_left_edge - 2 : prim_col - 1) - move_col;

  const AnalysisColumns columns = {
      .interior_left = interior_left,
      .interior_right = interior_right,
      .interior_top = interior_top,
      .interior_bottom = interior_bottom,
      .move_col = move_col,
      .max_leave_w = max_leave_w,
      .primary_w = primary_w,
      .secondary_w = secondary_w,
      .primary_secondary_gap = primary_secondary_gap,
      .prim_col = prim_col,
      .sec_col = sec_col,
      .show_avgs = show_avgs,
      .max_ply_count = max_ply_count,
      .avg_left_edge = avg_left_edge,
      .show_score = show_score,
      .score_w = score_w,
      .score_right_edge = score_right_edge,
      .show_leaves = show_leaves,
      .leave_right_edge = leave_right_edge,
      .full_move_max = full_move_max,
  };

  // Column headers above the data rows. We render the strip whenever
  // at least one label has something to say. Sim mode lights up
  // every header (leave / sc / win% / sprd / avg…); play-only mode
  // (loaded GCG with no sim results — primary_w == secondary_w == 0)
  // shows just leave + sc; endgame mode (primary_w == 0,
  // secondary_w == 4) suppresses headers entirely because its
  // W/T/L + spread labels don't fit the strip's style. Each header
  // right-aligns at the same edge as the column it labels.
  const bool has_primary_label = primary_w >= 4;
  // The secondary column's header ("sprd", or "eq" for a static
  // ranking); NULL leaves it unlabeled.
  const char *secondary_header = secondary_w >= 4 ? secondary_label : NULL;
  const bool has_secondary_label = secondary_header != NULL;
  // The strip goes on the panel's top border (sharing the row with the
  // title) when there's room, else on the first interior row; the
  // on-border placement gives the data one extra row.
  const bool show_headers = interior_top <= interior_bottom &&
                            (has_primary_label || has_secondary_label ||
                             show_score || max_leave_w > 0);
  const int list_top =
      show_headers
          ? render_analysis_headers(plane, theme, L, &columns, title_end_col,
                                    has_primary_label, secondary_header)
          : interior_top;

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
    state->analysis_scroll_offset = scroll_offset;
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
    render_analysis_row(plane, theme, state, rows, &columns, primary_bold, hit,
                        row, rank_fmt, &bests, effective_cursor, data_i);

    row++;
  }
  if (state != NULL) {
    atomic_store(&state->analysis_visible_rows, hit->analysis_row_map_count);
  }

  // Scrollbar — right edge of the panel interior. Renders a track
  // across the scroll-window rows with a thumb proportional to
  // visible/total, using LEFT N/8 BLOCK chars on the track and
  // LOWER N/8 BLOCK chars for the thumb's fractional top/bottom
  // edges (gives ~1/8-row visual precision).
  if (scrollbar_visible && state != NULL && view_h > 0 && total_rows > 0) {
    render_analysis_scrollbar(plane, theme, state, &columns, total_rows,
                              list_top, view_h, scroll_offset);
  } else if (state != NULL) {
    // Hidden scrollbar — publish zero so input handlers know not
    // to try to hit-test against stale geometry.
    atomic_store(&state->analysis_scrollbar_total, 0);
    atomic_store(&state->analysis_scrollbar_view, 0);
  }
}
void render_analysis_panel(struct ncplane *plane, const Theme *theme,
                           TuiGameState *state, const Layout *L) {
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
  // A "/kibitz" static ranking: move, leave, score, and equity.
  const bool use_static = snap != NULL && snap->is_static;
  const bool use_endgame = snap != NULL
                               ? (!use_static && !snap->is_sim && !snap->is_peg)
                               : (bag_empty && state->endgame_snapshot.valid &&
                                  state->endgame_snapshot.num_entries > 0);
  const bool use_peg =
      snap != NULL ? (!use_static && snap->is_peg)
                   : (!use_endgame && tui_position_in_peg_range(src_game) &&
                      state->peg_poll != NULL &&
                      atomic_load(&state->peg_results_turn_idx) >= 0);

  // Title varies by mode.
  char title[64];
  if (use_static) {
    // "Static · played #4 (-6.2)" — where the played move ranks.
    if (snap->static_played_rank > 0) {
      (void)snprintf(title, sizeof(title), "Static \xc2\xb7 played #%d (%+.1f)",
                     snap->static_played_rank,
                     -snap->static_played_equity_loss);
    } else {
      (void)snprintf(title, sizeof(title), "Static");
    }
  } else if (use_peg) {
    // "PEG (2p 5/16)" — fidelity (plies) of the ranking shown, plus
    // done/field progress through the current stage. The live meta
    // was refreshed by this frame's row build from the same poll
    // snapshot the rows came from.
    if (snap != NULL) {
      if (snap->peg_fidelity > 0) {
        (void)snprintf(title, sizeof(title), "PEG (%dp%s)", snap->peg_fidelity,
                       snap->peg_done ? "" : " partial");
      } else {
        (void)snprintf(title, sizeof(title), "PEG");
      }
    } else {
      const TuiPegLiveMeta *meta = &state->peg_live_meta;
      const bool searching = atomic_load(&state->peg_results_active);
      if (meta->valid && searching && meta->field_size > 0) {
        // Fidelity 0 is the greedy seed stage — label it instead of
        // showing a meaningless "0p".
        if (meta->fidelity > 0) {
          (void)snprintf(title, sizeof(title), "PEG (%dp %d/%d)",
                         meta->fidelity, meta->cands_done, meta->field_size);
        } else {
          (void)snprintf(title, sizeof(title), "PEG (seed %d/%d)",
                         meta->cands_done, meta->field_size);
        }
      } else if (meta->valid && meta->fidelity > 0) {
        (void)snprintf(title, sizeof(title), "PEG (%dp)", meta->fidelity);
      } else if (searching) {
        (void)snprintf(title, sizeof(title), "PEG (starting\xe2\x80\xa6)");
      } else {
        (void)snprintf(title, sizeof(title), "PEG");
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
      searching = atomic_load(&state->endgame_results_active);
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
      (void)snprintf(title, sizeof(title), "Endgame (d%d/%s)", depth_to_show,
                     nodes_str);
    } else if (depth_to_show > 0) {
      (void)snprintf(title, sizeof(title), "Endgame (d%d)", depth_to_show);
    } else if (searching) {
      (void)snprintf(title, sizeof(title), "Endgame (starting\xe2\x80\xa6)");
    } else {
      (void)snprintf(title, sizeof(title), "Endgame");
    }
  } else if (snap != NULL) {
    if (snap->sim_plies > 0 && snap->sim_iterations > 0) {
      char nodes_str[16];
      format_count_compact(snap->sim_nodes, nodes_str, sizeof(nodes_str));
      (void)snprintf(title, sizeof(title), "Sim (%dp/%s)", snap->sim_plies,
                     nodes_str);
    } else {
      (void)snprintf(title, sizeof(title), "Sim");
    }
  } else if (state->sim_results != NULL) {
    const int sim_turn_idx_title = atomic_load(&state->sim_results_turn_idx);
    const int plies = sim_results_get_num_plies(state->sim_results);
    const uint64_t iters = sim_results_get_iteration_count(state->sim_results);
    if (sim_turn_idx_title >= 0 && plies > 0 && iters > 0) {
      // Report node count instead of sample count so the unit lines
      // up with what Endgame reports. Each iteration visits the root
      // plus `plies` plies, so nodes ≈ iters * (plies + 1).
      const uint64_t nodes = iters * (uint64_t)(plies + 1);
      char nodes_str[16];
      format_count_compact(nodes, nodes_str, sizeof(nodes_str));
      (void)snprintf(title, sizeof(title), "Sim (%dp/%s)", plies, nodes_str);
    } else {
      // sim_results exists but no real sim ran (e.g. loaded GCG
      // viewer or post-reset state). The panel is in fallback
      // "just-the-played-move" mode, so it's a Plays log rather
      // than analysis output.
      (void)snprintf(title, sizeof(title), "Plays");
    }
  } else {
    (void)snprintf(title, sizeof(title), "Plays");
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
  const int sim_turn_idx = atomic_load(&state->sim_results_turn_idx);
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
  if (use_static) {
    primary_w = 0;             // no win% column
    secondary_w = 6;           // "%+.1f" equity, e.g. " +32.5"
    primary_secondary_gap = 0; // the leading pad provides the gap
    primary_bold = false;
  } else if (use_endgame) {
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
                       title_end_col, use_static ? "eq" : "sprd");
}

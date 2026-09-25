#include "render_layout.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "render_view.h"
#include <string.h>

int compute_effective_scale(int user_pref, unsigned plane_cols,
                            unsigned plane_rows) {
  static const int cell_w_for[3] = {1, 2, 4};
  static const int cell_h_for[3] = {1, 1, 2};
  if (user_pref < 0) {
    user_pref = 0;
  } else if (user_pref > 2) {
    user_pref = 2;
  }
  // Account for the chrome rows compute_layout reserves at the
  // bottom: command bar (always) + status bar (always). The pending
  // banner is ignored here — if it shows up later the panels just
  // get one row tighter, which is fine and visually unchanged.
  if (plane_rows >= 2) {
    plane_rows -= 1;
  }
  for (int s = user_pref; s >= 0; s--) {
    const int cols = CELL_COL_BASE + BOARD_DIM * cell_w_for[s] +
                     BOARD_RIGHT_BORDER + RIGHT_COL_LEFT_OFFSET +
                     RIGHT_COL_MIN_WIDTH;
    // Board widget = top border (1) + col-label row (1) + cells + bottom
    // border (1). Then rack box, bag min 3 rows, status 1. Rack sits
    // flush under the board's bottom border, no gap.
    const int rack_box_rows = (s == 2) ? 4 : 3;
    const int rows = BOARD_DIM * cell_h_for[s] + 3 + rack_box_rows + 3 + 1;
    if (plane_cols >= (unsigned)cols && plane_rows >= (unsigned)rows) {
      return s;
    }
  }
  return -1;
}
// Display-column count of the dense bag/unseen-tile string the bag
// panel renders. Mirrors render_bag_panel's content building; used by
// compute_layout to size the bag tightly when the analysis panel is
// taking its overflow.
static int bag_unseen_chars(const TuiGameState *state) {
  if (state == NULL || state->game == NULL) {
    return 0;
  }
  const Bag *bag = game_get_bag(state->game);
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
  // Each grouping is "<n copies of letter>"; groupings are separated
  // by a single space. Letters are ASCII (1 col each) for the lexica
  // we ship; non-ASCII would over-count visual width slightly but the
  // bag would just end up a row taller, which is fine.
  int total = 0;
  bool first = true;
  for (int ml = 0; ml < ld_size; ml++) {
    if (counts[ml] == 0) {
      continue;
    }
    if (!first) {
      total += 1; // separator space
    }
    first = false;
    total += counts[ml];
  }
  return total;
}
// Maximum row usage across the two history columns. Used to decide
// whether history fits without scrolling so we can give the surplus
// space to the analysis panel.
static int history_two_col_rows_needed(const TuiGameState *state) {
  if (state == NULL) {
    return 0;
  }
  int left = 0;
  int right = 0;
  for (int idx = 0; idx < state->history_count; idx++) {
    const int rows = (state->history[idx].end_bonus != 0 ||
                      state->history[idx].challenged_off)
                         ? 4
                         : 2; // mirrors history_entry_rows
    if (idx % 2 == 0) {
      left += rows;
    } else {
      right += rows;
    }
  }
  return left > right ? left : right;
}
// True when the bag the user is currently looking at is empty.
// Mirrors render_bag_panel's formula (ld_total − on_board −
// on_turn_rack) so the layout's bag-collapse decision matches
// what the renderer will actually paint.
static bool cursor_view_bag_empty(const TuiGameState *state) {
  if (state == NULL || state->ld == NULL) {
    return true;
  }
  const Board *render_board = pick_render_board(state);
  const TuiHistoryEntry *hview = pick_history_view(state);
  const Rack *on_turn_rack = NULL;
  if (hview != NULL && hview->rack_before != NULL) {
    on_turn_rack = hview->rack_before;
  } else if (state->game != NULL) {
    on_turn_rack = player_get_rack(game_get_player(
        state->game, game_get_player_on_turn_index(state->game)));
  }
  int board_counts[64] = {0};
  if (render_board != NULL) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        MachineLetter ml = board_get_letter(render_board, row, col);
        if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
          continue;
        }
        if (ml & BLANK_MASK) {
          ml = 0;
        }
        if (ml < (int)(sizeof(board_counts) / sizeof(int))) {
          board_counts[ml]++;
        }
      }
    }
  }
  const int ld_size = ld_get_size(state->ld);
  for (int ml = 0;
       ml < ld_size && ml < (int)(sizeof(board_counts) / sizeof(int)); ml++) {
    const int dist = ld_get_dist(state->ld, (MachineLetter)ml);
    const int on_rack = on_turn_rack != NULL
                            ? rack_get_letter(on_turn_rack, (MachineLetter)ml)
                            : 0;
    int v = dist - board_counts[ml] - on_rack;
    if (v < 0) {
      v = 0;
    }
    if (v > 0) {
      return false;
    }
  }
  return true;
}
Layout compute_layout(struct ncplane *plane, int user_scale,
                      const TuiGameState *state) {
  Layout L = {0};
  ncplane_dim_yx(plane, &L.plane_rows, &L.plane_cols);

  L.status_row = (int)L.plane_rows - 1;
  L.command_bar_row = L.status_row - 1;
  // Pending-change banner: rendered above the command bar when the
  // user's Settings have outpaced the live game's loaded tables.
  // Content panels treat content_bottom_row as their lowest available
  // row regardless of which path is active.
  const bool pending_changes =
      state != NULL &&
      (strcmp(state->pending_lexicon, state->active_lexicon) != 0 ||
       state->pending_load_rit != state->active_load_rit);
  L.pending_row = pending_changes ? L.command_bar_row - 1 : -1;
  L.content_bottom_row =
      (L.pending_row >= 0 ? L.pending_row : L.command_bar_row) - 1;

  L.scale = compute_effective_scale(user_scale, L.plane_cols, L.plane_rows);
  if (L.scale < 0) {
    // Caller checks L.scale < 0 and renders the too-small message;
    // the rest of the fields stay zeroed and unused.
    return L;
  }
  L.board_cell_w = (L.scale == 2) ? 4 : (L.scale == 1) ? 2 : 1;
  L.board_cell_h = (L.scale == 2) ? 2 : 1;
  // board_width spans cols [0, board_width-1], with col 0 the left
  // border and col board_width-1 the right border.
  L.board_width =
      CELL_COL_BASE + BOARD_DIM * L.board_cell_w + BOARD_RIGHT_BORDER;
  L.board_bottom_row = CELL_ROW_BASE + BOARD_DIM * L.board_cell_h - 1;

  L.board_right_col = L.board_width - 1;
  // Skip the bottom-border row of the board widget; rack starts on the
  // row after that.
  L.rack_top = L.board_bottom_row + 2;
  // Rack box gains a row when the board is double-size so the rack
  // tiles (also rendered at 4×2 cells) get the vertical room they need.
  const int rack_box_rows = (L.scale == 2) ? 4 : 3;
  L.rack_bottom = L.rack_top + rack_box_rows - 1;

  L.right_col_left = L.board_width + RIGHT_COL_LEFT_OFFSET;
  L.right_col_right = (int)L.plane_cols - 1;
  L.right_col_width = L.right_col_right - L.right_col_left + 1;

  // Decide where the analysis panel lives based on right-column width.
  // Two-col history is allowed as long as halfwidth pills fit; the
  // analysis-three-col split still gates on the fullwidth threshold
  // because the leftover slice still has to be wide enough to host a
  // useful analysis panel.
  L.analysis_placement = ANALYSIS_NONE;
  // Play-vs-computer hides the Analysis panel during the game so the
  // human can't see the bot's candidate plays (which would reveal its
  // rack). The panel returns once the game is over, for post-game study.
  const bool pvc_in_progress =
      state != NULL && state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
      state->game != NULL && !tui_game_state_play_over(state);
  if (state != NULL && !pvc_in_progress) {
    if (L.right_col_width >= ANALYSIS_THREE_COL_THRESHOLD) {
      L.analysis_placement = ANALYSIS_RIGHT_OF_HISTORY;
    } else if (L.right_col_width >= HISTORY_TWO_COL_HALFWIDTH_THRESHOLD) {
      L.analysis_placement = ANALYSIS_BELOW_HISTORY;
    } else {
      L.analysis_placement = ANALYSIS_BELOW_BAG;
    }
  }

  // Three-column case: peel the analysis strip off the right of the
  // right column BEFORE computing pill geometry. Instead of giving
  // history a fixed slab and letting analysis have whatever's left,
  // size history just wide enough for its actual content (longest
  // history entry's move row) — wider analysis is much more useful
  // than wider history.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    // Minimum interior per history column: enough for a halfwidth
    // pill ("▶ P1 ABCDEFG 999 99:59" ≈ 22 cols). History entry
    // content (move row: "N. <move> +score") sometimes runs wider —
    // we walk the history and take the max so column-widths grow
    // (never shrink below the pill minimum) to fit the data.
    int col_interior = 22;
    if (state != NULL) {
      for (int idx = 0; idx < state->history_count; idx++) {
        const TuiHistoryEntry *e = &state->history[idx];
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%d. ", idx + 1);
        char delta[16];
        snprintf(delta, sizeof(delta), "+%d", e->score);
        // Row 1 width: "N. <move>" + 1-col gap + "+<score>".
        const int row1 = (int)strlen(prefix) + (int)strlen(e->move_str) + 1 +
                         (int)strlen(delta);
        if (row1 > col_interior) {
          col_interior = row1;
        }
        // Row 2 width: indent (== prefix len) + clock(4-5) + space +
        // rack(up to 7) + 1 gap + running total.
        char total[16];
        snprintf(total, sizeof(total), "%d", e->total_after);
        const int rack_len = e->rack_str[0] ? (int)strlen(e->rack_str) : 1;
        const int clock_len = 5; // worst case "99:59"
        const int row2 = (int)strlen(prefix) + clock_len + 1 + rack_len + 1 +
                         (int)strlen(total);
        if (row2 > col_interior) {
          col_interior = row2;
        }
      }
    }
    // History panel total: 2 columns + 1-col divider + 2 outer borders.
    const int hist_w = 2 * col_interior + 1 + 2;
    // Cap so analysis still gets at least ANALYSIS_MIN_WIDTH.
    const int max_hist =
        L.right_col_width - ANALYSIS_GUTTER - ANALYSIS_MIN_WIDTH;
    const int actual_hist = hist_w > max_hist ? max_hist
                            : hist_w < HISTORY_TWO_COL_HALFWIDTH_THRESHOLD
                                ? HISTORY_TWO_COL_HALFWIDTH_THRESHOLD
                                : hist_w;
    const int analysis_width =
        L.right_col_width - actual_hist - ANALYSIS_GUTTER;
    L.analysis_right = L.right_col_right;
    L.analysis_left = L.right_col_right - analysis_width + 1;
    L.right_col_right = L.analysis_left - 1 - ANALYSIS_GUTTER;
    L.right_col_width = L.right_col_right - L.right_col_left + 1;
  }

  L.two_col = L.right_col_width >= HISTORY_TWO_COL_HALFWIDTH_THRESHOLD;
  // Even at a width that nominally fits two columns, a single
  // entry's move + leave + delta combo can be longer than the
  // half-column interior. When that happens the move text runs
  // into the leave column and produces visible character overlap.
  // Downgrade to single-column mode in that case — each row gets
  // ~2× the width and the overlap clears. Zero-gap collisions are
  // fine (color styling differentiates them); we only flip when a
  // truly negative gap would be needed.
  if (L.two_col && state != NULL && state->history_count > 0) {
    const int gutter = 1;
    const int half_width = (L.right_col_width - 2 - gutter) / 2;
    const int per_col_interior_w = half_width - 2; // minus side borders
    int rank_digits = 1;
    for (int n = state->history_count; n >= 10; n /= 10) {
      rank_digits++;
    }
    const int prefix_w = rank_digits + 2; // digits + ". "
    // 4 cells reserved on the right for the delta column (a 3-char
    // "+NN" + a 1-char gap). Same reservation render_history_entry
    // uses when placing the leave.
    const int delta_room = 4;
    for (int i = 0; i < state->history_count; i++) {
      const TuiHistoryEntry *e = &state->history[i];
      if (e->pending) {
        continue;
      }
      int move_w = 0;
      for (const char *p = e->move_str; *p != '\0'; p++) {
        if (*p != '(' && *p != ')') {
          move_w++;
        }
      }
      const int leave_w =
          e->leave_str[0] != '\0' ? (int)strlen(e->leave_str) : 1;
      if (move_w + leave_w + delta_room + prefix_w > per_col_interior_w) {
        L.two_col = false;
        break;
      }
    }
  }
  L.pills_halfwidth =
      L.two_col && L.right_col_width < HISTORY_TWO_COL_THRESHOLD;

  if (L.two_col) {
    // Pills + history share one combined box. The vertical divider
    // between the two columns runs through both the pill row and
    // the history rows; the divider row between pill content and
    // history is the pills' bottom border (├─┼─┤). Geometry:
    //   row 0:   ┌─...─┬─...─┐   (combined top border)
    //   row 1:   │pill1 │pill2│   (pill content)
    //   row 2:   ├─...─┼─...─┤   (horizontal divider == pill bottom)
    //   rows 3+: │hist1 │hist2│
    //   bot:     └─...─┴─...─┘
    const int gutter = 1; // box-vertical, drawn by the combined frame
    const int half_width = (L.right_col_width - 2 - gutter) / 2;
    L.combined_pills_history = true;
    L.divider_col = L.right_col_left + 1 + half_width;
    L.pill1_top = 0;
    L.pill1_bottom = PILL_HEIGHT - 1; // == row 2, the divider row
    L.pill1_left = L.right_col_left;
    L.pill1_right = L.divider_col;
    L.pill2_top = L.pill1_top;
    L.pill2_bottom = L.pill1_bottom;
    L.pill2_left = L.divider_col;
    L.pill2_right = L.right_col_right;
    L.history_top = L.pill1_bottom; // shared divider row
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
  L.history_bottom = L.content_bottom_row;

  // Default bag region: rack_bottom+1 to content_bottom_row. Tightened
  // below in the BELOW_BAG case so analysis can sit beneath the bag,
  // and also when the bag is fully empty — in that case
  // render_bag_panel collapses to a single divider line and we
  // shouldn't waste rows on a blank box.
  L.bag_top = L.rack_bottom + 1;
  L.bag_bottom = L.content_bottom_row;
  // Follow the cursor view so scrolling back to a mid-game turn
  // in a finished game restores the bag panel.
  const bool bag_empty = cursor_view_bag_empty(state);
  if (bag_empty) {
    L.bag_bottom = L.bag_top; // single-row divider
  }

  // Three-column: analysis fills the right strip from the very top of
  // the plane down to the status bar. There's nothing else over there,
  // so there's no reason to align with the pills row.
  if (L.analysis_placement == ANALYSIS_RIGHT_OF_HISTORY) {
    L.analysis_top = 0;
    L.analysis_bottom = L.content_bottom_row;
    L.has_analysis = (L.analysis_bottom - L.analysis_top + 1) >= 3;
  }

  // Two-column: carve rows off the bottom of history for analysis.
  // If history fits without scrolling, hand all the surplus to
  // analysis; otherwise reserve a fixed slab.
  if (L.analysis_placement == ANALYSIS_BELOW_HISTORY) {
    const int interior_rows = L.history_bottom - L.history_top - 1;
    const int needed = history_two_col_rows_needed(state);
    int analysis_rows;
    if (needed <= interior_rows) {
      analysis_rows = interior_rows - needed;
    } else {
      analysis_rows = ANALYSIS_DEFAULT_ROWS;
    }
    if (analysis_rows >= 3 &&
        L.history_bottom - analysis_rows > L.history_top) {
      L.analysis_top = L.history_bottom - analysis_rows + 1;
      L.analysis_bottom = L.history_bottom;
      L.analysis_left = L.right_col_left;
      L.analysis_right = L.right_col_right;
      L.history_bottom = L.analysis_top - 1;
      L.has_analysis = true;
    }
  }

  // Single-column history: analysis sits below the bag in the LEFT
  // column. Bag shrinks to its content height (plus borders + tally),
  // and analysis claims everything else above the status bar.
  if (L.analysis_placement == ANALYSIS_BELOW_BAG) {
    int bag_height;
    if (bag_empty) {
      // The empty-bag divider only needs one row.
      bag_height = 1;
    } else {
      const int interior_width = L.board_width - 2;
      const int chars = bag_unseen_chars(state);
      int content_lines = interior_width > 0
                              ? (chars + interior_width - 1) / interior_width
                              : 1;
      if (content_lines < 1) {
        content_lines = 1;
      }
      // 2 borders + content rows + 1 tally row.
      bag_height = 2 + content_lines + 1;
    }
    // Floor 1 instead of 0 in case the math collapses; we still need
    // at least the bag's own row before the analysis starts.
    const int total = L.content_bottom_row - L.bag_top + 1;
    const int analysis_floor = 3 + 1;
    if (bag_height > total - analysis_floor) {
      bag_height = total - analysis_floor;
    }
    if (bag_height < 1) {
      bag_height = 1;
    }
    L.bag_bottom = L.bag_top + bag_height - 1;
    const int analysis_top = L.bag_bottom + 1;
    if (analysis_top <= L.content_bottom_row &&
        L.content_bottom_row - analysis_top + 1 >= 3) {
      L.analysis_top = analysis_top;
      L.analysis_bottom = L.content_bottom_row;
      L.analysis_left = 0;
      L.analysis_right = L.board_width - 1;
      L.has_analysis = true;
    }
  }

  return L;
}

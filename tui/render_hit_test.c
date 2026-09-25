#include "render_hit_test.h"

#include "../src/def/board_defs.h"

static TuiHitMaps hit_maps;

TuiHitMaps *tui_hit_maps(void) { return &hit_maps; }

int tui_game_panel_at(struct ncplane *plane, const TuiGameState *state, int y,
                      int x) {
  if (plane == NULL || state == NULL || y < 0 || x < 0) {
    return -1;
  }
  // Pick the user's preferred scale the same way tui_game_render
  // does so the recomputed layout matches what's actually on screen.
  struct notcurses *render_nc = ncplane_notcurses(plane);
  const bool pixel_ok = render_nc != NULL && notcurses_canpixel(render_nc);
  const int user_pref =
      (state->board_scale >= 2 && pixel_ok && state->glyph_cache != NULL) ? 2
                                                                          : 1;
  const Layout L = compute_layout(plane, user_pref, state);
  if (L.scale < 0) {
    return -1;
  }
  // Command bar and status bar — clicks on either focus [0].
  if (y == L.command_bar_row || y == L.status_row) {
    return TUI_FOCUS_NONE;
  }
  // Right column is the pills+history assembly (treated as one
  // component) or the separate panels in 3-column mode. The
  // combined_pills_history bool decides.
  if (x >= L.right_col_left && x <= L.right_col_right) {
    if (L.combined_pills_history) {
      if (y >= L.pill1_top && y <= L.history_bottom) {
        return TUI_FOCUS_HISTORY;
      }
    } else {
      if (L.has_analysis && y >= L.analysis_top && y <= L.analysis_bottom &&
          x >= L.analysis_left && x <= L.analysis_right) {
        return TUI_FOCUS_ANALYSIS;
      }
      if (y >= L.history_top && y <= L.history_bottom) {
        return TUI_FOCUS_HISTORY;
      }
    }
  }
  // Three-column analysis lives even further right (separate from
  // the history column).
  if (L.has_analysis && y >= L.analysis_top && y <= L.analysis_bottom &&
      x >= L.analysis_left && x <= L.analysis_right) {
    return TUI_FOCUS_ANALYSIS;
  }
  // Left column: board (top), rack, bag stacked vertically.
  if (x >= 0 && x < L.board_width) {
    if (y <= L.board_bottom_row + 1) {
      return TUI_FOCUS_BOARD;
    }
    if (y >= L.rack_top && y <= L.rack_bottom) {
      return TUI_FOCUS_RACK;
    }
    if (y >= L.bag_top && y <= L.bag_bottom) {
      return TUI_FOCUS_BAG;
    }
  }
  return -1;
}
bool tui_board_cell_at(struct ncplane *plane, const TuiGameState *state, int y,
                       int x, int *out_row, int *out_col) {
  if (plane == NULL || state == NULL || y < 0 || x < 0) {
    return false;
  }
  // Recompute the on-screen layout the same way tui_game_panel_at does
  // so the inverse cell mapping matches the rendered geometry.
  struct notcurses *render_nc = ncplane_notcurses(plane);
  const bool pixel_ok = render_nc != NULL && notcurses_canpixel(render_nc);
  const int user_pref =
      (state->board_scale >= 2 && pixel_ok && state->glyph_cache != NULL) ? 2
                                                                          : 1;
  const Layout L = compute_layout(plane, user_pref, state);
  if (L.scale < 0) {
    return false;
  }
  const int rel_y = y - CELL_ROW_BASE;
  const int rel_x = x - CELL_COL_BASE;
  if (rel_y < 0 || rel_x < 0) {
    return false;
  }
  // board_cell_w / board_cell_h already encode the active scale (2/1 cols
  // and 2/1 rows per cell), so integer division collapses any sub-cell
  // click to the cell it lands in for both 1x and 2x.
  const int row = rel_y / L.board_cell_h;
  const int col = rel_x / L.board_cell_w;
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM) {
    return false;
  }
  *out_row = row;
  *out_col = col;
  return true;
}
int tui_analysis_cursor_at(int y, int x) {
  return tui_analysis_cursor_column_at(y, x, NULL);
}
int tui_analysis_cursor_column_at(int y, int x, TuiAnalysisColumn *out_column) {
  if (y < hit_maps.analysis_panel_top || y > hit_maps.analysis_panel_bottom ||
      x < hit_maps.analysis_panel_left || x > hit_maps.analysis_panel_right) {
    return -2;
  }
  for (int i = 0; i < hit_maps.analysis_row_map_count; i++) {
    const AnalysisRowMap *m = &hit_maps.analysis_row_map[i];
    if (y >= m->top_row && y <= m->bottom_row && x >= m->left_col &&
        x <= m->right_col) {
      if (out_column != NULL) {
        *out_column = (x >= m->move_left_col) ? TUI_ANALYSIS_COLUMN_MOVE
                                              : TUI_ANALYSIS_COLUMN_RANK;
      }
      return m->idx;
    }
  }
  return -1;
}
int tui_modal_item_at(int y, int x) {
  if (!hit_maps.modal_hit_map.valid) {
    return -2;
  }
  // Outside the modal entirely (including the shadow column/row).
  if (y < hit_maps.modal_hit_map.outer_top ||
      y > hit_maps.modal_hit_map.outer_bottom ||
      x < hit_maps.modal_hit_map.outer_left ||
      x > hit_maps.modal_hit_map.outer_right) {
    return -2;
  }
  // Inside the modal but on the chrome (top/bottom border, side
  // columns). The clickable item rectangle runs from
  // (top, left) to (top + item_count - 1, right) inclusive.
  if (y < hit_maps.modal_hit_map.top ||
      y >= hit_maps.modal_hit_map.top + hit_maps.modal_hit_map.item_count ||
      x < hit_maps.modal_hit_map.left || x > hit_maps.modal_hit_map.right) {
    return -1;
  }
  const int idx = y - hit_maps.modal_hit_map.top;
  if (idx < 0 || idx >= hit_maps.modal_hit_map.item_count) {
    return -1;
  }
  // Disabled items count as chrome — clicks are absorbed but not
  // activated.
  if (hit_maps.modal_hit_map.disabled[idx]) {
    return -1;
  }
  return idx;
}
TuiModalChevron tui_modal_chevron_at(int y, int x) {
  const int idx = tui_modal_item_at(y, x);
  if (idx < 0) {
    return TUI_MODAL_CHEVRON_NONE;
  }
  // The chevron column was recorded as the screen col of the
  // glyph cell itself. Accept that exact col as a hit.
  if (hit_maps.modal_hit_map.left_chev_col[idx] >= 0 &&
      x == hit_maps.modal_hit_map.left_chev_col[idx]) {
    return TUI_MODAL_CHEVRON_LEFT;
  }
  if (hit_maps.modal_hit_map.right_chev_col[idx] >= 0 &&
      x == hit_maps.modal_hit_map.right_chev_col[idx]) {
    return TUI_MODAL_CHEVRON_RIGHT;
  }
  return TUI_MODAL_CHEVRON_NONE;
}
int tui_history_cursor_at(int y, int x) {
  return tui_history_cursor_field_at(y, x, NULL);
}
int tui_history_cursor_field_at(int y, int x, int *out_field) {
  // Outside the History panel entirely.
  if (y < hit_maps.history_panel_top || y > hit_maps.history_panel_bottom ||
      x < hit_maps.history_panel_left || x > hit_maps.history_panel_right) {
    return -2;
  }
  // Inside the panel — see whether the point falls on one of the
  // per-entry rectangles populated during the last render.
  for (int i = 0; i < hit_maps.history_row_map_count; i++) {
    const HistoryRowMap *m = &hit_maps.history_row_map[i];
    if (y >= m->top_row && y <= m->bottom_row && x >= m->left_col &&
        x <= m->right_col) {
      if (out_field != NULL) {
        // Row offset within the entry, with a special "leave"
        // sentinel for clicks landing in the right-anchored leave
        // column on the top row:
        //   0 = move text (top row, left of leave zone)
        //   1 = rack (second row)
        //   2 = leave (top row, inside the leave hit zone)
        //   3+ = end-bonus extension, unchanged
        const int dy = y - m->top_row;
        if (dy == 0 && m->leave_left >= 0 && x >= m->leave_left &&
            x <= m->leave_right) {
          *out_field = 2;
        } else {
          *out_field = dy;
        }
      }
      return m->idx;
    }
  }
  // Inside the panel but not on any entry — title / chrome / blank
  // space. Caller snaps the cursor back to -1 (the [4>] label).
  return -1;
}

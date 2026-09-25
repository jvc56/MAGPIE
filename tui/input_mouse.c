#include "input_mouse.h"

#include "move_entry.h"
#include "render_board.h"
#include "render_hit_test.h"
#include "tui_history_edit.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Mouse handling on the game screen: wheel scrolling of the Analysis panel,
// Analysis scrollbar click / drag, and click-to-focus / click-to-select on
// the panels. Returns true when the event was consumed.
bool tui_input_mouse(TuiGameState *state, struct ncplane *std_plane,
                     TuiModalState modal, uint32_t key, ncinput input) {
  // Scroll wheel anywhere on screen scrolls the Analysis panel
  // (currently the only scrollable widget). 3 ranks per wheel
  // notch matches common terminal feel.
  if ((key == NCKEY_BUTTON4 || key == NCKEY_BUTTON5) &&
      modal == TUI_MODAL_NONE) {
    pthread_mutex_lock(&state->mutex);
    const int total = state->last_rendered_analysis_row_count;
    const int view = atomic_load(&state->analysis_visible_rows);
    const int max_scroll = total > view ? total - view : 0;
    int sof = state->analysis_scroll_offset;
    sof += (key == NCKEY_BUTTON5 ? 3 : -3);
    if (sof < 0) {
      sof = 0;
    }
    if (sof > max_scroll) {
      sof = max_scroll;
    }
    state->analysis_scroll_offset = sof;
    // Drag the cursor along with the view so the auto-scroll
    // pass at render time doesn't snap the view right back to
    // wherever the cursor was sitting.
    int cur = state->analysis_cursor;
    if (cur < sof) {
      cur = sof;
    }
    if (view > 0 && cur > sof + view - 1) {
      cur = sof + view - 1;
    }
    if (total > 0 && cur >= total) {
      cur = total - 1;
    }
    state->analysis_cursor = cur;
    pthread_mutex_unlock(&state->mutex);
    return true;
  }
  // Scrollbar click + drag. A press on the scrollbar's track
  // starts a drag; subsequent BUTTON1 events while dragging map
  // y → scroll_offset proportionally so the thumb tracks the
  // cursor. Drag ends at NCTYPE_RELEASE (handled above).
  if (key == NCKEY_BUTTON1 && modal == TUI_MODAL_NONE) {
    const int sb_top = atomic_load(&state->analysis_scrollbar_top);
    const int sb_bot = atomic_load(&state->analysis_scrollbar_bottom);
    const int sb_col = atomic_load(&state->analysis_scrollbar_col);
    const int sb_total = atomic_load(&state->analysis_scrollbar_total);
    const int sb_view = atomic_load(&state->analysis_scrollbar_view);
    const bool sb_active = sb_total > sb_view && sb_view > 0;
    // Hit-test forgiveness — a click on either the scrollbar
    // column or the panel's right border (one cell further
    // right) counts. Widening to the LEFT would eat clicks on
    // the rightmost row-content cell (the sprd column), which
    // is what caused MOVE-column row clicks to sometimes
    // register as scrollbar clicks instead.
    const bool over_sb = sb_active && input.y >= sb_top && input.y <= sb_bot &&
                         input.x >= sb_col && input.x <= sb_col + 1;
    if (over_sb || state->analysis_scrollbar_dragging) {
      pthread_mutex_lock(&state->mutex);
      state->analysis_scrollbar_dragging = true;
      const int track_h = sb_bot - sb_top + 1;
      const int max_scroll = sb_total - sb_view;
      int y_rel = (int)input.y - sb_top;
      if (y_rel < 0) {
        y_rel = 0;
      }
      if (y_rel > track_h - 1) {
        y_rel = track_h - 1;
      }
      const int denom = track_h > 1 ? track_h - 1 : 1;
      int sof = (int)((long long)y_rel * max_scroll / denom);
      if (sof < 0) {
        sof = 0;
      }
      if (sof > max_scroll) {
        sof = max_scroll;
      }
      state->analysis_scroll_offset = sof;
      // Move the cursor along with the view so the auto-scroll
      // pass at render time doesn't immediately snap the view
      // back to wherever the cursor was sitting.
      int cur = state->analysis_cursor;
      if (cur < sof) {
        cur = sof;
      }
      if (sb_view > 0 && cur > sof + sb_view - 1) {
        cur = sof + sb_view - 1;
      }
      const int total = state->last_rendered_analysis_row_count;
      if (total > 0 && cur >= total) {
        cur = total - 1;
      }
      state->analysis_cursor = cur;
      // Focus the Analysis panel while dragging so the user can
      // see the cursor highlight on the scrolled rows.
      state->focused_panel = TUI_FOCUS_ANALYSIS;
      pthread_mutex_unlock(&state->mutex);
      return true;
    }
  }
  // Left mouse click → focus the panel under the cursor. Only
  // NCKEY_BUTTON1 (left-button press) for now; scroll wheel and
  // right-click are reserved for future per-panel interactions.
  if (key == NCKEY_BUTTON1 && modal == TUI_MODAL_NONE) {
    const int hit = tui_game_panel_at(std_plane, state, input.y, input.x);
    if (hit >= 0) {
      pthread_mutex_lock(&state->mutex);
      if (hit != 0 && state->slash_active) {
        state->slash_active = false;
        state->slash_len = 0;
        state->slash_cursor = 0;
        state->slash_buf[0] = '\0';
      }
      // First click into a panel just focuses it (with the cursor
      // reset to the label). A click on History when it's ALREADY
      // focused reads the per-entry hit map to move the in-panel
      // cursor — clicks on the title / chrome row snap back to
      // -1, clicks on a turn jump to that entry.
      //
      // Exception: a click that lands directly on a PENDING entry
      // skips the two-step "focus first, act second" dance and
      // jumps straight into edit mode. The pending row is the
      // primary input surface in annotation mode; needing to
      // click twice to start typing felt broken in testing.
      bool pending_edit_handled = false;
      // Play-vs-computer never opens the history-cell text editor:
      // the human enters moves on the board, and the cell editor's
      // blur-revalidation replays committed history from text, which
      // would desync the live bag-drawn racks. Clicks on History in
      // that mode just move the browse cursor (handled below).
      if (hit == TUI_FOCUS_HISTORY &&
          state->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER) {
        int entry_row_off = 0;
        const int target =
            tui_history_cursor_field_at(input.y, input.x, &entry_row_off);
        // Click into any history entry's textedit (move/rack/
        // leave) opens the editor on that field — committed
        // entries included. Previously the pending-only check
        // meant clicks on already-finalized turns just parked
        // the history cursor on the row label, with no way to
        // jump straight into the cell. Annotation flow needs to
        // revise prior turns, so all entries are addressable.
        if (target >= 0 && target < state->history_count) {
          const TuiHistoryEntry *e = &state->history[target];
          int field;
          switch (entry_row_off) {
          case 1:
            field = TUI_EDIT_FIELD_RACK;
            break;
          case 2:
            field = TUI_EDIT_FIELD_LEAVE;
            break;
          default:
            field = TUI_EDIT_FIELD_MOVE;
            break;
          }
          // Switching to a DIFFERENT entry: blur-commit the turn
          // we're leaving (so typed text persists), then reload
          // the clicked entry's stored move/rack/leave into the
          // editor buffers. (Clicking within the SAME entry —
          // e.g. JUNKY typed in MOVE, then clicking its RACK row
          // — leaves the buffers untouched so work-in-progress
          // survives a field switch.)
          if (state->edit_history_idx != target) {
            if (state->edit_history_idx >= 0) {
              tui_commit_edit_and_revalidate(state);
              // commit may re-fetch e via revalidate side effects;
              // re-resolve the pointer to be safe.
              e = &state->history[target];
            }
            snprintf(state->edit_move_buf, sizeof(state->edit_move_buf), "%s",
                     e->move_str);
            state->edit_move_len = (int)strlen(state->edit_move_buf);
            state->edit_move_cursor = state->edit_move_len;
            snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                     e->rack_str);
            state->edit_rack_len = (int)strlen(state->edit_rack_buf);
            state->edit_rack_cursor = state->edit_rack_len;
            // Committed rack reads as user-authored so it doesn't
            // snap back to the move's inferred letters on the
            // next keystroke.
            state->edit_rack_user_modified = e->rack_str[0] != '\0';
            // Leave buffer tracks the entry's stored leave so
            // clicking into a different turn shows its leave.
            snprintf(state->edit_leave_buf, sizeof(state->edit_leave_buf), "%s",
                     e->leave_str);
            state->edit_leave_len = (int)strlen(state->edit_leave_buf);
            state->edit_leave_cursor = state->edit_leave_len;
            // Drop stale carryover from a previously-edited turn.
            state->edit_rack_carryover[0] = '\0';
          }
          state->focused_panel = TUI_FOCUS_HISTORY;
          state->history_cursor = target;
          state->analysis_cursor = 0;
          state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          state->analysis_anchored_move[0] = '\0';
          state->edit_history_idx = target;
          state->edit_field = field;
          // Clicking into the history-cell text editor takes over
          // from any in-progress board move-entry so keystrokes go
          // to the cell, not the board.
          state->board_entry_active = false;
          tui_game_state_parse_edit_buf(state);
          pending_edit_handled = true;
        }
      }
      if (pending_edit_handled) {
        // Already entered edit mode; skip the focus-only fallback.
      } else if (hit == TUI_FOCUS_HISTORY) {
        // A click acts on the entry under the cursor IMMEDIATELY,
        // focusing the panel as a side effect — no "first click
        // focuses, second click selects" dance. (Two-step focus
        // felt broken everywhere it existed; the board and the
        // pending-cell click already worked this way.)
        state->focused_panel = TUI_FOCUS_HISTORY;
        int entry_row_off = 0;
        const int target =
            tui_history_cursor_field_at(input.y, input.x, &entry_row_off);
        if (target >= -1) {
          state->history_cursor = target;
          state->analysis_cursor = 0;
          state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          state->analysis_anchored_move[0] = '\0';
          // Non-pending row clicked: just move the in-panel cursor.
          state->edit_history_idx = -1;
        }
        (void)entry_row_off;
      } else if (hit == TUI_FOCUS_ANALYSIS) {
        // Same single-click routing as History: focus + move the
        // in-panel cursor in one click. The hit also reports which
        // column was clicked (rank gutter vs move text) so the
        // cursor lands in the right mode. Title / chrome clicks
        // snap to -1.
        state->focused_panel = TUI_FOCUS_ANALYSIS;
        TuiAnalysisColumn clicked_col = TUI_ANALYSIS_COLUMN_RANK;
        const int target =
            tui_analysis_cursor_column_at(input.y, input.x, &clicked_col);
        if (target >= -1) {
          state->analysis_cursor = target;
          state->analysis_cursor_column = clicked_col;
          if (clicked_col == TUI_ANALYSIS_COLUMN_MOVE && target >= 0 &&
              target < state->last_rendered_analysis_row_count) {
            snprintf(state->analysis_anchored_move,
                     sizeof(state->analysis_anchored_move), "%s",
                     state->last_rendered_analysis_rows[target].move);
          } else {
            state->analysis_anchored_move[0] = '\0';
          }
        }
      } else if (hit == TUI_FOCUS_BOARD &&
                 state->app_mode != TUI_APP_MODE_WATCH) {
        // Board move-entry: click an empty cell to anchor and start
        // typing; click the same anchor again to toggle direction.
        // A click on an occupied cell (that isn't the anchor) just
        // focuses the board.
        int cell_row = -1;
        int cell_col = -1;
        if (tui_board_cell_at(std_plane, state, input.y, input.x, &cell_row,
                              &cell_col)) {
          const Board *brd = game_get_board(state->game);
          const bool empty_cell =
              brd != NULL && board_get_letter(brd, cell_row, cell_col) ==
                                 ALPHABET_EMPTY_SQUARE_MARKER;
          if (state->board_entry_active &&
              cell_row == state->board_origin_row &&
              cell_col == state->board_origin_col) {
            // Re-click on the ORIGIN cell (the one the user
            // clicked, not the playthrough-walked-back anchor —
            // comparing against the anchor made the toggle
            // unreachable whenever the walk-back had moved it).
            tui_board_builder_toggle_dir(state);
          } else if (empty_cell) {
            const int dir =
                tui_board_builder_default_dir(state, cell_row, cell_col);
            tui_board_builder_set_anchor(state, cell_row, cell_col, dir);
          }
        }
        state->focused_panel = TUI_FOCUS_BOARD;
      } else {
        state->focused_panel = hit;
      }
      pthread_mutex_unlock(&state->mutex);
    }
    return true;
  }
  return false;
}

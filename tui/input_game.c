#include "input_game.h"

#include "bot_worker.h"
#include "game_state.h"
#include "move_entry.h"
#include "time_picker.h"
#include "tui_clipboard.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Whether the keyboard may open the history-cell editor on entry `idx`.
// Play-vs-computer edits only the human's live pending turn (the cell is
// its keyboard move-entry surface); reopening a committed turn would
// replay history from text and desync the bag-drawn racks, which is
// also why mouse clicks never open the editor in that mode.
static bool history_entry_keyboard_editable(const TuiGameState *state,
                                            int idx) {
  if (idx < 0 || idx >= state->history_count) {
    return false;
  }
  if (state->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER) {
    return true;
  }
  const TuiHistoryEntry *entry = &state->history[idx];
  return idx == state->history_count - 1 && entry->pending &&
         entry->player_idx == state->human_player_idx;
}

// Game-screen keys when no modal or cell editor is open: panel focus
// (0-5, Tab), Esc menu, CGP copy, board / Analysis / History navigation,
// and the command bar with its slash commands. Returns true when consumed.
bool tui_input_game(TuiGameState *state, TuiUiState *ui,
                    const TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_NONE && state->edit_history_idx < 0 &&
      state->focused_panel == TUI_FOCUS_HISTORY && state->history_cursor >= 0 &&
      state->history_cursor < state->history_count &&
      state->history[state->history_cursor].pending &&
      history_entry_keyboard_editable(state, state->history_cursor) &&
      (key == NCKEY_TAB || key == '\t' || key == NCKEY_ENTER || key == '\r' ||
       key == '\n' || key == NCKEY_RIGHT || key == NCKEY_DOWN)) {
    pthread_mutex_lock(&state->mutex);
    const int target = state->history_cursor;
    const TuiHistoryEntry *e = &state->history[target];
    // Seed the buffers from the entry only when nothing has
    // been typed yet — preserves any in-flight text the user
    // had left in the buffer.
    if (state->edit_move_len == 0) {
      tui_game_state_seed_edit_move(state, e->move_str);
    }
    if (state->edit_rack_len == 0) {
      snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
               e->rack_str);
      state->edit_rack_len = (int)strlen(state->edit_rack_buf);
      state->edit_rack_cursor = state->edit_rack_len;
      // Committed rack text on the entry is treated as
      // user-authored: don't snap it back to "match the
      // move's inferred letters" on the next keystroke.
      state->edit_rack_user_modified = e->rack_str[0] != '\0';
    }
    state->edit_history_idx = target;
    // ↓ lands on the RACK row (mirrors the inside-edit
    // semantics where ↓ in MOVE switches to RACK). Everything
    // else opens the MOVE field. In play-vs-computer the rack
    // row is read-only (racks come from the bag), so ↓ opens
    // MOVE like everything else.
    state->edit_field =
        (key == NCKEY_DOWN && state->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER)
            ? TUI_EDIT_FIELD_RACK
            : TUI_EDIT_FIELD_MOVE;
    tui_game_state_parse_edit_buf(state);
    pthread_mutex_unlock(&state->mutex);
    return true;
  }

  if (key == NCKEY_ESC && !state->slash_active) {
    // Esc while typing a slash command cancels the command instead
    // (handled with the rest of the slash input below).
    ui->modal = TUI_MODAL_MAIN_MENU;
    ui->main_menu_focus = 0;
  } else if (key >= '0' && key <= '5') {
    // Direct panel focus hotkeys (no modal). '0' focuses the
    // command bar; '1'-'5' focus the corresponding panel. These
    // work globally regardless of which panel is currently
    // focused — typing digits never gets captured by a panel.
    pthread_mutex_lock(&state->mutex);
    const int new_focus = (int)(key - '0');
    if (new_focus != 0 && state->slash_active) {
      state->slash_active = false;
      state->slash_len = 0;
      state->slash_cursor = 0;
      state->slash_buf[0] = '\0';
    }
    state->focused_panel = new_focus;
    pthread_mutex_unlock(&state->mutex);
  } else if ((key == NCKEY_TAB || key == '\t') && !state->slash_active) {
    // Tab cycles forward through 0..5 (Command → Board → ... →
    // Analysis → Command); Shift-Tab cycles the other way.
    // Useful as a discoverability path — press Tab repeatedly to
    // walk every focus state. While slash mode is active Tab
    // means "autocomplete" instead, so it falls through to the
    // [0]-focused handler below.
    pthread_mutex_lock(&state->mutex);
    const int delta = ncinput_shift_p(&input) ? 5 : 1; // 5 = (-1 mod 6)
    const int new_focus = (state->focused_panel + delta) % 6;
    if (new_focus != 0 && state->slash_active) {
      state->slash_active = false;
      state->slash_len = 0;
      state->slash_cursor = 0;
      state->slash_buf[0] = '\0';
    }
    state->focused_panel = new_focus;
    pthread_mutex_unlock(&state->mutex);
  } else if (key == '/' && !state->slash_active) {
    // Global "/" — focuses [0] Command if it isn't already and
    // begins a slash-command. Lets the user kick off a command
    // from any panel without first pressing 0 / Tab to land on
    // the command bar.
    pthread_mutex_lock(&state->mutex);
    state->focused_panel = 0;
    state->slash_active = true;
    state->slash_len = 0;
    state->slash_cursor = 0;
    state->slash_buf[0] = '\0';
    pthread_mutex_unlock(&state->mutex);
  } else if ((key == 'c' || key == 'C') && !state->slash_active) {
    // Global copy hotkey: current position → clipboard as CGP.
    // Works from any panel focus; editing contexts (board entry,
    // history cells, slash input, modals) consume their keys
    // before this chain so a typed 'c' never lands here.
    tui_copy_position_cgp(state);
  } else if (state->focused_panel == TUI_FOCUS_BOARD &&
             (key == NCKEY_ENTER || key == '\r' || key == '\n') &&
             state->app_mode != TUI_APP_MODE_WATCH) {
    // Keyboard path into board move-entry (design doc: "Enter while
    // the Board panel is focused"): anchors at the board center
    // when open, else the last origin, else the first empty cell —
    // then the normal board-entry keys (letters, arrows, Space,
    // Backspace, Enter, Esc) take over. Mouse-free move entry.
    pthread_mutex_lock(&state->mutex);
    tui_board_entry_begin_keyboard(state);
    pthread_mutex_unlock(&state->mutex);
  } else if (state->focused_panel == TUI_FOCUS_ANALYSIS &&
             (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
              key == NCKEY_RIGHT || key == 'k' || key == 'K' || key == 'j' ||
              key == 'J' || key == 'h' || key == 'H' || key == 'l' ||
              key == 'L' || key == NCKEY_PGUP || key == NCKEY_PGDOWN ||
              key == NCKEY_HOME || key == NCKEY_END)) {
    // Analysis panel nav. -1 = cursor on the [5>] label;
    // 0..N-1 = on a visible candidate row. Up/Down (and j/k)
    // moves the cursor row; Left/Right (and h/l) toggles the
    // column: LEFT/h → RANK (cursor pins to a row index),
    // RIGHT/l → MOVE (cursor pins to the move at the current
    // row and follows it as the sim reorders). PageUp/PageDown
    // jump by the visible window height; Home/End jump to the
    // first/last candidate.
    const bool key_up = key == NCKEY_UP || key == 'k' || key == 'K';
    const bool key_down = key == NCKEY_DOWN || key == 'j' || key == 'J';
    const bool key_left = key == NCKEY_LEFT || key == 'h' || key == 'H';
    const bool key_right = key == NCKEY_RIGHT || key == 'l' || key == 'L';
    const bool key_pgup = key == NCKEY_PGUP;
    const bool key_pgdn = key == NCKEY_PGDOWN;
    const bool key_home = key == NCKEY_HOME;
    const bool key_end = key == NCKEY_END;
    pthread_mutex_lock(&state->mutex);
    const int total = state->last_rendered_analysis_row_count;
    const int view_h = atomic_load(&state->analysis_visible_rows);
    if (key_home) {
      state->analysis_cursor = 0;
    } else if (key_end && total > 0) {
      state->analysis_cursor = total - 1;
    } else if ((key_pgup || key_pgdn) && view_h > 0) {
      const int step = view_h - 1 > 1 ? view_h - 1 : 1;
      int target = state->analysis_cursor < 0 ? 0 : state->analysis_cursor;
      target += key_pgdn ? step : -step;
      if (target < 0) {
        target = 0;
      }
      if (total > 0 && target >= total) {
        target = total - 1;
      }
      state->analysis_cursor = target;
    } else if (key_up || key_down) {
      const int last = total - 1;
      if (key_down) {
        if (state->analysis_cursor < last) {
          state->analysis_cursor++;
        }
      } else {
        if (state->analysis_cursor > -1) {
          state->analysis_cursor--;
        }
      }
      // Re-anchor when in MOVE column so the cursor follows the
      // new row's move from this point on.
      if (state->analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE) {
        const int idx = state->analysis_cursor;
        if (idx >= 0 && idx < state->last_rendered_analysis_row_count) {
          snprintf(state->analysis_anchored_move,
                   sizeof(state->analysis_anchored_move), "%s",
                   state->last_rendered_analysis_rows[idx].move);
        } else {
          state->analysis_anchored_move[0] = '\0';
        }
      }
    } else if (key_left) {
      // Drop to RANK column. Discard anchor — the cursor sticks
      // to whatever row it's currently sitting at.
      state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
      state->analysis_anchored_move[0] = '\0';
    } else if (key_right) {
      // Switch to MOVE column. Capture the current row's move
      // text so the cursor will follow it as the sim reorders.
      const int idx = state->analysis_cursor;
      if (idx >= 0 && idx < state->last_rendered_analysis_row_count) {
        snprintf(state->analysis_anchored_move,
                 sizeof(state->analysis_anchored_move), "%s",
                 state->last_rendered_analysis_rows[idx].move);
        state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_MOVE;
      }
    }
    pthread_mutex_unlock(&state->mutex);
  } else if (state->focused_panel == TUI_FOCUS_HISTORY &&
             (key == NCKEY_HOME || key == NCKEY_END)) {
    // Home/End jump the cursor to the first / newest entry without
    // stepping through every turn — End is the quick "take me to
    // the live pending turn" gesture (and what a scripted driver
    // uses to reach the human's pending cell in play-vs-computer).
    pthread_mutex_lock(&state->mutex);
    const int prev_cursor = state->history_cursor;
    if (state->history_count > 0) {
      state->history_cursor =
          (key == NCKEY_HOME) ? 0 : state->history_count - 1;
    }
    if (state->history_cursor != prev_cursor) {
      state->analysis_cursor = 0;
      state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
      state->analysis_anchored_move[0] = '\0';
    }
    pthread_mutex_unlock(&state->mutex);
  } else if (state->focused_panel == TUI_FOCUS_HISTORY &&
             (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
              key == NCKEY_RIGHT || key == 'k' || key == 'K' || key == 'j' ||
              key == 'J' || key == 'h' || key == 'H' || key == 'l' ||
              key == 'L')) {
    // History panel keyboard nav (cursor is on a label, not in
    // an entry's edit fields). The full step-through sequence is
    //   [4>] → 1> → 1.MOVE → 1.RACK → 2> → 2.MOVE → 2.RACK → 3> → ...
    // Plain Right from a "N>" label enters the editor on turn N's
    // MOVE field; plain Left from "N>" enters the editor on the
    // PREVIOUS turn's RACK (or stays at -1 if already there).
    // Shift+arrow keeps the original label-only behavior (jumps
    // 1> → 2> → 3>); h/j/k/l aliases follow the same rules.
    const bool forward = key == NCKEY_DOWN || key == NCKEY_RIGHT ||
                         key == 'j' || key == 'J' || key == 'l' || key == 'L';
    const bool is_horizontal = key == NCKEY_LEFT || key == NCKEY_RIGHT ||
                               key == 'h' || key == 'H' || key == 'l' ||
                               key == 'L';
    const bool shift = ncinput_shift_p(&input);
    pthread_mutex_lock(&state->mutex);
    const int last = state->history_count - 1;
    const int prev_cursor = state->history_cursor;
    const bool step_into = is_horizontal && !shift;
    if (step_into && forward && state->history_cursor >= 0 &&
        state->history_cursor <= last &&
        history_entry_keyboard_editable(state, state->history_cursor)) {
      // N> → N.MOVE
      TuiHistoryEntry *e = &state->history[state->history_cursor];
      tui_game_state_seed_edit_move(state, e->move_str);
      snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
               e->rack_str);
      state->edit_rack_len = (int)strlen(state->edit_rack_buf);
      state->edit_rack_cursor = state->edit_rack_len;
      state->edit_rack_user_modified = e->rack_str[0] != '\0';
      state->edit_rack_carryover[0] = '\0';
      state->edit_history_idx = state->history_cursor;
      state->edit_field = TUI_EDIT_FIELD_MOVE;
      tui_game_state_parse_edit_buf(state);
    } else if (step_into && !forward && state->history_cursor > 0 &&
               history_entry_keyboard_editable(state,
                                               state->history_cursor - 1)) {
      // N> → (N-1).RACK
      const int target = state->history_cursor - 1;
      TuiHistoryEntry *e = &state->history[target];
      tui_game_state_seed_edit_move(state, e->move_str);
      snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
               e->rack_str);
      state->edit_rack_len = (int)strlen(state->edit_rack_buf);
      state->edit_rack_cursor = state->edit_rack_len;
      state->edit_rack_user_modified = e->rack_str[0] != '\0';
      state->edit_rack_carryover[0] = '\0';
      state->edit_history_idx = target;
      state->history_cursor = target;
      state->edit_field = TUI_EDIT_FIELD_RACK;
      tui_game_state_parse_edit_buf(state);
    } else if (forward) {
      if (state->history_cursor < last) {
        state->history_cursor++;
      }
    } else {
      if (state->history_cursor > -1) {
        state->history_cursor--;
      }
    }
    // Whenever the History cursor lands on a new turn, snap the
    // Analysis cursor back to row 0 so the panel highlights the
    // play actually made for that turn (or the top play of the
    // live in-progress analysis when on the label / a pending
    // entry). Keeps the board preview consistent with the row
    // the user is looking at.
    if (state->history_cursor != prev_cursor) {
      state->analysis_cursor = 0;
      state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
      state->analysis_anchored_move[0] = '\0';
    }
    pthread_mutex_unlock(&state->mutex);
  } else if (state->focused_panel == 0) {
    // Slash-mode input loop: typing /, letters, Tab, Backspace,
    // Enter, Esc all map to slash buffer behavior rather than
    // global hotkeys. Falls through to the alphabetical hotkeys
    // (N/S/Q) only when slash mode is NOT active.
    if (state->slash_active) {
      if (key == NCKEY_ESC) {
        pthread_mutex_lock(&state->mutex);
        state->slash_active = false;
        state->slash_len = 0;
        state->slash_cursor = 0;
        state->slash_buf[0] = '\0';
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_LEFT) {
        pthread_mutex_lock(&state->mutex);
        if (state->slash_cursor > 0) {
          state->slash_cursor--;
        }
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_RIGHT) {
        pthread_mutex_lock(&state->mutex);
        if (state->slash_cursor < state->slash_len) {
          state->slash_cursor++;
        }
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_HOME) {
        pthread_mutex_lock(&state->mutex);
        state->slash_cursor = 0;
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_END) {
        pthread_mutex_lock(&state->mutex);
        state->slash_cursor = state->slash_len;
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_DEL) {
        // Forward-delete: remove char at cursor.
        pthread_mutex_lock(&state->mutex);
        if (state->slash_cursor < state->slash_len) {
          memmove(state->slash_buf + state->slash_cursor,
                  state->slash_buf + state->slash_cursor + 1,
                  (size_t)(state->slash_len - state->slash_cursor));
          state->slash_len--;
        }
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_BACKSPACE || key == 0x7F || key == '\b') {
        pthread_mutex_lock(&state->mutex);
        if (state->slash_cursor > 0) {
          // Remove the char before the cursor.
          memmove(state->slash_buf + state->slash_cursor - 1,
                  state->slash_buf + state->slash_cursor,
                  (size_t)(state->slash_len - state->slash_cursor + 1));
          state->slash_len--;
          state->slash_cursor--;
        } else if (state->slash_len == 0) {
          // Backspace at the very start of an empty buffer exits.
          state->slash_active = false;
        }
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_TAB || key == '\t') {
        // Tab completes against the unique prefix match.
        static const char *cmd_names[] = {"copy",   "exit",     "new", "quit",
                                          "resume", "settings", "stop"};
        static const int n_cmds =
            (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
        const char *match = NULL;
        int n_match = 0;
        for (int i = 0; i < n_cmds; i++) {
          if ((int)strlen(cmd_names[i]) >= state->slash_len &&
              strncmp(cmd_names[i], state->slash_buf,
                      (size_t)state->slash_len) == 0) {
            match = cmd_names[i];
            n_match++;
          }
        }
        if (n_match == 1 && match != NULL) {
          pthread_mutex_lock(&state->mutex);
          snprintf(state->slash_buf, sizeof(state->slash_buf), "%s", match);
          state->slash_len = (int)strlen(match);
          state->slash_cursor = state->slash_len;
          pthread_mutex_unlock(&state->mutex);
        }
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        // Execute the typed-or-completed command. Fall back to a
        // unique prefix match if user pressed Enter without
        // completing first.
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "%s", state->slash_buf);
        if (strcmp(cmd, "new") == 0 || strcmp(cmd, "n") == 0) {
          ui->modal = TUI_MODAL_TIME_PICKER;
          ui->time_focus = tui_time_picker_closest_index(session->chosen_time);
          ui->time_picker_return = TUI_MODAL_NONE;
        } else if (strcmp(cmd, "settings") == 0) {
          ui->modal = TUI_MODAL_SETTINGS;
          ui->settings_focus = 0;
          ui->settings_return = TUI_MODAL_NONE;
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
          ui->modal = TUI_MODAL_QUIT_CONFIRM;
          ui->quit_confirm_focus = 0;
          ui->quit_confirm_return = TUI_MODAL_NONE;
        } else if (strcmp(cmd, "copy") == 0) {
          tui_copy_position_cgp(state);
        } else if (strcmp(cmd, "resume") == 0) {
          pthread_mutex_lock(&state->mutex);
          tui_analysis_worker_start(state, state->history_cursor);
          pthread_mutex_unlock(&state->mutex);
        } else if (strcmp(cmd, "stop") == 0) {
          tui_analysis_worker_stop_and_join(state);
        } else {
          // Try a unique prefix match.
          static const char *cmd_names[] = {"copy",   "exit",     "new", "quit",
                                            "resume", "settings", "stop"};
          static const int n_cmds =
              (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
          const char *match = NULL;
          int n_match = 0;
          for (int i = 0; i < n_cmds; i++) {
            if ((int)strlen(cmd_names[i]) >= state->slash_len &&
                strncmp(cmd_names[i], state->slash_buf,
                        (size_t)state->slash_len) == 0) {
              match = cmd_names[i];
              n_match++;
            }
          }
          if (n_match == 1 && match != NULL) {
            if (strcmp(match, "new") == 0) {
              ui->modal = TUI_MODAL_TIME_PICKER;
              ui->time_focus =
                  tui_time_picker_closest_index(session->chosen_time);
              ui->time_picker_return = TUI_MODAL_NONE;
            } else if (strcmp(match, "settings") == 0) {
              ui->modal = TUI_MODAL_SETTINGS;
              ui->settings_focus = 0;
              ui->settings_return = TUI_MODAL_NONE;
            } else if (strcmp(match, "quit") == 0 ||
                       strcmp(match, "exit") == 0) {
              ui->modal = TUI_MODAL_QUIT_CONFIRM;
              ui->quit_confirm_focus = 0;
              ui->quit_confirm_return = TUI_MODAL_NONE;
            } else if (strcmp(match, "copy") == 0) {
              tui_copy_position_cgp(state);
            } else if (strcmp(match, "resume") == 0) {
              pthread_mutex_lock(&state->mutex);
              tui_analysis_worker_start(state, state->history_cursor);
              pthread_mutex_unlock(&state->mutex);
            } else if (strcmp(match, "stop") == 0) {
              tui_analysis_worker_stop_and_join(state);
            }
          }
        }
        pthread_mutex_lock(&state->mutex);
        state->slash_active = false;
        state->slash_len = 0;
        state->slash_cursor = 0;
        state->slash_buf[0] = '\0';
        pthread_mutex_unlock(&state->mutex);
      } else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        // Insert (lowercased) at the cursor position rather than
        // always appending. Shifts the buffer tail right.
        const char ch =
            (char)((key >= 'A' && key <= 'Z') ? key + ('a' - 'A') : key);
        pthread_mutex_lock(&state->mutex);
        if (state->slash_len < (int)sizeof(state->slash_buf) - 1) {
          memmove(state->slash_buf + state->slash_cursor + 1,
                  state->slash_buf + state->slash_cursor,
                  (size_t)(state->slash_len - state->slash_cursor + 1));
          state->slash_buf[state->slash_cursor] = ch;
          state->slash_len++;
          state->slash_cursor++;
        }
        pthread_mutex_unlock(&state->mutex);
      }
    } else if (key == 'q' || key == 'Q') {
      ui->modal = TUI_MODAL_QUIT_CONFIRM;
      ui->quit_confirm_focus = 0;
      ui->quit_confirm_return = TUI_MODAL_NONE;
    } else if (key == 's' || key == 'S') {
      ui->modal = TUI_MODAL_SETTINGS;
      ui->settings_focus = 0;
      ui->settings_return = TUI_MODAL_NONE;
    } else if (key == 'n' || key == 'N') {
      ui->modal = TUI_MODAL_TIME_PICKER;
      ui->time_focus = tui_time_picker_closest_index(session->chosen_time);
      ui->time_picker_return = TUI_MODAL_NONE;
    }
  }
  return false;
}

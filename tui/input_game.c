#include "input_game.h"

#include "bot_worker.h"
#include "game_state.h"
#include "input_menus.h"
#include "input_settings.h"
#include "move_entry.h"
#include "settings_table.h"
#include "slash_commands.h"
#include "time_picker.h"
#include "tui_clipboard.h"
#include "tui_history_edit.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// Replaces the slash buffer's last word with
// `text`, followed by a space when `then_space`. Caller holds
// state->mutex.
static void replace_last_slash_word(TuiGameState *state,
                                    const TuiSlashWords *words,
                                    const char *text, bool then_space) {
  const int start = words->start[words->count - 1];
  (void)snprintf(state->slash_buf + start,
                 sizeof(state->slash_buf) - (size_t)start, "%s%s", text,
                 then_space ? " " : "");
  state->slash_len = (int)strlen(state->slash_buf);
  state->slash_cursor = state->slash_len;
}

// Extends `typed` (its first `len` characters) to the longest prefix
// every name starting with it shares, into `out`. Returns false when no
// name starts with it.
static bool common_completion(const char *const *names, int name_count,
                              const char *typed, int len, char *out,
                              size_t out_size) {
  int common = -1;
  const char *first = NULL;
  for (int name_idx = 0; name_idx < name_count; name_idx++) {
    const char *name = names[name_idx];
    if ((int)strlen(name) < len || strncasecmp(name, typed, (size_t)len) != 0) {
      continue;
    }
    if (first == NULL) {
      first = name;
      common = (int)strlen(name);
      continue;
    }
    int shared = 0;
    while (shared < common && name[shared] == first[shared]) {
      shared++;
    }
    common = shared;
  }
  if (first == NULL) {
    return false;
  }
  (void)snprintf(out, out_size, "%.*s", common, first);
  return true;
}

// Tab in the command bar: completes the word being typed when it has
// one match, or extends it as far as its matches agree. The command name comes
// first; after "/set", the setting's name and then its value. Caller holds
// state->mutex.
static void complete_slash_word(TuiGameState *state) {
  TuiSlashWords words;
  tui_slash_split(state->slash_buf, state->slash_len, &words);
  if (words.count == 0) {
    return;
  }
  const char *buf = state->slash_buf;
  const int last = words.count - 1;
  const char *typed = buf + words.start[last];
  const int typed_len = words.len[last];
  const TuiSlashCommand *cmd =
      tui_slash_command_resolve(buf + words.start[0], words.len[0]);
  if (last == 0) {
    if (cmd != NULL) {
      replace_last_slash_word(state, &words, cmd->name,
                              cmd->id == TUI_SLASH_SET);
      return;
    }
    int cmd_count = 0;
    const TuiSlashCommand *cmds = tui_slash_commands(&cmd_count);
    const char *names[TUI_SLASH_COUNT * 2];
    int name_count = 0;
    for (int cmd_idx = 0; cmd_idx < cmd_count &&
                          name_count < (int)(sizeof(names) / sizeof(names[0]));
         cmd_idx++) {
      names[name_count++] = cmds[cmd_idx].name;
    }
    char prefix[64];
    if (common_completion(names, name_count, typed, typed_len, prefix,
                          sizeof(prefix))) {
      replace_last_slash_word(state, &words, prefix, false);
    }
    return;
  }
  if (cmd == NULL || cmd->id != TUI_SLASH_SET) {
    return;
  }
  const TuiSettingDef *def =
      tui_setting_resolve(buf + words.start[1], words.len[1]);
  if (last == 1) {
    if (def != NULL) {
      replace_last_slash_word(state, &words, def->key, true);
      return;
    }
    int def_count = 0;
    const TuiSettingDef *defs = tui_setting_defs(&def_count);
    const char *keys[TUI_SETTING_COUNT] = {NULL};
    for (int def_idx = 0; def_idx < def_count; def_idx++) {
      keys[def_idx] = defs[def_idx].key;
    }
    char prefix[64];
    if (common_completion(keys, def_count, typed, typed_len, prefix,
                          sizeof(prefix))) {
      replace_last_slash_word(state, &words, prefix, false);
    }
    return;
  }
  if (last == 2 && def != NULL) {
    const char *value = tui_setting_complete_value(def, typed, typed_len);
    if (value != NULL) {
      replace_last_slash_word(state, &words, value, false);
    }
  }
}

// "/set <name> <value>": changes a setting, reporting the result (or
// what's wrong) in the status bar. "/set <name>" shows its value.
static void run_set_command(TuiGameState *state, TuiSession *session,
                            const TuiSlashWords *words) {
  char message[sizeof(state->notice_buf)];
  const char *buf = state->slash_buf;
  const TuiSettingDef *def =
      words->count > 1 && words->len[1] > 0
          ? tui_setting_resolve(buf + words->start[1], words->len[1])
          : NULL;
  pthread_mutex_lock(&state->mutex);
  if (words->count < 2 || words->len[1] == 0) {
    tui_game_state_notice(state, "usage: /set <name> <value>");
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  if (def == NULL) {
    (void)snprintf(message, sizeof(message), "no setting \"%.*s\"",
                   words->len[1], buf + words->start[1]);
    tui_game_state_notice(state, message);
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  char value_text[32];
  if (words->count < 3 || words->len[2] == 0) {
    char values[64];
    tui_setting_format(def, tui_setting_get(state, def->id), value_text,
                       sizeof(value_text));
    tui_setting_describe_values(def, values, sizeof(values));
    (void)snprintf(message, sizeof(message), "%s is %s (%s)", def->key,
                   value_text, values);
    tui_game_state_notice(state, message);
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  const char *reason = tui_setting_unavailable_reason(state, session, def->id);
  if (reason != NULL) {
    tui_game_state_notice(state, reason);
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  (void)snprintf(value_text, sizeof(value_text), "%.*s", words->len[2],
                 buf + words->start[2]);
  int value = 0;
  if (!tui_setting_parse(def, value_text, &value, message, sizeof(message))) {
    tui_game_state_notice(state, message);
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  pthread_mutex_unlock(&state->mutex);
  tui_setting_set(state, session, def->id, value);
  pthread_mutex_lock(&state->mutex);
  tui_setting_format(def, tui_setting_get(state, def->id), value_text,
                     sizeof(value_text));
  (void)snprintf(message, sizeof(message), "%s set to %s", def->key,
                 value_text);
  tui_game_state_notice(state, message);
  pthread_mutex_unlock(&state->mutex);
}

// Game-screen keys when no modal or cell editor is open: panel focus
// (0-5, Tab), Esc menu, CGP copy, board / Analysis / History navigation,
// and the command bar with its slash commands. Returns true when consumed.
bool tui_input_game(TuiGameState *state, TuiUiState *ui, TuiSession *session,
                    uint32_t key, ncinput input) {
  // Opening the history-cell editor from the History cursor: Enter on
  // any editable entry, and Tab / Right / Down too on the pending one
  // (the turn being entered, so any key toward it starts typing).
  const bool enter_key = key == NCKEY_ENTER || key == '\r' || key == '\n';
  const bool pending_open_key = enter_key || key == NCKEY_TAB || key == '\t' ||
                                key == NCKEY_RIGHT || key == NCKEY_DOWN;
  if (ui->modal == TUI_MODAL_NONE && state->edit_history_idx < 0 &&
      state->focused_panel == TUI_FOCUS_HISTORY && state->history_cursor >= 0 &&
      state->history_cursor < state->history_count &&
      tui_history_entry_editable(state, state->history_cursor) &&
      (state->history[state->history_cursor].pending ? pending_open_key
                                                     : enter_key)) {
    pthread_mutex_lock(&state->mutex);
    const int target = state->history_cursor;
    const TuiHistoryEntry *e = &state->history[target];
    // On the pending entry, seed the buffers only when nothing has
    // been typed yet — preserves any in-flight text the user had left
    // in the buffer. A committed entry always opens on its stored text.
    if (state->edit_move_len == 0 || !e->pending) {
      tui_game_state_seed_edit_move(state, e->move_str);
    }
    if (state->edit_rack_len == 0 || !e->pending) {
      (void)snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                     e->rack_str);
      state->edit_rack_len = (int)strlen(state->edit_rack_buf);
      state->edit_rack_cursor = state->edit_rack_len;
      // Committed rack text on the entry is treated as
      // user-authored: don't snap it back to "match the
      // move's inferred letters" on the next keystroke.
      state->edit_rack_user_modified = e->rack_str[0] != '\0';
    }
    if (!e->pending) {
      state->edit_rack_carryover[0] = '\0';
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

  if (ui->modal == TUI_MODAL_NONE && !state->slash_active &&
      state->focused_panel == TUI_FOCUS_ANALYSIS &&
      state->analysis_cursor < 0 &&
      (key == ' ' || key == NCKEY_ENTER || key == '\r' || key == '\n')) {
    // Space or Enter on the [5] badge opens the analysis menu for the
    // turn selected in History, focused on its first available item.
    ui->modal = TUI_MODAL_ANALYSIS_MENU;
    ui->analysis_menu_focus = TUI_ANALYSIS_MENU_BACK;
    pthread_mutex_lock(&state->mutex);
    for (int item = 0; item < TUI_ANALYSIS_MENU_ITEM_COUNT; item++) {
      if (tui_analysis_menu_reason(state, item) == NULL) {
        ui->analysis_menu_focus = item;
        break;
      }
    }
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
    // "5" lands on the analysis panel's [5] badge, where Space or Enter
    // opens its menu; the arrows move down into the rows.
    if (new_focus == TUI_FOCUS_ANALYSIS) {
      state->analysis_cursor = -1;
      state->analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
      state->analysis_anchored_move[0] = '\0';
    }
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
          (void)snprintf(state->analysis_anchored_move,
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
        (void)snprintf(state->analysis_anchored_move,
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
        tui_history_entry_editable(state, state->history_cursor)) {
      // N> → N.MOVE
      TuiHistoryEntry *e = &state->history[state->history_cursor];
      tui_game_state_seed_edit_move(state, e->move_str);
      (void)snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
                     e->rack_str);
      state->edit_rack_len = (int)strlen(state->edit_rack_buf);
      state->edit_rack_cursor = state->edit_rack_len;
      state->edit_rack_user_modified = e->rack_str[0] != '\0';
      state->edit_rack_carryover[0] = '\0';
      state->edit_history_idx = state->history_cursor;
      state->edit_field = TUI_EDIT_FIELD_MOVE;
      tui_game_state_parse_edit_buf(state);
    } else if (step_into && !forward && state->history_cursor > 0 &&
               tui_history_entry_editable(state, state->history_cursor - 1)) {
      // N> → (N-1).RACK
      const int target = state->history_cursor - 1;
      TuiHistoryEntry *e = &state->history[target];
      tui_game_state_seed_edit_move(state, e->move_str);
      (void)snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
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
                  (size_t)(state->slash_len - state->slash_cursor) + 1);
          state->slash_len--;
          state->slash_cursor--;
        } else if (state->slash_len == 0) {
          // Backspace at the very start of an empty buffer exits.
          state->slash_active = false;
        }
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_TAB || key == '\t') {
        pthread_mutex_lock(&state->mutex);
        complete_slash_word(state);
        pthread_mutex_unlock(&state->mutex);
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        // Execute the typed command: an exact name, or a unique prefix
        // if the user pressed Enter without completing first.
        TuiSlashWords words;
        tui_slash_split(state->slash_buf, state->slash_len, &words);
        const TuiSlashCommand *cmd =
            words.count > 0
                ? tui_slash_command_resolve(state->slash_buf + words.start[0],
                                            words.len[0])
                : NULL;
        switch (cmd != NULL ? cmd->id : TUI_SLASH_COUNT) {
        case TUI_SLASH_SET:
          run_set_command(state, session, &words);
          break;
        case TUI_SLASH_NEW:
          ui->modal = TUI_MODAL_TIME_PICKER;
          ui->time_focus = tui_time_picker_closest_index(session->chosen_time);
          ui->time_picker_return = TUI_MODAL_NONE;
          break;
        case TUI_SLASH_SETTINGS:
          tui_open_settings(ui, TUI_MODAL_NONE);
          break;
        case TUI_SLASH_QUIT:
        case TUI_SLASH_EXIT:
          ui->modal = TUI_MODAL_QUIT_CONFIRM;
          ui->quit_confirm_focus = 0;
          ui->quit_confirm_return = TUI_MODAL_NONE;
          break;
        case TUI_SLASH_COPY:
          tui_copy_position_cgp(state);
          break;
        case TUI_SLASH_RESUME:
        case TUI_SLASH_SIM:
          pthread_mutex_lock(&state->mutex);
          tui_analysis_worker_start(state, state->history_cursor,
                                    cmd->id == TUI_SLASH_SIM);
          pthread_mutex_unlock(&state->mutex);
          break;
        case TUI_SLASH_KIBITZ:
          pthread_mutex_lock(&state->mutex);
          tui_analysis_kibitz(state, state->history_cursor);
          pthread_mutex_unlock(&state->mutex);
          break;
        case TUI_SLASH_STOP:
          tui_analysis_worker_stop_and_join(state);
          break;
        case TUI_SLASH_COUNT:
        default:
          break;
        }
        pthread_mutex_lock(&state->mutex);
        state->slash_active = false;
        state->slash_len = 0;
        state->slash_cursor = 0;
        state->slash_buf[0] = '\0';
        pthread_mutex_unlock(&state->mutex);
      } else if (key >= ' ' && key < 0x7f) {
        // Insert (letters lowercased) at the cursor position rather
        // than always appending. Shifts the buffer tail right.
        const char ch =
            (char)((key >= 'A' && key <= 'Z') ? key + ('a' - 'A') : key);
        pthread_mutex_lock(&state->mutex);
        if (state->slash_len < (int)sizeof(state->slash_buf) - 1) {
          memmove(state->slash_buf + state->slash_cursor + 1,
                  state->slash_buf + state->slash_cursor,
                  (size_t)(state->slash_len - state->slash_cursor) + 1);
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
      tui_open_settings(ui, TUI_MODAL_NONE);
    } else if (key == 'n' || key == 'N') {
      ui->modal = TUI_MODAL_TIME_PICKER;
      ui->time_focus = tui_time_picker_closest_index(session->chosen_time);
      ui->time_picker_return = TUI_MODAL_NONE;
    }
  }
  return false;
}

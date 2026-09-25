#include "input_load.h"

#include "render_board.h"
#include "tui_text_edit.h"
#include <pthread.h>
#include <string.h>

// Load game modal keys (text editing; Enter commits the parsed game).
// Returns true when the key was consumed.
bool tui_input_load_game(TuiGameState *state, TuiUiState *ui,
                         TuiSession *session, uint32_t key, ncinput input) {
  (void)session;
  if (ui->modal == TUI_MODAL_LOAD_GAME) {
    // Mirrors the LOAD_POSITION handler: Esc cancels and resets
    // the previewed game; Enter commits when the live parse
    // succeeded; arrows / Home / End / Backspace / Del / printable
    // chars edit the buffer. The wrap column matches the
    // load-game modal's interior width so Up/Down line up with
    // visual rows. GCGs are real multi-line records — Enter
    // here also inserts a newline, not just submits, so users
    // can build one by hand.
    if (key == NCKEY_ESC) {
      pthread_mutex_lock(&state->mutex);
      game_reset(state->game);
      pthread_mutex_unlock(&state->mutex);
      ui->modal = TUI_MODAL_STARTUP_MENU;
      return true;
    }
    if (key == NCKEY_LEFT) {
      if (ui->load_game_cursor > 0) {
        ui->load_game_cursor--;
      }
      return true;
    }
    if (key == NCKEY_RIGHT) {
      if (ui->load_game_cursor < ui->load_game_len) {
        ui->load_game_cursor++;
      }
      return true;
    }
    if (key == NCKEY_HOME) {
      ui->load_game_cursor = 0;
      return true;
    }
    if (key == NCKEY_END) {
      ui->load_game_cursor = ui->load_game_len;
      return true;
    }
    if (key == NCKEY_UP || key == NCKEY_DOWN) {
      const int wrap_w = LOAD_POSITION_WRAP_W;
      int cur_row = 0;
      int cur_col = 0;
      int target_offset = 0;
      for (int i = 0; i < ui->load_game_cursor; i++) {
        if (ui->load_game_buf[i] == '\n') {
          cur_row++;
          cur_col = 0;
          continue;
        }
        cur_col++;
        if (cur_col >= wrap_w) {
          cur_row++;
          cur_col = 0;
        }
      }
      const int desired_row = cur_row + (key == NCKEY_DOWN ? 1 : -1);
      if (desired_row < 0) {
        target_offset = 0;
      } else {
        int row = 0;
        int col = 0;
        int i = 0;
        target_offset = ui->load_game_cursor;
        bool found = false;
        for (; i <= ui->load_game_len; i++) {
          if (row == desired_row && col == cur_col) {
            target_offset = i;
            found = true;
            break;
          }
          if (i == ui->load_game_len) {
            break;
          }
          if (ui->load_game_buf[i] == '\n') {
            if (row == desired_row) {
              target_offset = i;
              found = true;
              break;
            }
            row++;
            col = 0;
            continue;
          }
          col++;
          if (col >= wrap_w) {
            if (row == desired_row) {
              target_offset = i + 1;
              found = true;
              break;
            }
            row++;
            col = 0;
          }
        }
        if (!found) {
          target_offset = ui->load_game_len;
        }
      }
      ui->load_game_cursor = target_offset;
      return true;
    }
    if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
      if (ui->load_game_cursor > 0) {
        memmove(&ui->load_game_buf[ui->load_game_cursor - 1],
                &ui->load_game_buf[ui->load_game_cursor],
                (size_t)(ui->load_game_len - ui->load_game_cursor + 1));
        ui->load_game_cursor--;
        ui->load_game_len--;
        ui->load_game_dirty = true;
      }
      return true;
    }
    if (key == NCKEY_DEL) {
      if (ui->load_game_cursor < ui->load_game_len) {
        memmove(&ui->load_game_buf[ui->load_game_cursor],
                &ui->load_game_buf[ui->load_game_cursor + 1],
                (size_t)(ui->load_game_len - ui->load_game_cursor));
        ui->load_game_len--;
        ui->load_game_dirty = true;
      }
      return true;
    }
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      // The modal is single-line (path input only) — drag the
      // .gcg file from the Finder onto the window and the
      // terminal pastes its path. Pasting raw GCG content is
      // not supported (macOS Terminal's "warn before paste"
      // mitigation silently strips newlines, leaving the
      // parser unable to tokenize). Enter just submits when
      // the live-preview parse succeeded.
      if (ui->load_game_parse_ok) {
        ui->modal = TUI_MODAL_NONE;
      }
      return true;
    }
    // Translate Ctrl+letter into the corresponding ASCII control
    // code (0x01..0x1a) so the readline helper sees a uniform
    // input regardless of whether the terminal is using a
    // legacy raw-byte ctrl mapping or a modern protocol that
    // delivers ctrl as a modifier on the letter keycode.
    uint32_t rl_key_g = key;
    if ((input.modifiers & NCKEY_MOD_CTRL) || input.ctrl) {
      if (key >= 'a' && key <= 'z') {
        rl_key_g = key - 'a' + 1;
      } else if (key >= 'A' && key <= 'Z') {
        rl_key_g = key - 'A' + 1;
      }
    }
    if (tui_text_readline_key(rl_key_g, ui->load_game_buf,
                              &ui->load_game_cursor, &ui->load_game_len,
                              &ui->load_game_dirty)) {
      return true;
    }
    if (key >= 0x20 && key < 0x7f) {
      if (ui->load_game_len + 1 < (int)sizeof(ui->load_game_buf)) {
        memmove(&ui->load_game_buf[ui->load_game_cursor + 1],
                &ui->load_game_buf[ui->load_game_cursor],
                (size_t)(ui->load_game_len - ui->load_game_cursor + 1));
        ui->load_game_buf[ui->load_game_cursor] = (char)key;
        ui->load_game_cursor++;
        ui->load_game_len++;
        ui->load_game_dirty = true;
      }
      return true;
    }
    return true;
  }
  return false;
}

// Load position modal keys (text editing; Enter commits the parsed position).
// Returns true when the key was consumed.
bool tui_input_load_position(TuiGameState *state, TuiUiState *ui,
                             TuiSession *session, uint32_t key, ncinput input) {
  (void)session;
  if (ui->modal == TUI_MODAL_LOAD_POSITION) {
    if (key == NCKEY_ESC) {
      // Cancel — reset the previewed game back to idle so the
      // user returns to the empty board they came from.
      pthread_mutex_lock(&state->mutex);
      game_reset(state->game);
      pthread_mutex_unlock(&state->mutex);
      ui->modal = TUI_MODAL_STARTUP_MENU;
      return true;
    }
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      // Enter commits whatever the live-preview parse produced.
      // CGPs don't contain newlines, so there's no reason for
      // Enter to do anything else inside the input. If the last
      // parse failed, the error stays visible and Enter is a
      // no-op.
      if (ui->load_position_parse_ok) {
        ui->modal = TUI_MODAL_NONE;
      }
      return true;
    }
    if (key == NCKEY_LEFT) {
      if (ui->load_position_cursor > 0) {
        ui->load_position_cursor--;
      }
      return true;
    }
    if (key == NCKEY_RIGHT) {
      if (ui->load_position_cursor < ui->load_position_len) {
        ui->load_position_cursor++;
      }
      return true;
    }
    if (key == NCKEY_HOME) {
      ui->load_position_cursor = 0;
      return true;
    }
    if (key == NCKEY_END) {
      ui->load_position_cursor = ui->load_position_len;
      return true;
    }
    if (key == NCKEY_UP || key == NCKEY_DOWN) {
      // Walk one visual row in the wrapped layout. Visual rows
      // are formed by '\n' OR by reaching LOAD_POSITION_WRAP_W
      // cells — same logic the renderer uses to lay out the
      // input area, so cursor motion lines up with what the
      // user sees.
      const int wrap_w = LOAD_POSITION_WRAP_W;
      // Find the visual (row, col) of the current cursor.
      int cur_row = 0;
      int cur_col = 0;
      int target_offset = 0;
      for (int i = 0; i < ui->load_position_cursor; i++) {
        if (ui->load_position_buf[i] == '\n') {
          cur_row++;
          cur_col = 0;
          continue;
        }
        cur_col++;
        if (cur_col >= wrap_w) {
          cur_row++;
          cur_col = 0;
        }
      }
      const int desired_row = cur_row + (key == NCKEY_DOWN ? 1 : -1);
      if (desired_row < 0) {
        target_offset = 0;
      } else {
        // Walk again, this time landing at (desired_row, cur_col)
        // or end-of-row if shorter.
        int row = 0;
        int col = 0;
        int i = 0;
        target_offset = ui->load_position_cursor; // default: stay put
        bool found = false;
        for (; i <= ui->load_position_len; i++) {
          if (row == desired_row && col == cur_col) {
            target_offset = i;
            found = true;
            break;
          }
          if (i == ui->load_position_len) {
            break;
          }
          if (ui->load_position_buf[i] == '\n') {
            if (row == desired_row) {
              target_offset = i;
              found = true;
              break;
            }
            row++;
            col = 0;
            continue;
          }
          col++;
          if (col >= wrap_w) {
            if (row == desired_row) {
              target_offset = i + 1;
              found = true;
              break;
            }
            row++;
            col = 0;
          }
        }
        if (!found) {
          // Past the end — clamp to end of buffer.
          target_offset = ui->load_position_len;
        }
      }
      ui->load_position_cursor = target_offset;
      return true;
    }
    if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
      if (ui->load_position_cursor > 0) {
        memmove(&ui->load_position_buf[ui->load_position_cursor - 1],
                &ui->load_position_buf[ui->load_position_cursor],
                (size_t)(ui->load_position_len - ui->load_position_cursor + 1));
        ui->load_position_cursor--;
        ui->load_position_len--;
        ui->load_position_dirty = true;
      }
      return true;
    }
    if (key == NCKEY_DEL) {
      if (ui->load_position_cursor < ui->load_position_len) {
        memmove(&ui->load_position_buf[ui->load_position_cursor],
                &ui->load_position_buf[ui->load_position_cursor + 1],
                (size_t)(ui->load_position_len - ui->load_position_cursor));
        ui->load_position_len--;
        ui->load_position_dirty = true;
      }
      return true;
    }
    // See LOAD_GAME handler — translate Ctrl+letter into the
    // 0x01..0x1a ASCII range so the readline helper recognizes
    // the binding regardless of how the terminal encodes Ctrl.
    uint32_t rl_key_p = key;
    if ((input.modifiers & NCKEY_MOD_CTRL) || input.ctrl) {
      if (key >= 'a' && key <= 'z') {
        rl_key_p = key - 'a' + 1;
      } else if (key >= 'A' && key <= 'Z') {
        rl_key_p = key - 'A' + 1;
      }
    }
    if (tui_text_readline_key(rl_key_p, ui->load_position_buf,
                              &ui->load_position_cursor, &ui->load_position_len,
                              &ui->load_position_dirty)) {
      return true;
    }
    if (key >= 0x20 && key < 0x7f) {
      if (ui->load_position_len + 1 < (int)sizeof(ui->load_position_buf)) {
        memmove(&ui->load_position_buf[ui->load_position_cursor + 1],
                &ui->load_position_buf[ui->load_position_cursor],
                (size_t)(ui->load_position_len - ui->load_position_cursor + 1));
        ui->load_position_buf[ui->load_position_cursor] = (char)key;
        ui->load_position_cursor++;
        ui->load_position_len++;
        ui->load_position_dirty = true;
      }
      return true;
    }
    return true;
  }
  return false;
}

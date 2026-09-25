#include "input_load.h"

#include "../src/impl/cgp.h"
#include "../src/impl/gcg.h"
#include "bot_worker.h"
#include "gcg_import.h"
#include "tui_text_edit.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

// Re-parse the Load game text when it changed: resolve a path or raw GCG,
// import it into the History, and record the parse result / error for
// the modal.
void tui_load_game_live_parse(TuiGameState *state, TuiUiState *ui) {
  if (ui->modal == TUI_MODAL_LOAD_GAME && ui->load_game_dirty) {
    ui->load_game_dirty = false;
    // Working buffer big enough to copy the entire load buffer.
    // GCGs can be several KB; we keep this on the stack but
    // sized to the modal buffer.
    static char working[sizeof(ui->load_game_buf)];
    snprintf(working, sizeof(working), "%s", ui->load_game_buf);
    char *start = working;
    while (*start == ' ' || *start == '\t' || *start == '\n' ||
           *start == '\r') {
      start++;
    }
    size_t wlen = strlen(start);
    while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                        start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
      start[--wlen] = '\0';
    }
    if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                      (start[0] == '\'' && start[wlen - 1] == '\''))) {
      start[wlen - 1] = '\0';
      start++;
      wlen -= 2;
    }
    if (strncmp(start, "file://", 7) == 0) {
      start += 7;
      wlen -= 7;
    }
    bool looks_like_path =
        wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                     (wlen > 4 && strcmp(start + wlen - 4, ".gcg") == 0));
    if (looks_like_path) {
      // A path can't contain embedded newlines. Multi-line raw
      // GCGs will always fail this check.
      for (size_t i = 0; i < wlen; i++) {
        if (start[i] == '\n') {
          looks_like_path = false;
          break;
        }
      }
    }
    // GCG payload buffer — GCGs can be sizable (a long game
    // with notes can run into the tens of KB), so allocate
    // generously on the heap rather than blowing the stack.
    char *gcg_payload = NULL;
    bool resolve_ok = true;
    if (wlen == 0) {
      ui->load_game_error[0] = '\0';
      resolve_ok = false;
    } else if (looks_like_path) {
      char path[1024];
      if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home != NULL) {
          snprintf(path, sizeof(path), "%s%s", home,
                   start[1] == '\0' ? "" : start + 1);
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
      } else {
        snprintf(path, sizeof(path), "%s", start);
      }
      // A path typed character by character passes through directory
      // prefixes ("/", "/Users", ...). fopen succeeds on a directory and
      // reads nothing, so reject anything that isn't a regular file.
      struct stat path_stat;
      const bool not_regular_file =
          stat(path, &path_stat) == 0 && !S_ISREG(path_stat.st_mode);
      FILE *fp = not_regular_file ? NULL : fopen(path, "rb");
      if (not_regular_file) {
        snprintf(ui->load_game_error, sizeof(ui->load_game_error),
                 "%s is not a file", path);
        resolve_ok = false;
      } else if (fp == NULL) {
        snprintf(ui->load_game_error, sizeof(ui->load_game_error),
                 "Cannot open %s", path);
        resolve_ok = false;
      } else {
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsize < 0 || fsize > (long)(1 << 20)) {
          // Cap at 1 MiB — anything bigger is almost certainly
          // not a real GCG.
          snprintf(ui->load_game_error, sizeof(ui->load_game_error),
                   "%s is too large", path);
          fclose(fp);
          resolve_ok = false;
        } else {
          gcg_payload = malloc((size_t)fsize + 1);
          size_t n = fread(gcg_payload, 1, (size_t)fsize, fp);
          fclose(fp);
          gcg_payload[n] = '\0';
        }
      }
    } else {
      gcg_payload = malloc(wlen + 1);
      memcpy(gcg_payload, start, wlen);
      gcg_payload[wlen] = '\0';
    }
    // Strip a trailing incomplete `>nickname: rack` line.
    // Quackle and cross-tables.com emit this as a hint about the
    // on-turn player's rack at the time the GCG was exported,
    // but it is not a valid GCG event — MAGPIE's strict parser
    // rejects the whole file otherwise. We just truncate it
    // here; loading the events that precede it is what the user
    // actually wants.
    if (resolve_ok && gcg_payload != NULL) {
      size_t plen = strlen(gcg_payload);
      while (plen > 0 &&
             (gcg_payload[plen - 1] == '\n' || gcg_payload[plen - 1] == '\r' ||
              gcg_payload[plen - 1] == ' ' || gcg_payload[plen - 1] == '\t')) {
        gcg_payload[--plen] = '\0';
      }
      size_t line_start = plen;
      while (line_start > 0 && gcg_payload[line_start - 1] != '\n') {
        line_start--;
      }
      if (line_start < plen && gcg_payload[line_start] == '>') {
        int tokens = 0;
        bool in_token = false;
        for (size_t i = line_start; i < plen; i++) {
          const char c = gcg_payload[i];
          const bool ws = c == ' ' || c == '\t';
          if (!ws && !in_token) {
            tokens++;
            in_token = true;
          } else if (ws) {
            in_token = false;
          }
        }
        // Real GCG event lines have at least 4 whitespace-
        // separated tokens (the end-rack-points form). Anything
        // shorter is the trailing rack hint.
        if (tokens < 4) {
          gcg_payload[line_start] = '\0';
        }
      }
      // The GCG parser treats input with no lines as a fatal error, so
      // never hand it an empty payload (an empty file, or text that was
      // only the trailing rack hint).
      if (gcg_payload[0] == '\0') {
        snprintf(ui->load_game_error, sizeof(ui->load_game_error), "Empty GCG");
        resolve_ok = false;
      }
    }
    if (resolve_ok) {
      // The analysis-resume worker reads history entries and the
      // endgame ctx — stop it before any reset / reconfigure.
      tui_analysis_worker_stop_and_join(state);
      if (state->bot_started) {
        atomic_store(&state->bot_stop, true);
        pthread_join(state->bot_thread, NULL);
        state->bot_started = false;
        atomic_store(&state->bot_stop, false);
      }
      ErrorStack *err = error_stack_create();
      GameHistory *history = game_history_create();
      GCGParser *parser =
          gcg_parser_create(gcg_payload, history, state->active_lexicon, err);
      bool ok = error_stack_is_empty(err);
      if (ok) {
        parse_gcg_settings(parser, err);
        ok = error_stack_is_empty(err);
      }
      pthread_mutex_lock(&state->mutex);
      if (ok) {
        parse_gcg_events(parser, state->game, err);
        ok = error_stack_is_empty(err);
      }
      if (ok) {
        for (int i = 0; i < state->history_count; i++) {
          TuiHistoryEntry *e = &state->history[i];
          if (e->board_before != NULL) {
            board_destroy(e->board_before);
            e->board_before = NULL;
          }
          if (e->rack_before != NULL) {
            rack_destroy(e->rack_before);
            e->rack_before = NULL;
          }
          if (e->opp_rack_before != NULL) {
            rack_destroy(e->opp_rack_before);
            e->opp_rack_before = NULL;
          }
          if (e->sim_results_saved != NULL) {
            sim_results_destroy(e->sim_results_saved);
            e->sim_results_saved = NULL;
          }
          if (e->endgame_moves_saved != NULL) {
            free(e->endgame_moves_saved);
            e->endgame_moves_saved = NULL;
            e->endgame_moves_saved_count = 0;
          }
          if (e->loaded_move != NULL) {
            free(e->loaded_move);
            e->loaded_move = NULL;
          }
        }
        state->history_count = 0;
        state->history_cursor = -1;
        state->analysis_cursor = -1;
        state->analysis_cursor_column = 0;
        state->analysis_anchored_move[0] = '\0';
        state->seconds_used[0] = 0.0;
        state->seconds_used[1] = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &state->turn_started);

        // Wipe analysis state left over from any prior session in
        // this process. Without this, loading a GCG after watching
        // a Magpie-vs-Magpie game leaves the previous game's
        // endgame leaderboard and sim plays stranded in the
        // Analysis panel. The structs are owned by the
        // TuiGameState, so destroying + recreating is the safest
        // way to drop their internal data without needing a
        // MoveList for sim_results_reset.
        tui_endgame_snapshot_clear(&state->endgame_snapshot);
        atomic_store(&state->endgame_results_active, false);
        atomic_store(&state->endgame_results_turn_idx, -1);
        if (state->sim_results != NULL) {
          sim_results_destroy(state->sim_results);
        }
        state->sim_results = sim_results_create(0.005);
        atomic_store(&state->sim_results_active, false);
        atomic_store(&state->sim_results_turn_idx, -1);
        atomic_store(&state->peg_results_active, false);
        atomic_store(&state->peg_results_turn_idx, -1);

        // Surface real player names from the GCG so the pill
        // headers read "Quackle" / "New Player 1" instead of
        // the generic "P1" / "P2". Empty when not set.
        for (int p = 0; p < 2; p++) {
          const char *pname = game_history_player_get_name(history, p);
          if (pname != NULL) {
            snprintf(state->player_names[p], sizeof(state->player_names[p]),
                     "%s", pname);
          } else {
            state->player_names[p][0] = '\0';
          }
        }

        tui_gcg_import_history(state, history);

        ui->load_game_error[0] = '\0';
        ui->load_game_parse_ok = true;
      } else {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(ui->load_game_error, sizeof(ui->load_game_error), "%s",
                 msg != NULL ? msg : "Parse error");
        free(msg);
        ui->load_game_parse_ok = false;
      }
      pthread_mutex_unlock(&state->mutex);
      gcg_parser_destroy(parser);
      game_history_destroy(history);
      error_stack_destroy(err);
    } else {
      ui->load_game_parse_ok = false;
    }
    free(gcg_payload);
  }
}

// Re-parse the Load position text when it changed: resolve a path or raw
// CGP, preview the position behind the modal, and record the parse result /
// error.
void tui_load_position_live_parse(TuiGameState *state, TuiUiState *ui) {
  if (ui->modal == TUI_MODAL_LOAD_POSITION && ui->load_position_dirty) {
    ui->load_position_dirty = false;
    char working[2048];
    snprintf(working, sizeof(working), "%s", ui->load_position_buf);
    // Strip leading + trailing whitespace.
    char *start = working;
    while (*start == ' ' || *start == '\t' || *start == '\n' ||
           *start == '\r') {
      start++;
    }
    size_t wlen = strlen(start);
    while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                        start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
      start[--wlen] = '\0';
    }
    // Strip surrounding quotes (terminals wrap dragged paths).
    if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                      (start[0] == '\'' && start[wlen - 1] == '\''))) {
      start[wlen - 1] = '\0';
      start++;
      wlen -= 2;
    }
    if (strncmp(start, "file://", 7) == 0) {
      start += 7;
      wlen -= 7;
    }
    // Heuristic for path vs raw CGP.
    bool looks_like_path =
        wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                     (wlen > 4 && strcmp(start + wlen - 4, ".cgp") == 0));
    if (looks_like_path) {
      for (size_t i = 0; i < wlen; i++) {
        if (start[i] == ' ' || start[i] == '\t' || start[i] == '\n') {
          looks_like_path = false;
          break;
        }
      }
    }
    char cgp_payload[4096];
    cgp_payload[0] = '\0';
    bool resolve_ok = true;
    if (wlen == 0) {
      ui->load_position_error[0] = '\0';
      resolve_ok = false;
    } else if (looks_like_path) {
      char path[1024];
      if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home != NULL) {
          snprintf(path, sizeof(path), "%s%s", home,
                   start[1] == '\0' ? "" : start + 1);
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
      } else {
        snprintf(path, sizeof(path), "%s", start);
      }
      FILE *fp = fopen(path, "rb");
      if (fp == NULL) {
        snprintf(ui->load_position_error, sizeof(ui->load_position_error),
                 "Cannot open %s", path);
        resolve_ok = false;
      } else {
        size_t n = fread(cgp_payload, 1, sizeof(cgp_payload) - 1, fp);
        fclose(fp);
        cgp_payload[n] = '\0';
      }
    } else {
      snprintf(cgp_payload, sizeof(cgp_payload), "%s", start);
    }
    if (resolve_ok) {
      // Stop the bot if a previous load started one (currently
      // we never start the bot on load, but be safe).
      // The analysis-resume worker reads history entries and the
      // endgame ctx — stop it before any reset / reconfigure.
      tui_analysis_worker_stop_and_join(state);
      if (state->bot_started) {
        atomic_store(&state->bot_stop, true);
        pthread_join(state->bot_thread, NULL);
        state->bot_started = false;
        atomic_store(&state->bot_stop, false);
      }
      ErrorStack *err = error_stack_create();
      pthread_mutex_lock(&state->mutex);
      game_load_cgp(state->game, cgp_payload, err);
      const bool ok = error_stack_is_empty(err);
      if (ok) {
        // On success, reset per-turn history / cursors so the
        // panels reflect a fresh starting state for the loaded
        // position.
        for (int i = 0; i < state->history_count; i++) {
          TuiHistoryEntry *e = &state->history[i];
          if (e->board_before != NULL) {
            board_destroy(e->board_before);
            e->board_before = NULL;
          }
          if (e->rack_before != NULL) {
            rack_destroy(e->rack_before);
            e->rack_before = NULL;
          }
          if (e->opp_rack_before != NULL) {
            rack_destroy(e->opp_rack_before);
            e->opp_rack_before = NULL;
          }
          if (e->sim_results_saved != NULL) {
            sim_results_destroy(e->sim_results_saved);
            e->sim_results_saved = NULL;
          }
          if (e->endgame_moves_saved != NULL) {
            free(e->endgame_moves_saved);
            e->endgame_moves_saved = NULL;
            e->endgame_moves_saved_count = 0;
          }
          if (e->loaded_move != NULL) {
            free(e->loaded_move);
            e->loaded_move = NULL;
          }
        }
        state->history_count = 0;
        state->history_cursor = -1;
        state->analysis_cursor = -1;
        state->analysis_cursor_column = 0;
        state->analysis_anchored_move[0] = '\0';
        state->seconds_used[0] = 0.0;
        state->seconds_used[1] = 0.0;
        clock_gettime(CLOCK_MONOTONIC, &state->turn_started);
        // Seed a pending history entry for the upcoming turn so
        // the History panel shows "1." waiting for input, with
        // the on-turn player's rack snapshotted and clocks
        // reset to full. This matches how the bot worker
        // appends a pending entry at the start of every turn —
        // the user is now "the bot" deciding what comes next.
        const int on_turn = game_get_player_on_turn_index(state->game);
        const Rack *on_turn_rack =
            player_get_rack(game_get_player(state->game, on_turn));
        tui_bot_worker_append_pending_history(state, on_turn, on_turn_rack,
                                              state->time_per_side_seconds);
        ui->load_position_error[0] = '\0';
        ui->load_position_parse_ok = true;
      } else {
        char *msg = error_stack_get_string_and_reset(err);
        snprintf(ui->load_position_error, sizeof(ui->load_position_error), "%s",
                 msg != NULL ? msg : "Parse error");
        free(msg);
        ui->load_position_parse_ok = false;
      }
      pthread_mutex_unlock(&state->mutex);
      error_stack_destroy(err);
    } else {
      ui->load_position_parse_ok = false;
    }
  }
}

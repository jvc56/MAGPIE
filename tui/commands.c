#include "commands.h"

#include "bot_worker.h"
#include "config.h"
#include "game_render.h"
#include "game_state.h"
#include "gcg_export.h"
#include "input_settings.h"
#include "slash_commands.h"
#include "time_picker.h"
#include "tui_clipboard.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Whether the game has a turn to save.
static bool has_committed_turn(const TuiGameState *state) {
  for (int idx = 0; idx < state->history_count; idx++) {
    if (!state->history[idx].pending) {
      return true;
    }
  }
  return false;
}

void tui_command_save(TuiGameState *state, const char *path) {
  char resolved[TUI_CONFIG_PATH_MAX];
  const char *home = getenv("HOME");
  if (path == NULL || path[0] == '\0') {
    tui_gcg_default_path(resolved, sizeof(resolved));
  } else if (strncmp(path, "~/", 2) == 0 && home != NULL) {
    (void)snprintf(resolved, sizeof(resolved), "%s/%s", home, path + 2);
  } else {
    (void)snprintf(resolved, sizeof(resolved), "%s", path);
  }
  path = resolved;
  pthread_mutex_lock(&state->mutex);
  const char *reason = tui_command_unavailable_reason(state, TUI_SLASH_SAVE);
  if (reason != NULL) {
    tui_game_state_notice(state, reason);
  } else {
    char message[sizeof(state->notice_buf)];
    (void)snprintf(message, sizeof(message),
                   tui_gcg_save(state, path) ? "Saved %s" : "Couldn't write %s",
                   path);
    tui_game_state_notice(state, message);
  }
  pthread_mutex_unlock(&state->mutex);
}

const char *tui_command_unavailable_reason(const TuiGameState *state,
                                           TuiSlashCommandId id) {
  switch (id) {
  case TUI_SLASH_PLAY_FROM:
  case TUI_SLASH_WATCH_FROM:
    return tui_game_state_branch_reason(state, state->history_cursor);
  case TUI_SLASH_SAVE:
  case TUI_SLASH_COPY_GCG:
    return has_committed_turn(state) ? NULL : "No turns to save yet.";
  case TUI_SLASH_SIM:
    return tui_analysis_unavailable_reason(state, state->history_cursor,
                                           TUI_ANALYSIS_SIM);
  case TUI_SLASH_RESUME:
    return tui_analysis_unavailable_reason(state, state->history_cursor,
                                           TUI_ANALYSIS_RESUME);
  case TUI_SLASH_KIBITZ:
    return tui_analysis_unavailable_reason(state, state->history_cursor,
                                           TUI_ANALYSIS_KIBITZ);
  case TUI_SLASH_STOP:
    return tui_analysis_unavailable_reason(state, state->history_cursor,
                                           TUI_ANALYSIS_STOP);
  case TUI_SLASH_SOLVE:
    return tui_analysis_unavailable_reason(state, state->history_cursor,
                                           TUI_ANALYSIS_SOLVE);
  default:
    return NULL;
  }
}

// "/playfrom" / "/watchfrom": rewinds to the turn under the History
// cursor and carries on from there, the human taking that turn's player
// (`play`) or both computers playing.
static void carry_on_from_cursor(TuiGameState *state, bool play) {
  tui_stop_workers(state);
  pthread_mutex_lock(&state->mutex);
  const int turn_idx = state->history_cursor;
  const int on_turn_idx = turn_idx >= 0 && turn_idx < state->history_count
                              ? state->history[turn_idx].player_idx
                              : 0;
  const bool branched = tui_game_state_branch_at(state, turn_idx);
  if (!branched) {
    tui_game_state_notice(state, "Couldn't load that turn's position.");
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  state->app_mode = play ? TUI_APP_MODE_PLAY_VS_COMPUTER : TUI_APP_MODE_WATCH;
  if (play) {
    state->human_player_idx = on_turn_idx;
    state->focused_panel = TUI_FOCUS_BOARD;
  }
  pthread_mutex_unlock(&state->mutex);
  // Rebuild cached tile planes against the rewound board.
  tui_game_render_reset_grids();
  tui_bot_worker_start(state);
}

void tui_command_run(TuiGameState *state, TuiUiState *ui,
                     const TuiSession *session, TuiSlashCommandId id) {
  pthread_mutex_lock(&state->mutex);
  const char *reason = tui_command_unavailable_reason(state, id);
  if (reason != NULL) {
    tui_game_state_notice(state, reason);
  }
  pthread_mutex_unlock(&state->mutex);
  if (reason != NULL) {
    return;
  }
  switch (id) {
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
  case TUI_SLASH_COPY_GCG:
    tui_copy_game_gcg(state);
    break;
  case TUI_SLASH_SAVE:
    tui_command_save(state, NULL);
    break;
  case TUI_SLASH_PLAY_FROM:
  case TUI_SLASH_WATCH_FROM:
    carry_on_from_cursor(state, id == TUI_SLASH_PLAY_FROM);
    break;
  case TUI_SLASH_RESUME:
  case TUI_SLASH_SIM:
  case TUI_SLASH_SOLVE: {
    TuiAnalysisAction action = TUI_ANALYSIS_SIM;
    if (id == TUI_SLASH_RESUME) {
      action = TUI_ANALYSIS_RESUME;
    } else if (id == TUI_SLASH_SOLVE) {
      action = TUI_ANALYSIS_SOLVE;
    }
    pthread_mutex_lock(&state->mutex);
    tui_analysis_worker_start(state, state->history_cursor, action);
    pthread_mutex_unlock(&state->mutex);
    break;
  }
  case TUI_SLASH_KIBITZ:
    pthread_mutex_lock(&state->mutex);
    tui_analysis_kibitz(state, state->history_cursor);
    pthread_mutex_unlock(&state->mutex);
    break;
  case TUI_SLASH_STOP:
    tui_analysis_worker_stop_and_join(state);
    break;
  case TUI_SLASH_SET:
  case TUI_SLASH_COUNT:
  default:
    break;
  }
}

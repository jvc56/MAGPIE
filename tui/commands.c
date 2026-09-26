#include "commands.h"

#include "bot_worker.h"
#include "game_state.h"
#include "input_settings.h"
#include "slash_commands.h"
#include "time_picker.h"
#include "tui_clipboard.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <pthread.h>
#include <stddef.h>

const char *tui_command_unavailable_reason(const TuiGameState *state,
                                           TuiSlashCommandId id) {
  switch (id) {
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
  default:
    return NULL;
  }
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
  case TUI_SLASH_RESUME:
  case TUI_SLASH_SIM:
    pthread_mutex_lock(&state->mutex);
    tui_analysis_worker_start(state, state->history_cursor,
                              id == TUI_SLASH_SIM);
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
  case TUI_SLASH_SET:
  case TUI_SLASH_COUNT:
  default:
    break;
  }
}

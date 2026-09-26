#ifndef TUI_COMMANDS_H
#define TUI_COMMANDS_H

#include "game_state.h"
#include "slash_commands.h"
#include "tui_ui_state.h"

// Running a slash command, whether typed in the command bar or picked
// from a panel menu. "/set" takes arguments and runs from the command
// bar alone.

// Why command `id` can't run right now, or NULL when it can. Caller
// holds state->mutex.
const char *tui_command_unavailable_reason(const TuiGameState *state,
                                           TuiSlashCommandId id);

// Runs command `id`, or shows why it can't run in the status bar.
// Caller doesn't hold state->mutex.
void tui_command_run(TuiGameState *state, TuiUiState *ui,
                     const TuiSession *session, TuiSlashCommandId id);

// "/save [path]": writes the game as GCG to `path` ("~/" means the home
// directory), or to a fresh magpie-<time>.gcg in the working directory
// when it's NULL or empty, and says where in the status bar. Caller doesn't
// hold state->mutex.
void tui_command_save(TuiGameState *state, const char *path);

#endif

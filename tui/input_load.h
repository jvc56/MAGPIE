#ifndef TUI_INPUT_LOAD_H
#define TUI_INPUT_LOAD_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Load game modal keys (text editing; Enter commits the parsed game).
// Returns true when the key was consumed.
bool tui_input_load_game(TuiGameState *state, TuiUiState *ui,
                         TuiSession *session, uint32_t key, ncinput input);

// Load position modal keys (text editing; Enter commits the parsed position).
// Returns true when the key was consumed.
bool tui_input_load_position(TuiGameState *state, TuiUiState *ui,
                             TuiSession *session, uint32_t key, ncinput input);

// Re-parse the Load game text when it changed: resolve a path or raw GCG,
// import it into the History, and record the parse result / error for
// the modal.
void tui_load_game_live_parse(TuiGameState *state, TuiUiState *ui);

// Re-parse the Load position text when it changed: resolve a path or raw
// CGP, preview the position behind the modal, and record the parse result /
// error.
void tui_load_position_live_parse(TuiGameState *state, TuiUiState *ui);

#endif

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

#endif

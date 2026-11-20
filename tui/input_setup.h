#ifndef TUI_INPUT_SETUP_H
#define TUI_INPUT_SETUP_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Play-vs-computer setup modal keys.
// Returns true when the key was consumed.
bool tui_input_play_setup(TuiGameState *state, TuiUiState *ui,
                          TuiSession *session, uint32_t key, ncinput input);

// Annotate-game setup modal keys.
// Returns true when the key was consumed.
bool tui_input_annotate_setup(TuiGameState *state, TuiUiState *ui,
                              TuiSession *session, uint32_t key, ncinput input);

// Watch-game setup modal keys.
// Returns true when the key was consumed.
bool tui_input_watch_setup(TuiGameState *state, TuiUiState *ui,
                           TuiSession *session, uint32_t key, ncinput input);

#endif

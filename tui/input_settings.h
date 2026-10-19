#ifndef TUI_INPUT_SETTINGS_H
#define TUI_INPUT_SETTINGS_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Settings modal keys.
// Returns true when the key was consumed.
bool tui_input_settings(TuiGameState *state, TuiUiState *ui,
                        TuiSession *session, uint32_t key, ncinput input);

#endif

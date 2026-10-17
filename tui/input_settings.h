#ifndef TUI_INPUT_SETTINGS_H
#define TUI_INPUT_SETTINGS_H

#include "game_state.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Opens the Settings dialog on its first setting; Esc goes back to
// `return_to`.
void tui_open_settings(TuiUiState *ui, TuiModalState return_to);

// Settings modal keys.
// Returns true when the key was consumed.
bool tui_input_settings(TuiGameState *state, TuiUiState *ui,
                        TuiSession *session, uint32_t key, ncinput input);

#endif

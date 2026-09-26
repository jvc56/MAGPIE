#ifndef TUI_INPUT_MENUS_H
#define TUI_INPUT_MENUS_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Why analysis-menu `item` can't run now (see
// tui_analysis_unavailable_reason), or NULL when it can. Back always can.
// Caller holds state->mutex.
const char *tui_analysis_menu_reason(const TuiGameState *state, int item);

// The analysis panel's menu: Space or Enter on the [5] badge opens it
// for the turn selected in History. Returns true when the key was
// consumed.
bool tui_input_analysis_menu(TuiGameState *state, TuiUiState *ui, uint32_t key,
                             ncinput input);

// Quit-confirm modal keys.
// Returns true when the key was consumed.
bool tui_input_quit_confirm(TuiGameState *state, TuiUiState *ui,
                            TuiSession *session, uint32_t key, ncinput input);

// Time picker modal keys (starts a new Watch game on Enter).
// Returns true when the key was consumed.
bool tui_input_time_picker(TuiGameState *state, TuiUiState *ui,
                           TuiSession *session, uint32_t key, ncinput input);

// Main menu (Esc) modal keys.
// Returns true when the key was consumed.
bool tui_input_main_menu(TuiGameState *state, TuiUiState *ui,
                         TuiSession *session, uint32_t key, ncinput input);

// Startup menu modal keys.
// Returns true when the key was consumed.
bool tui_input_startup_menu(TuiGameState *state, TuiUiState *ui,
                            const TuiSession *session, uint32_t key,
                            ncinput input);

#endif

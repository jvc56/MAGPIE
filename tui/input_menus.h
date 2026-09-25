#ifndef TUI_INPUT_MENUS_H
#define TUI_INPUT_MENUS_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

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

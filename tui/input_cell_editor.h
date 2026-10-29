#ifndef TUI_INPUT_CELL_EDITOR_H
#define TUI_INPUT_CELL_EDITOR_H

#include "game_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Key handling while a History cell editor (or its board move-entry
// sub-mode) is open. Returns true when the key was consumed.
bool tui_input_cell_editor(TuiGameState *state, uint32_t key, ncinput input);

#endif

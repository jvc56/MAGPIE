#ifndef TUI_INPUT_MOUSE_H
#define TUI_INPUT_MOUSE_H

#include "game_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Mouse handling on the game screen: wheel scrolling of the Analysis panel,
// Analysis scrollbar click / drag, and click-to-focus / click-to-select on
// the panels. Returns true when the event was consumed.
bool tui_input_mouse(TuiGameState *state, struct ncplane *std_plane,
                     TuiModalState modal, uint32_t key, ncinput input);

#endif

#ifndef TUI_TUI_CLIPBOARD_H
#define TUI_TUI_CLIPBOARD_H

#include "game_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

void tui_copy_position_cgp(TuiGameState *gs);

// Copies the game so far to the clipboard as GCG.
void tui_copy_game_gcg(TuiGameState *gs);

#endif

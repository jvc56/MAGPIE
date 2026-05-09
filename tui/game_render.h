#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include <notcurses/notcurses.h>
#include "game_state.h"
#include "theme.h"

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds);

#endif

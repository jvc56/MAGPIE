#ifndef TUI_RENDER_HISTORY_H
#define TUI_RENDER_HISTORY_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

void render_history_panel(struct ncplane *plane, const Theme *theme,
                          const TuiGameState *state, const Layout *L);

#endif

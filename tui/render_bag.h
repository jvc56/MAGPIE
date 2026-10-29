#ifndef TUI_RENDER_BAG_H
#define TUI_RENDER_BAG_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>

void render_bag_panel(struct ncplane *plane, const Theme *theme,
                      const TuiGameState *state, const Layout *L);

#endif

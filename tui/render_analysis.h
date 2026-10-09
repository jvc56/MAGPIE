#ifndef TUI_RENDER_ANALYSIS_H
#define TUI_RENDER_ANALYSIS_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

void render_analysis_panel(struct ncplane *plane, const Theme *theme,
                           TuiGameState *state, const Layout *L);

#endif

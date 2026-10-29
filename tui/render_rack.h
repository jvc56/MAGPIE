#ifndef TUI_RENDER_RACK_H
#define TUI_RENDER_RACK_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>

void invalidate_rack_tile_planes(void);
void render_rack_panel(struct ncplane *plane, const Theme *theme,
                       const TuiGameState *state, const Layout *L);
unsigned long tui_debug_rack_blits(void);

// Mark the 2x rack composite stale (next render re-blits).
void render_rack_invalidate_blit_cache(void);

#endif

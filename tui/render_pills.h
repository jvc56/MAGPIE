#ifndef TUI_RENDER_PILLS_H
#define TUI_RENDER_PILLS_H

#include "game_state.h"
#include "render_layout.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

void draw_combined_pills_history_frame(struct ncplane *plane,
                                       const Theme *theme,
                                       const TuiGameState *state,
                                       const Layout *L, bool focused);
void render_player_pill(struct ncplane *plane, const Theme *theme,
                        const TuiGameState *state, int player_idx, int top,
                        int left, int right, bool halfwidth,
                        bool draw_box_around);

#endif

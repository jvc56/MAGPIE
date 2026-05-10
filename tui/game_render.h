#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include <notcurses/notcurses.h>
#include "game_state.h"
#include "theme.h"

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds);

// Menu modal — rendered on top of the game frame when the user presses Esc.
typedef enum {
  TUI_MENU_RESUME = 0,
  TUI_MENU_QUIT = 1,
  TUI_MENU_ITEM_COUNT = 2,
} TuiMenuItem;

void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus);

#endif

#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include <notcurses/notcurses.h>
#include "game_state.h"
#include "theme.h"

// Which modal (if any) is currently open. Drives both rendering of the
// modal itself and what control hints the status bar advertises.
typedef enum {
  TUI_MODAL_NONE = 0,
  TUI_MODAL_MAIN_MENU = 1,
  TUI_MODAL_SETTINGS = 2,
} TuiModalState;

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal);

// Main menu modal — rendered on top of the game frame when the user
// presses Esc.
typedef enum {
  TUI_MENU_RESUME = 0,
  TUI_MENU_SETTINGS = 1,
  TUI_MENU_QUIT = 2,
  TUI_MENU_ITEM_COUNT = 3,
} TuiMenuItem;

void tui_game_render_menu(struct ncplane *plane, const Theme *theme, int focus);

// Settings modal — opened from the main menu. Left/Right arrows on the
// focused row adjust that setting's value.
typedef enum {
  TUI_SETTINGS_BORDER = 0,
  TUI_SETTINGS_BLANKS = 1,
  TUI_SETTINGS_BACK = 2,
  TUI_SETTINGS_ITEM_COUNT = 3,
} TuiSettingsItem;

// `border_thickness` is the current pixel-grid thickness (0..6).
// `pixel_supported` is true when the host terminal can render pixel
// graphics. `blank_uppercase` controls whether played blanks render
// uppercase (with blank_tile_fg) or lowercase (with the regular tile_fg).
void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int border_thickness,
                              bool pixel_supported, bool blank_uppercase);

#endif

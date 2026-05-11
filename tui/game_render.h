#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include "game_state.h"
#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>

struct Game;
struct LetterDistribution;

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
  TUI_MENU_SETTINGS = 0,
  TUI_MENU_QUIT = 1,
  TUI_MENU_BACK = 2,
  TUI_MENU_ITEM_COUNT = 3,
} TuiMenuItem;

void tui_game_render_menu(struct ncplane *plane, const Theme *theme, int focus);

// Settings modal — opened from the main menu. Left/Right arrows on the
// focused row adjust that setting's value.
typedef enum {
  TUI_SETTINGS_SCALE = 0,
  TUI_SETTINGS_AA = 1,
  TUI_SETTINGS_BORDER = 2,
  TUI_SETTINGS_PREMIUM = 3,
  TUI_SETTINGS_BLANKS = 4,
  TUI_SETTINGS_BACK = 5,
  TUI_SETTINGS_ITEM_COUNT = 6,
} TuiSettingsItem;

// `board_scale` is 1 or 2; the scale row is grayed out when 2x is
// unavailable (no pixel support or font load failed). `antialias`
// applies to the 2x render only and is grayed at 1x. `border_thickness`
// is the current pixel-grid thickness (0..6). `pixel_supported` is true
// when the host terminal can render pixel graphics. `font_available` is
// true when the bundled TTF loaded. `premium_labels` selects the
// TW/tw/none labeling style for premium squares. `blank_uppercase`
// controls whether played blanks render uppercase (with blank_tile_fg)
// or lowercase (with tile_fg).
void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase);

// Render just the board cells (no row/col labels) at (top, left). Each
// cell is 2 columns wide; the rendered region is BOARD_DIM rows tall and
// BOARD_DIM*2 columns wide. When the host terminal supports pixel
// graphics and `border_thickness` is positive, a grid overlay is drawn
// on top using a private cached child plane.
void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const struct Game *game,
                         const struct LetterDistribution *ld,
                         bool blank_uppercase, TuiPremiumLabels premium_labels,
                         int border_thickness);

// Destroy any cached pixel-grid child planes (board, rack, both pills,
// preview). Call after the theme picker exits so the preview's grid
// doesn't linger under the in-game UI, and on resize so a font-size
// change rebuilds them at the new cell-to-pixel ratio.
void tui_game_render_reset_grids(void);

#endif

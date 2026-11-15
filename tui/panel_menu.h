#ifndef TUI_PANEL_MENU_H
#define TUI_PANEL_MENU_H

#include "game_state.h"
#include "settings_table.h"
#include "slash_commands.h"
#include "theme.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Each panel's context menu, opened with Space (or Enter) on the panel's
// badge or a right-click on the panel: the panel's commands, then its
// settings (changed with Left/Right, as in Settings), then Back. Items
// that can't be used right now stay focusable, dimmed, and the help line
// says why.

typedef enum {
  TUI_PANEL_MENU_ROW_COMMAND,
  TUI_PANEL_MENU_ROW_HEADING,
  TUI_PANEL_MENU_ROW_SETTING,
  TUI_PANEL_MENU_ROW_BACK,
} TuiPanelMenuRowKind;

typedef struct {
  TuiPanelMenuRowKind kind;
  TuiSlashCommandId command; // COMMAND
  TuiSettingId setting;      // SETTING
  const char *heading;       // HEADING
} TuiPanelMenuRow;

enum { TUI_PANEL_MENU_MAX_ROWS = TUI_SLASH_COUNT + TUI_SETTING_COUNT + 3 };

// The rows of panel `panel`'s menu (1 Board ... 5 Analysis). Returns the
// row count.
int tui_panel_menu_rows(int panel,
                        TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS]);

// Opens panel `panel`'s menu, focused on its first usable item.
void tui_open_panel_menu(TuiGameState *state, TuiUiState *ui,
                         const TuiSession *session, int panel);

// Panel-menu keys and clicks. Returns true when consumed.
bool tui_input_panel_menu(TuiGameState *state, TuiUiState *ui,
                          TuiSession *session, uint32_t key, ncinput input);

// The focused row's help line: what it does, or why it can't be used
// now. Caller holds state->mutex.
const char *tui_panel_menu_help(const TuiGameState *state, const TuiUiState *ui,
                                const TuiSession *session);

// Draws the open panel menu. Takes state->mutex.
void tui_render_panel_menu(struct ncplane *plane, const Theme *theme,
                           TuiGameState *state, TuiUiState *ui,
                           const TuiSession *session);

#endif

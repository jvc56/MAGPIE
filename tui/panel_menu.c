#include "panel_menu.h"

#include "commands.h"
#include "game_state.h"
#include "list_nav.h"
#include "render_hit_test.h"
#include "render_modals.h"
#include "settings_table.h"
#include "slash_commands.h"
#include "theme.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { PANEL_COUNT = 6, MAX_PANEL_COMMANDS = 6 };

// Each panel's commands, in menu order.
static const TuiSlashCommandId panel_commands[PANEL_COUNT][MAX_PANEL_COMMANDS] =
    {
        [1] = {TUI_SLASH_COPY},
        [4] = {TUI_SLASH_PLAY_FROM, TUI_SLASH_WATCH_FROM, TUI_SLASH_SAVE,
               TUI_SLASH_COPY_GCG},
        [5] = {TUI_SLASH_SIM, TUI_SLASH_KIBITZ, TUI_SLASH_RESUME,
               TUI_SLASH_STOP},
};
static const int panel_command_counts[PANEL_COUNT] = {
    [1] = 1, [4] = 4, [5] = 4};

int tui_panel_menu_rows(int panel,
                        TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS]) {
  int row_count = 0;
  if (panel < 1 || panel >= PANEL_COUNT) {
    rows[row_count++] = (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_BACK};
    return row_count;
  }
  for (int cmd_idx = 0; cmd_idx < panel_command_counts[panel]; cmd_idx++) {
    rows[row_count++] =
        (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_COMMAND,
                          .command = panel_commands[panel][cmd_idx]};
  }
  const bool has_commands = row_count > 0;
  int def_count = 0;
  const TuiSettingDef *defs = tui_setting_defs(&def_count);
  bool has_settings = false;
  for (int def_idx = 0; def_idx < def_count; def_idx++) {
    if (defs[def_idx].panel != panel) {
      continue;
    }
    if (!has_settings && has_commands) {
      rows[row_count++] = (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_HEADING,
                                            .heading = "Settings"};
    }
    has_settings = true;
    rows[row_count++] = (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_SETTING,
                                          .setting = defs[def_idx].id};
  }
  if (!has_commands && !has_settings) {
    rows[row_count++] = (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_HEADING,
                                          .heading = "Nothing here yet"};
  }
  rows[row_count++] = (TuiPanelMenuRow){.kind = TUI_PANEL_MENU_ROW_BACK};
  return row_count;
}

// Why row `row` can't be used now, or NULL. Caller holds state->mutex.
static const char *row_unavailable_reason(const TuiGameState *state,
                                          const TuiSession *session,
                                          const TuiPanelMenuRow *row) {
  switch (row->kind) {
  case TUI_PANEL_MENU_ROW_COMMAND:
    return tui_command_unavailable_reason(state, row->command);
  case TUI_PANEL_MENU_ROW_SETTING:
    return tui_setting_unavailable_reason(state, session, row->setting);
  default:
    return NULL;
  }
}

void tui_open_panel_menu(TuiGameState *state, TuiUiState *ui,
                         const TuiSession *session, int panel) {
  TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS];
  const int row_count = tui_panel_menu_rows(panel, rows);
  ui->modal = TUI_MODAL_PANEL_MENU;
  ui->panel_menu_panel = panel;
  ui->panel_menu_scroll = 0;
  ui->panel_menu_focus = row_count - 1; // Back
  pthread_mutex_lock(&state->mutex);
  for (int row_idx = 0; row_idx < row_count; row_idx++) {
    if (rows[row_idx].kind != TUI_PANEL_MENU_ROW_HEADING &&
        row_unavailable_reason(state, session, &rows[row_idx]) == NULL) {
      ui->panel_menu_focus = row_idx;
      break;
    }
  }
  pthread_mutex_unlock(&state->mutex);
}

bool tui_input_panel_menu(TuiGameState *state, TuiUiState *ui,
                          TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal != TUI_MODAL_PANEL_MENU) {
    return false;
  }
  TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS];
  const int row_count = tui_panel_menu_rows(ui->panel_menu_panel, rows);
  bool focusable[TUI_PANEL_MENU_MAX_ROWS];
  bool usable[TUI_PANEL_MENU_MAX_ROWS];
  pthread_mutex_lock(&state->mutex);
  for (int row_idx = 0; row_idx < row_count; row_idx++) {
    focusable[row_idx] = rows[row_idx].kind != TUI_PANEL_MENU_ROW_HEADING;
    usable[row_idx] =
        focusable[row_idx] &&
        row_unavailable_reason(state, session, &rows[row_idx]) == NULL;
  }
  pthread_mutex_unlock(&state->mutex);
  // A click on a command or Back picks it; on a setting it focuses it,
  // and on the focused setting's ◀ / ▶ steps its value.
  if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
    const int hit = tui_modal_item_at(input.y, input.x);
    const int row = hit < 0 ? -1 : hit + ui->panel_menu_scroll;
    if (row < 0 || row >= row_count || !focusable[row]) {
      return true;
    }
    ui->panel_menu_focus = row;
    const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
    if (chev == TUI_MODAL_CHEVRON_LEFT) {
      key = NCKEY_LEFT;
    } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
      key = NCKEY_RIGHT;
    } else if (rows[row].kind != TUI_PANEL_MENU_ROW_SETTING) {
      key = NCKEY_ENTER;
    } else {
      return true;
    }
  }
  const int focus = ui->panel_menu_focus;
  const int nav = tui_list_nav(key, &input, focus, row_count, focusable,
                               TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
  if (nav >= 0) {
    ui->panel_menu_focus = nav;
    return true;
  }
  if (focus < 0 || focus >= row_count) {
    return true;
  }
  const TuiPanelMenuRow *row = &rows[focus];
  if (key == NCKEY_ESC) {
    ui->modal = TUI_MODAL_NONE;
  } else if (key == NCKEY_LEFT || key == 'h' || key == 'H' ||
             key == NCKEY_RIGHT || key == 'l' || key == 'L') {
    if (row->kind == TUI_PANEL_MENU_ROW_SETTING && usable[focus]) {
      const int dir = (key == NCKEY_LEFT || key == 'h' || key == 'H') ? -1 : 1;
      tui_setting_step(state, session, row->setting, dir);
    }
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    if (row->kind == TUI_PANEL_MENU_ROW_BACK) {
      ui->modal = TUI_MODAL_NONE;
    } else if (row->kind == TUI_PANEL_MENU_ROW_COMMAND && usable[focus]) {
      // Close first: the command may open a dialog of its own.
      ui->modal = TUI_MODAL_NONE;
      tui_command_run(state, ui, session, row->command);
    }
  }
  return true;
}

const char *tui_panel_menu_help(const TuiGameState *state, const TuiUiState *ui,
                                const TuiSession *session) {
  TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS];
  const int row_count = tui_panel_menu_rows(ui->panel_menu_panel, rows);
  const int focus = ui->panel_menu_focus;
  if (focus < 0 || focus >= row_count) {
    return NULL;
  }
  const TuiPanelMenuRow *row = &rows[focus];
  const char *reason = row_unavailable_reason(state, session, row);
  if (reason != NULL) {
    return reason;
  }
  switch (row->kind) {
  case TUI_PANEL_MENU_ROW_COMMAND:
    return tui_slash_command(row->command)->desc;
  case TUI_PANEL_MENU_ROW_SETTING:
    return tui_setting_def(row->setting)->help;
  case TUI_PANEL_MENU_ROW_BACK:
    return "Close this menu.";
  default:
    return NULL;
  }
}

void tui_render_panel_menu(struct ncplane *plane, const Theme *theme,
                           TuiGameState *state, TuiUiState *ui,
                           const TuiSession *session) {
  TuiPanelMenuRow rows[TUI_PANEL_MENU_MAX_ROWS];
  const int row_count = tui_panel_menu_rows(ui->panel_menu_panel, rows);
  const char *labels[TUI_PANEL_MENU_MAX_ROWS] = {NULL};
  const char *values[TUI_PANEL_MENU_MAX_ROWS] = {NULL};
  char value_bufs[TUI_PANEL_MENU_MAX_ROWS][32];
  bool heading[TUI_PANEL_MENU_MAX_ROWS] = {false};
  bool unavailable[TUI_PANEL_MENU_MAX_ROWS] = {false};
  pthread_mutex_lock(&state->mutex);
  const int turn = state->history_cursor;
  const bool sim_continues = turn >= 0 && turn < state->history_count &&
                             state->history[turn].sim_results_saved != NULL;
  for (int row_idx = 0; row_idx < row_count; row_idx++) {
    const TuiPanelMenuRow *row = &rows[row_idx];
    heading[row_idx] = row->kind == TUI_PANEL_MENU_ROW_HEADING;
    unavailable[row_idx] = row_unavailable_reason(state, session, row) != NULL;
    switch (row->kind) {
    case TUI_PANEL_MENU_ROW_COMMAND:
      labels[row_idx] = row->command == TUI_SLASH_SIM && sim_continues
                            ? "Continue sim"
                            : tui_slash_command(row->command)->menu_label;
      break;
    case TUI_PANEL_MENU_ROW_HEADING:
      labels[row_idx] = row->heading;
      break;
    case TUI_PANEL_MENU_ROW_SETTING: {
      const TuiSettingDef *def = tui_setting_def(row->setting);
      labels[row_idx] = def->label;
      tui_setting_format(def, tui_setting_get(state, row->setting),
                         value_bufs[row_idx], sizeof(value_bufs[row_idx]));
      values[row_idx] = value_bufs[row_idx];
      break;
    }
    case TUI_PANEL_MENU_ROW_BACK:
    default:
      labels[row_idx] = "Back";
      break;
    }
  }
  pthread_mutex_unlock(&state->mutex);
  tui_game_render_settings(plane, theme,
                           tui_setting_panel_title(ui->panel_menu_panel),
                           labels, values, heading, unavailable, row_count,
                           ui->panel_menu_focus, &ui->panel_menu_scroll);
}

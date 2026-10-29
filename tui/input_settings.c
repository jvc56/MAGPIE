#include "input_settings.h"

#include "game_state.h"
#include "list_nav.h"
#include "render_hit_test.h"
#include "settings_table.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void tui_open_settings(TuiUiState *ui, TuiModalState return_to) {
  TuiSettingsRow rows[TUI_SETTINGS_MAX_ROWS];
  const int row_count = tui_settings_rows(rows);
  ui->modal = TUI_MODAL_SETTINGS;
  ui->settings_return = return_to;
  ui->settings_scroll = 0;
  ui->settings_focus = 0;
  while (ui->settings_focus < row_count - 1 &&
         rows[ui->settings_focus].kind == TUI_SETTINGS_ROW_HEADING) {
    ui->settings_focus++;
  }
}

// Settings modal keys: Up/Down move between settings (headings are
// skipped), Left/Right change the focused one, Enter on Back or Esc
// closes. Returns true when the key was consumed.
bool tui_input_settings(TuiGameState *state, TuiUiState *ui,
                        TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal != TUI_MODAL_SETTINGS) {
    return false;
  }
  TuiSettingsRow rows[TUI_SETTINGS_MAX_ROWS];
  const int row_count = tui_settings_rows(rows);
  // Headings can't take focus. Unavailable settings can, so the help
  // line can say why they're unavailable; Left/Right skip changing them.
  bool focusable[TUI_SETTINGS_MAX_ROWS];
  bool adjustable[TUI_SETTINGS_MAX_ROWS];
  pthread_mutex_lock(&state->mutex);
  for (int row_idx = 0; row_idx < row_count; row_idx++) {
    focusable[row_idx] = rows[row_idx].kind != TUI_SETTINGS_ROW_HEADING;
    adjustable[row_idx] = rows[row_idx].kind == TUI_SETTINGS_ROW_SETTING &&
                          tui_setting_unavailable_reason(
                              state, session, rows[row_idx].id) == NULL;
  }
  pthread_mutex_unlock(&state->mutex);
  // Click on a setting selects it; a click on the focused row's ◀ / ▶
  // steps its value, and one on Back closes the dialog.
  if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
    const int hit = tui_modal_item_at(input.y, input.x);
    const int row = hit < 0 ? -1 : hit + ui->settings_scroll;
    if (row < 0 || row >= row_count || !focusable[row]) {
      return true;
    }
    const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
    ui->settings_focus = row;
    if (chev == TUI_MODAL_CHEVRON_LEFT) {
      key = NCKEY_LEFT;
    } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
      key = NCKEY_RIGHT;
    } else if (rows[row].kind == TUI_SETTINGS_ROW_BACK) {
      key = NCKEY_ENTER;
    } else {
      return true;
    }
  }
  const int focus = ui->settings_focus;
  const int nav = tui_list_nav(key, &input, focus, row_count, focusable,
                               TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
  if (nav >= 0) {
    ui->settings_focus = nav;
  } else if (key == NCKEY_ESC) {
    // Esc returns to whichever modal opened Settings: the main menu, or
    // no modal when opened from the command bar.
    ui->modal = ui->settings_return;
  } else if (key == NCKEY_LEFT || key == 'h' || key == 'H' ||
             key == NCKEY_RIGHT || key == 'l' || key == 'L') {
    if (focus >= 0 && focus < row_count && adjustable[focus]) {
      const int dir = (key == NCKEY_LEFT || key == 'h' || key == 'H') ? -1 : 1;
      tui_setting_step(state, session, rows[focus].id, dir);
    }
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    if (focus >= 0 && focus < row_count &&
        rows[focus].kind == TUI_SETTINGS_ROW_BACK) {
      ui->modal = ui->settings_return;
    }
  }
  return true;
}

#include "input_settings.h"

#include "config.h"
#include "game_state.h"
#include "list_nav.h"
#include "render_hit_test.h"
#include "render_modals.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Steps the focused Settings row's value by `dir` (-1 for Left, +1 for
// Right) and saves it to the config. Two-state rows toggle either way;
// multi-state rows cycle; Border clamps to 0..6 px. RIT is deferred:
// it's written to the config and takes effect on the next New Game (the
// pending-change banner shows the divergence until then).
static void settings_adjust(TuiGameState *state, const TuiUiState *ui,
                            TuiSession *session, int dir) {
  const bool save = !session->args.no_config;
  TuiConfig *cfg = &session->to_save;
  pthread_mutex_lock(&state->mutex);
  switch (ui->settings_focus) {
  case TUI_SETTINGS_SCALE:
    state->board_scale = state->board_scale == 2 ? 1 : 2;
    cfg->board_scale = state->board_scale;
    cfg->board_scale_set = true;
    break;
  case TUI_SETTINGS_AA:
    state->antialias = !state->antialias;
    cfg->antialias = state->antialias;
    cfg->antialias_set = true;
    break;
  case TUI_SETTINGS_SUBSCRIPTS:
    state->score_subscripts =
        (TuiScoreSubscripts)((state->score_subscripts +
                              TUI_SCORE_SUBSCRIPTS_COUNT + dir) %
                             TUI_SCORE_SUBSCRIPTS_COUNT);
    cfg->score_subscripts = state->score_subscripts;
    cfg->score_subscripts_set = true;
    break;
  case TUI_SETTINGS_BORDER:
    if ((dir < 0 && state->border_thickness > 0) ||
        (dir > 0 && state->border_thickness < 6)) {
      state->border_thickness += dir;
    }
    cfg->border_thickness = state->border_thickness;
    cfg->border_thickness_set = true;
    break;
  case TUI_SETTINGS_PREMIUM:
    state->premium_labels =
        (TuiPremiumLabels)((state->premium_labels + TUI_PREMIUM_LABELS_COUNT +
                            dir) %
                           TUI_PREMIUM_LABELS_COUNT);
    cfg->premium_labels = state->premium_labels;
    cfg->premium_labels_set = true;
    break;
  case TUI_SETTINGS_BLANKS:
    state->blank_uppercase = !state->blank_uppercase;
    cfg->blank_uppercase = state->blank_uppercase;
    cfg->blank_uppercase_set = true;
    break;
  case TUI_SETTINGS_RACK_SORT:
    state->rack_sort =
        (TuiRackSort)((state->rack_sort + TUI_RACK_SORT_COUNT + dir) %
                      TUI_RACK_SORT_COUNT);
    cfg->rack_sort = state->rack_sort;
    cfg->rack_sort_set = true;
    break;
  case TUI_SETTINGS_RIT: {
    const bool prev =
        cfg->load_rit_set ? cfg->load_rit : session->initial_load_rit;
    cfg->load_rit = !prev;
    cfg->load_rit_set = true;
    state->pending_load_rit = cfg->load_rit;
    break;
  }
  default:
    pthread_mutex_unlock(&state->mutex);
    return;
  }
  pthread_mutex_unlock(&state->mutex);
  if (save) {
    tui_config_save(cfg);
  }
}

// Settings modal keys.
// Returns true when the key was consumed.
bool tui_input_settings(TuiGameState *state, TuiUiState *ui,
                        TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal != TUI_MODAL_SETTINGS) {
    return false;
  }
  // Click on a settings row selects it. Settings uses ←/→ to adjust
  // values rather than Enter, so a click just changes focus, except on
  // the ◀ / ▶ chevrons of the focused row (which step the value) and on
  // "Back" (which closes the modal).
  if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
    const int hit = tui_modal_item_at(input.y, input.x);
    if (hit < 0) {
      return true;
    }
    const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
    ui->settings_focus = hit;
    if (chev == TUI_MODAL_CHEVRON_LEFT) {
      key = NCKEY_LEFT;
    } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
      key = NCKEY_RIGHT;
    } else if (hit == TUI_SETTINGS_BACK) {
      key = NCKEY_ENTER;
    } else {
      return true;
    }
  }
  bool enabled[TUI_SETTINGS_ITEM_COUNT];
  tui_settings_enabled_rows(state->board_scale, session->pixel_supported,
                            session->font_available, enabled);
  const int nav =
      tui_list_nav(key, &input, ui->settings_focus, TUI_SETTINGS_ITEM_COUNT,
                   enabled, TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
  if (nav >= 0) {
    ui->settings_focus = nav;
  } else if (key == NCKEY_ESC) {
    // Esc returns to whichever modal opened Settings: the main menu, or
    // no modal when opened from the command bar.
    ui->modal = ui->settings_return;
  } else if (key == NCKEY_LEFT || key == 'h' || key == 'H' ||
             key == NCKEY_RIGHT || key == 'l' || key == 'L') {
    if (enabled[ui->settings_focus]) {
      const int dir = (key == NCKEY_LEFT || key == 'h' || key == 'H') ? -1 : 1;
      settings_adjust(state, ui, session, dir);
      // A changed value can alter the board's look; invalidate the
      // pixel-blit caches keyed on render_version.
      atomic_fetch_add(&state->render_version, 1);
    }
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    if (ui->settings_focus == TUI_SETTINGS_BACK) {
      ui->modal = ui->settings_return;
    }
  }
  return true;
}

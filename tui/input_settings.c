#include "input_settings.h"

#include "render_hit_test.h"
#include <pthread.h>

// Settings modal keys.
// Returns true when the key was consumed.
bool tui_input_settings(TuiGameState *state, TuiUiState *ui,
                        TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_SETTINGS) {
    // Click on a settings row: select that row. Settings uses
    // ←/→ to adjust values rather than Enter, so a click just
    // changes focus — it doesn't trigger a value change. The
    // "Back" row is the exception; for it we synthesize an
    // Enter so the click commits. A click on the ◀ / ▶
    // chevrons of an already-focused row synthesizes ← / →
    // so the adjuster fires.
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0) {
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
      } else {
        return true;
      }
    }
    // Any left/right keystroke at this modal might mutate visual
    // state (scale, AA, border, premium labels, blanks). Bumping
    // render_version once at the top invalidates the pixel-blit
    // caches that key off it, regardless of which branch below
    // actually toggled something — cheap and avoids scattering
    // atomic_fetch_add through every handler.
    if (key == NCKEY_LEFT || key == 'h' || key == 'H' || key == NCKEY_RIGHT ||
        key == 'l' || key == 'L') {
      atomic_fetch_add(&state->render_version, 1);
    }
    if (key == NCKEY_ESC) {
      // Esc returns to whichever modal opened Settings — main menu
      // when reached via Esc → Settings (so the user can navigate
      // to another menu entry without re-opening from scratch), or
      // back to no modal when reached via the command-bar S
      // shortcut.
      ui->modal = ui->settings_return;
    } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
      const bool effective_2x = session->pixel_supported &&
                                session->font_available &&
                                state->board_scale >= 2;
      int idx = ui->settings_focus - 1;
      while (idx > 0 && SETTINGS_2X_ONLY(idx) && !effective_2x) {
        idx--;
      }
      if (idx >= 0) {
        ui->settings_focus = idx;
      }
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      const bool effective_2x = session->pixel_supported &&
                                session->font_available &&
                                state->board_scale >= 2;
      int idx = ui->settings_focus + 1;
      while (idx < TUI_SETTINGS_ITEM_COUNT - 1 && SETTINGS_2X_ONLY(idx) &&
             !effective_2x) {
        idx++;
      }
      if (idx < TUI_SETTINGS_ITEM_COUNT) {
        ui->settings_focus = idx;
      }
    } else if (key == NCKEY_LEFT || key == 'h' || key == 'H') {
      if (ui->settings_focus == TUI_SETTINGS_SCALE &&
          session->pixel_supported && session->font_available) {
        // Scale is a 2-state toggle (1, 2). Both arrows flip it.
        pthread_mutex_lock(&state->mutex);
        state->board_scale = state->board_scale == 2 ? 1 : 2;
        const int v = state->board_scale;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.board_scale = v;
          session->to_save.board_scale_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_AA &&
                 session->pixel_supported && session->font_available &&
                 state->board_scale >= 2) {
        pthread_mutex_lock(&state->mutex);
        state->antialias = !state->antialias;
        const bool v = state->antialias;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.antialias = v;
          session->to_save.antialias_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                 session->pixel_supported && session->font_available &&
                 state->board_scale >= 2) {
        pthread_mutex_lock(&state->mutex);
        state->score_subscripts =
            (TuiScoreSubscripts)((state->score_subscripts +
                                  TUI_SCORE_SUBSCRIPTS_COUNT - 1) %
                                 TUI_SCORE_SUBSCRIPTS_COUNT);
        const TuiScoreSubscripts v = state->score_subscripts;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.score_subscripts = v;
          session->to_save.score_subscripts_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_BORDER &&
                 session->pixel_supported) {
        pthread_mutex_lock(&state->mutex);
        if (state->border_thickness > 0) {
          state->border_thickness--;
        }
        const int v = state->border_thickness;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.border_thickness = v;
          session->to_save.border_thickness_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_PREMIUM) {
        pthread_mutex_lock(&state->mutex);
        state->premium_labels =
            (TuiPremiumLabels)((state->premium_labels +
                                TUI_PREMIUM_LABELS_COUNT - 1) %
                               TUI_PREMIUM_LABELS_COUNT);
        const TuiPremiumLabels v = state->premium_labels;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.premium_labels = v;
          session->to_save.premium_labels_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_BLANKS) {
        // Blanks is a two-state toggle, so left and right both flip it.
        pthread_mutex_lock(&state->mutex);
        state->blank_uppercase = !state->blank_uppercase;
        const bool v = state->blank_uppercase;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.blank_uppercase = v;
          session->to_save.blank_uppercase_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_RACK_SORT) {
        pthread_mutex_lock(&state->mutex);
        int v = (int)state->rack_sort - 1;
        if (v < 0) {
          v = TUI_RACK_SORT_COUNT - 1;
        }
        state->rack_sort = (TuiRackSort)v;
        const TuiRackSort saved = state->rack_sort;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.rack_sort = saved;
          session->to_save.rack_sort_set = true;
          tui_config_save(&session->to_save);
        }
      }
    } else if (key == NCKEY_RIGHT || key == 'l' || key == 'L') {
      if (ui->settings_focus == TUI_SETTINGS_SCALE &&
          session->pixel_supported && session->font_available) {
        pthread_mutex_lock(&state->mutex);
        state->board_scale = state->board_scale == 2 ? 1 : 2;
        const int v = state->board_scale;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.board_scale = v;
          session->to_save.board_scale_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_AA &&
                 session->pixel_supported && session->font_available &&
                 state->board_scale >= 2) {
        pthread_mutex_lock(&state->mutex);
        state->antialias = !state->antialias;
        const bool v = state->antialias;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.antialias = v;
          session->to_save.antialias_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                 session->pixel_supported && session->font_available &&
                 state->board_scale >= 2) {
        pthread_mutex_lock(&state->mutex);
        state->score_subscripts =
            (TuiScoreSubscripts)((state->score_subscripts + 1) %
                                 TUI_SCORE_SUBSCRIPTS_COUNT);
        const TuiScoreSubscripts v = state->score_subscripts;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.score_subscripts = v;
          session->to_save.score_subscripts_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_BORDER &&
                 session->pixel_supported) {
        pthread_mutex_lock(&state->mutex);
        if (state->border_thickness < 6) {
          state->border_thickness++;
        }
        const int v = state->border_thickness;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.border_thickness = v;
          session->to_save.border_thickness_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_PREMIUM) {
        pthread_mutex_lock(&state->mutex);
        state->premium_labels = (TuiPremiumLabels)((state->premium_labels + 1) %
                                                   TUI_PREMIUM_LABELS_COUNT);
        const TuiPremiumLabels v = state->premium_labels;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.premium_labels = v;
          session->to_save.premium_labels_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_BLANKS) {
        pthread_mutex_lock(&state->mutex);
        state->blank_uppercase = !state->blank_uppercase;
        const bool v = state->blank_uppercase;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.blank_uppercase = v;
          session->to_save.blank_uppercase_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_RACK_SORT) {
        pthread_mutex_lock(&state->mutex);
        int v = (int)state->rack_sort + 1;
        if (v >= TUI_RACK_SORT_COUNT) {
          v = 0;
        }
        state->rack_sort = (TuiRackSort)v;
        const TuiRackSort saved = state->rack_sort;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          session->to_save.rack_sort = saved;
          session->to_save.rack_sort_set = true;
          tui_config_save(&session->to_save);
        }
      } else if (ui->settings_focus == TUI_SETTINGS_RIT) {
        // RIT is a deferred toggle — write to config; the live
        // game keeps using whatever was loaded at game-state init.
        // The pending-change banner picks up the divergence and the
        // setting takes effect on the next New Game.
        const bool prev = session->to_save.load_rit_set
                              ? session->to_save.load_rit
                              : session->initial_load_rit;
        session->to_save.load_rit = !prev;
        session->to_save.load_rit_set = true;
        pthread_mutex_lock(&state->mutex);
        state->pending_load_rit = session->to_save.load_rit;
        pthread_mutex_unlock(&state->mutex);
        if (!session->args.no_config) {
          tui_config_save(&session->to_save);
        }
      }
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      if (ui->settings_focus == TUI_SETTINGS_BACK) {
        ui->modal = ui->settings_return;
      }
    }
    return true;
  }
  return false;
}

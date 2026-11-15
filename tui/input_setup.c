#include "input_setup.h"

#include "bot_worker.h"
#include "config.h"
#include "game_render.h"
#include "game_state.h"
#include "lexicon_picker.h"
#include "list_nav.h"
#include "render_hit_test.h"
#include "render_modals.h"
#include "time_picker.h"
#include "tui_session.h"
#include "tui_ui_state.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// Play-vs-computer setup modal keys.
// Returns true when the key was consumed.
bool tui_input_play_setup(TuiGameState *state, TuiUiState *ui,
                          TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_PLAY_SETUP) {
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < TUI_PLAY_SETUP_ITEM_COUNT) {
        const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
        if (hit != ui->play_setup_focus) {
          if (hit == TUI_PLAY_SETUP_HUMAN_NAME) {
            ui->play_setup_name_cursor = (int)strlen(ui->play_setup_human_name);
          } else if (hit == TUI_PLAY_SETUP_COMPUTER_NAME) {
            ui->play_setup_name_cursor =
                (int)strlen(ui->play_setup_computer_name);
          }
        }
        ui->play_setup_focus = hit;
        // Chevrons only render on the focused adjustable row, so a
        // chevron hit always means "adjust this row".
        if (hit == TUI_PLAY_SETUP_START) {
          key = NCKEY_ENTER;
        } else if (chev == TUI_MODAL_CHEVRON_LEFT) {
          key = NCKEY_LEFT;
        } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
          key = NCKEY_RIGHT;
        } else {
          return true;
        }
      } else {
        return true;
      }
    }

    const bool focus_human = ui->play_setup_focus == TUI_PLAY_SETUP_HUMAN_NAME;
    const bool focus_comp =
        ui->play_setup_focus == TUI_PLAY_SETUP_COMPUTER_NAME;
    const bool focus_name = focus_human || focus_comp;
    char *name_buf = NULL;
    size_t name_cap = sizeof(ui->play_setup_computer_name);
    if (focus_human) {
      name_buf = ui->play_setup_human_name;
      name_cap = sizeof(ui->play_setup_human_name);
    } else if (focus_comp) {
      name_buf = ui->play_setup_computer_name;
    }

    if (key == NCKEY_ESC) {
      ui->modal = TUI_MODAL_STARTUP_MENU;
      ui->startup_menu_focus = TUI_STARTUP_PLAY_VS_COMPUTER;
      return true;
    }
    // Cursor navigation skips rows the current overtime rule
    // disables (cap under non-MAX, penalty rate under FLAG).
    bool ps_enabled[TUI_PLAY_SETUP_ITEM_COUNT];
    tui_play_setup_enabled_rows(ui->play_setup_overtime_rule,
                                ui->watch_setup_time,
                                ui->play_setup_challenge_rule, ps_enabled);
    // Home/End move the caret on the name rows, so they only jump rows
    // elsewhere.
    const int nav = tui_list_nav(
        key, &input, ui->play_setup_focus, TUI_PLAY_SETUP_ITEM_COUNT,
        ps_enabled, focus_name ? TUI_LIST_NAV_ARROWS : TUI_LIST_NAV_HOME_END);
    if (nav >= 0) {
      ui->play_setup_focus = nav;
      if (nav == TUI_PLAY_SETUP_HUMAN_NAME) {
        ui->play_setup_name_cursor = (int)strlen(ui->play_setup_human_name);
      } else if (nav == TUI_PLAY_SETUP_COMPUTER_NAME) {
        ui->play_setup_name_cursor = (int)strlen(ui->play_setup_computer_name);
      }
      return true;
    }
    if (!focus_name && (key == NCKEY_LEFT || key == NCKEY_RIGHT)) {
      const int dir = key == NCKEY_RIGHT ? 1 : -1;
      if (ui->play_setup_focus == TUI_PLAY_SETUP_FIRST_PLAYER) {
        ui->play_setup_first_move =
            (ui->play_setup_first_move + dir + TUI_PLAY_FIRST_COUNT) %
            TUI_PLAY_FIRST_COUNT;
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_TIME) {
        const int n = tui_time_picker_preset_count();
        const int cur = tui_time_picker_closest_index(ui->watch_setup_time);
        int next = cur + dir;
        if (next < 0) {
          next = 0;
        }
        if (next >= n) {
          next = n - 1;
        }
        ui->watch_setup_time = tui_time_picker_preset_seconds(next);
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_OVERTIME) {
        // One scale: none, up to 1..10 minutes, unlimited.
        int step = tui_overtime_step(ui->play_setup_overtime_rule,
                                     ui->play_setup_overtime_cap) +
                   dir;
        step = step < 0 ? 0 : step;
        step = step > TUI_OVERTIME_STEP_COUNT - 1 ? TUI_OVERTIME_STEP_COUNT - 1
                                                  : step;
        tui_overtime_from_step(step, &ui->play_setup_overtime_rule,
                               &ui->play_setup_overtime_cap);
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_TIME_PENALTY) {
        if (ui->play_setup_overtime_rule != UI_OVERTIME_FLAG) {
          // Two rates — Left/Right both toggle.
          ui->play_setup_penalty_rate =
              ui->play_setup_penalty_rate == UI_TIME_PENALTY_10_PER_MIN
                  ? UI_TIME_PENALTY_1_PER_SEC
                  : UI_TIME_PENALTY_10_PER_MIN;
        }
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_CHALLENGE) {
        ui->play_setup_challenge_rule =
            (UiChallengeRule)(((int)ui->play_setup_challenge_rule + dir +
                               UI_CHALLENGE_RULE_COUNT) %
                              UI_CHALLENGE_RULE_COUNT);
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_CHALLENGE_PENALTY) {
        if (ui->play_setup_challenge_rule == UI_CHALLENGE_PENALTY) {
          ui->play_setup_challenge_penalty =
              (UiChallengePenalty)(((int)ui->play_setup_challenge_penalty +
                                    dir + UI_CHALLENGE_PENALTY_COUNT) %
                                   UI_CHALLENGE_PENALTY_COUNT);
        }
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_LANGUAGE) {
        if (ui->lexicon_list == NULL) {
          ui->lexicon_list = tui_lexicon_list_load();
        }
        if (ui->lexicon_list != NULL) {
          int cur =
              tui_lexicon_list_find(ui->lexicon_list, ui->watch_setup_lexicon);
          if (cur < 0) {
            cur = 0;
          }
          const int next =
              tui_lexicon_list_step_language(ui->lexicon_list, cur, dir);
          char namebuf[TUI_LEXICON_NAME_MAX];
          if (next != cur && tui_lexicon_list_name(ui->lexicon_list, next,
                                                   namebuf, sizeof(namebuf))) {
            (void)snprintf(ui->watch_setup_lexicon,
                           sizeof(ui->watch_setup_lexicon), "%s", namebuf);
          }
        }
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_LEXICON) {
        if (ui->lexicon_list == NULL) {
          ui->lexicon_list = tui_lexicon_list_load();
        }
        if (ui->lexicon_list != NULL) {
          int cur =
              tui_lexicon_list_find(ui->lexicon_list, ui->watch_setup_lexicon);
          if (cur < 0) {
            cur = 0;
          }
          const int next =
              tui_lexicon_list_step_same_language(ui->lexicon_list, cur, dir);
          char namebuf[TUI_LEXICON_NAME_MAX];
          if (next != cur && tui_lexicon_list_name(ui->lexicon_list, next,
                                                   namebuf, sizeof(namebuf))) {
            (void)snprintf(ui->watch_setup_lexicon,
                           sizeof(ui->watch_setup_lexicon), "%s", namebuf);
          }
        }
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_SIM_PLIES) {
        pthread_mutex_lock(&state->mutex);
        int v = state->sim_plies + dir;
        if (v < 1) {
          v = 1;
        }
        if (v > 1024) {
          v = 1024;
        }
        state->sim_plies = v;
        pthread_mutex_unlock(&state->mutex);
      } else if (ui->play_setup_focus == TUI_PLAY_SETUP_SIM_CANDIDATES) {
        pthread_mutex_lock(&state->mutex);
        int v = state->sim_candidates + dir * 10;
        if (v < 2) {
          v = 2;
        }
        if (v > 1024) {
          v = 1024;
        }
        state->sim_candidates = v;
        pthread_mutex_unlock(&state->mutex);
      }
      return true;
    }
    if (focus_name && name_buf != NULL) {
      const int len = (int)strlen(name_buf);
      if (key == NCKEY_LEFT) {
        if (ui->play_setup_name_cursor > 0) {
          ui->play_setup_name_cursor--;
        }
        return true;
      }
      if (key == NCKEY_RIGHT) {
        if (ui->play_setup_name_cursor < len) {
          ui->play_setup_name_cursor++;
        }
        return true;
      }
      if (key == NCKEY_HOME) {
        ui->play_setup_name_cursor = 0;
        return true;
      }
      if (key == NCKEY_END) {
        ui->play_setup_name_cursor = len;
        return true;
      }
      if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
        if (ui->play_setup_name_cursor > 0) {
          memmove(name_buf + ui->play_setup_name_cursor - 1,
                  name_buf + ui->play_setup_name_cursor,
                  (size_t)(len - ui->play_setup_name_cursor) + 1);
          ui->play_setup_name_cursor--;
        }
        return true;
      }
      if (key == NCKEY_DEL) {
        if (ui->play_setup_name_cursor < len) {
          memmove(name_buf + ui->play_setup_name_cursor,
                  name_buf + ui->play_setup_name_cursor + 1,
                  (size_t)(len - ui->play_setup_name_cursor));
        }
        return true;
      }
      if (key >= 0x20 && key < 0x7f && len + 1 < (int)name_cap) {
        memmove(name_buf + ui->play_setup_name_cursor + 1,
                name_buf + ui->play_setup_name_cursor,
                (size_t)(len - ui->play_setup_name_cursor) + 1);
        name_buf[ui->play_setup_name_cursor] = (char)key;
        ui->play_setup_name_cursor++;
        return true;
      }
    }
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      // Enter on a non-Start row advances to the next enabled
      // field; Enter on Start launches the game.
      if (ui->play_setup_focus != TUI_PLAY_SETUP_START) {
        do {
          ui->play_setup_focus++;
        } while (ui->play_setup_focus < TUI_PLAY_SETUP_START &&
                 !ps_enabled[ui->play_setup_focus]);
        if (ui->play_setup_focus == TUI_PLAY_SETUP_HUMAN_NAME) {
          ui->play_setup_name_cursor = (int)strlen(ui->play_setup_human_name);
        } else if (ui->play_setup_focus == TUI_PLAY_SETUP_COMPUTER_NAME) {
          ui->play_setup_name_cursor =
              (int)strlen(ui->play_setup_computer_name);
        }
        return true;
      }
      // The engine always seats P1 on turn first, so "Human" means
      // the human is P1 (index 0); "Computer" makes the human P2;
      // "Random" flips a coin.
      int human_idx;
      if (ui->play_setup_first_move == TUI_PLAY_FIRST_HUMAN) {
        human_idx = 0;
      } else if (ui->play_setup_first_move == TUI_PLAY_FIRST_COMPUTER) {
        human_idx = 1;
      } else {
        human_idx = (int)((uint64_t)time(NULL) & 1ULL);
      }
      const char *hn = ui->play_setup_human_name[0] != '\0'
                           ? ui->play_setup_human_name
                           : "You";
      const char *cn = ui->play_setup_computer_name[0] != '\0'
                           ? ui->play_setup_computer_name
                           : "Computer";
      // Commit the modal's scratch time / lexicon into the session.
      session->chosen_time = ui->watch_setup_time;
      (void)snprintf(session->chosen_lexicon, sizeof(session->chosen_lexicon),
                     "%s", ui->watch_setup_lexicon);
      pthread_mutex_lock(&state->mutex);
      (void)snprintf(state->pending_lexicon, sizeof(state->pending_lexicon),
                     "%s", ui->watch_setup_lexicon);
      pthread_mutex_unlock(&state->mutex);
      // Stop any running bot before reconfiguring the game.
      tui_stop_workers(state);
      if (!session->args.no_config) {
        session->to_save.time_per_side_seconds = session->chosen_time;
        session->to_save.time_per_side_set = true;
        session->to_save.overtime_rule = ui->play_setup_overtime_rule;
        session->to_save.overtime_rule_set = true;
        session->to_save.overtime_cap_minutes = ui->play_setup_overtime_cap;
        session->to_save.overtime_cap_set = true;
        session->to_save.time_penalty_rate = ui->play_setup_penalty_rate;
        session->to_save.time_penalty_set = true;
        session->to_save.challenge_rule = ui->play_setup_challenge_rule;
        session->to_save.challenge_rule_set = true;
        session->to_save.challenge_penalty = ui->play_setup_challenge_penalty;
        session->to_save.challenge_penalty_set = true;
        strncpy(session->to_save.lexicon, session->chosen_lexicon,
                sizeof(session->to_save.lexicon) - 1);
        session->to_save.lexicon[sizeof(session->to_save.lexicon) - 1] = '\0';
        session->to_save.lexicon_set = true;
        tui_config_save(&session->to_save);
      }
      bool reinitialized = false;
      if (!tui_reinit_game_state_if_needed(state, session, &reinitialized)) {
        ui->running = false;
        ui->modal = TUI_MODAL_NONE;
        return true;
      }
      pthread_mutex_lock(&state->mutex);
      tui_game_state_set_time_per_side(state, session->chosen_time);
      state->overtime_rule = ui->play_setup_overtime_rule;
      state->overtime_cap_minutes = ui->play_setup_overtime_cap;
      state->time_penalty_rate = ui->play_setup_penalty_rate;
      state->challenge_rule = ui->play_setup_challenge_rule;
      state->challenge_penalty = ui->play_setup_challenge_penalty;
      tui_game_state_reset_game(state, (uint64_t)time(NULL));
      state->app_mode = TUI_APP_MODE_PLAY_VS_COMPUTER;
      state->human_player_idx = human_idx;
      (void)snprintf(state->player_names[human_idx],
                     sizeof(state->player_names[human_idx]), "%s", hn);
      (void)snprintf(state->player_names[1 - human_idx],
                     sizeof(state->player_names[1 - human_idx]), "%s", cn);
      state->history_cursor = -1;
      state->focused_panel = TUI_FOCUS_BOARD;
      pthread_mutex_unlock(&state->mutex);
      // Rebuild cached tile / arrow planes against the fresh board.
      tui_game_render_reset_grids();
      // Start the bot — it idles on the human's turn and plays the
      // computer's.
      tui_bot_worker_start(state);
      ui->modal = TUI_MODAL_NONE;
      return true;
    }
    return true;
  }
  return false;
}

// Annotate-game setup modal keys.
// Returns true when the key was consumed.
bool tui_input_annotate_setup(TuiGameState *state, TuiUiState *ui,
                              TuiSession *session, uint32_t key,
                              ncinput input) {
  if (ui->modal == TUI_MODAL_ANNOTATE_SETUP) {
    // Click anywhere on the modal: focus the clicked item.
    // Chevrons on the Lexicon row synthesize ←/→. The Start row
    // commits via synthesized Enter.
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < TUI_ANNOTATE_SETUP_ITEM_COUNT) {
        const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
        // Reset the name caret to end-of-text when switching to
        // a different name row so a fresh click on the row
        // doesn't strand the caret mid-word.
        if (hit != ui->annotate_setup_focus) {
          if (hit == TUI_ANNOTATE_SETUP_P1_NAME) {
            ui->annotate_setup_name_cursor =
                (int)strlen(ui->annotate_setup_p1_name);
          } else if (hit == TUI_ANNOTATE_SETUP_P2_NAME) {
            ui->annotate_setup_name_cursor =
                (int)strlen(ui->annotate_setup_p2_name);
          }
        }
        ui->annotate_setup_focus = hit;
        if (chev == TUI_MODAL_CHEVRON_LEFT) {
          key = NCKEY_LEFT;
        } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
          key = NCKEY_RIGHT;
        } else if (hit == TUI_ANNOTATE_SETUP_START) {
          key = NCKEY_ENTER;
        } else {
          return true;
        }
      } else {
        return true;
      }
    }

    const bool focus_p1 =
        ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_P1_NAME;
    const bool focus_p2 =
        ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_P2_NAME;
    const bool focus_name = focus_p1 || focus_p2;
    char *name_buf = NULL;
    size_t name_cap = sizeof(ui->annotate_setup_p2_name);
    if (focus_p1) {
      name_buf = ui->annotate_setup_p1_name;
      name_cap = sizeof(ui->annotate_setup_p1_name);
    } else if (focus_p2) {
      name_buf = ui->annotate_setup_p2_name;
    }

    if (key == NCKEY_ESC) {
      ui->modal = TUI_MODAL_STARTUP_MENU;
      return true;
    }
    // Home/End move the caret on the name rows, so they only jump rows
    // elsewhere.
    const int nav = tui_list_nav(
        key, &input, ui->annotate_setup_focus, TUI_ANNOTATE_SETUP_ITEM_COUNT,
        NULL, focus_name ? TUI_LIST_NAV_ARROWS : TUI_LIST_NAV_HOME_END);
    if (nav >= 0) {
      ui->annotate_setup_focus = nav;
      if (nav == TUI_ANNOTATE_SETUP_P1_NAME) {
        ui->annotate_setup_name_cursor =
            (int)strlen(ui->annotate_setup_p1_name);
      } else if (nav == TUI_ANNOTATE_SETUP_P2_NAME) {
        ui->annotate_setup_name_cursor =
            (int)strlen(ui->annotate_setup_p2_name);
      }
      return true;
    }
    if ((ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_LANGUAGE ||
         ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_LEXICON) &&
        (key == NCKEY_LEFT || key == NCKEY_RIGHT)) {
      const int dir = key == NCKEY_RIGHT ? 1 : -1;
      if (ui->lexicon_list == NULL) {
        ui->lexicon_list = tui_lexicon_list_load();
      }
      if (ui->lexicon_list != NULL) {
        int cur =
            tui_lexicon_list_find(ui->lexicon_list, ui->annotate_setup_lexicon);
        if (cur < 0) {
          cur = 0;
        }
        // Language steps to the next language's first lexicon; Lexicon
        // steps within the current language.
        const int next =
            ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_LANGUAGE
                ? tui_lexicon_list_step_language(ui->lexicon_list, cur, dir)
                : tui_lexicon_list_step_same_language(ui->lexicon_list, cur,
                                                      dir);
        char namebuf[TUI_LEXICON_NAME_MAX];
        if (next != cur && tui_lexicon_list_name(ui->lexicon_list, next,
                                                 namebuf, sizeof(namebuf))) {
          (void)snprintf(ui->annotate_setup_lexicon,
                         sizeof(ui->annotate_setup_lexicon), "%s", namebuf);
        }
      }
      return true;
    }
    if (focus_name && name_buf != NULL) {
      const int len = (int)strlen(name_buf);
      if (key == NCKEY_LEFT) {
        if (ui->annotate_setup_name_cursor > 0) {
          ui->annotate_setup_name_cursor--;
        }
        return true;
      }
      if (key == NCKEY_RIGHT) {
        if (ui->annotate_setup_name_cursor < len) {
          ui->annotate_setup_name_cursor++;
        }
        return true;
      }
      if (key == NCKEY_HOME) {
        ui->annotate_setup_name_cursor = 0;
        return true;
      }
      if (key == NCKEY_END) {
        ui->annotate_setup_name_cursor = len;
        return true;
      }
      if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
        if (ui->annotate_setup_name_cursor > 0) {
          memmove(name_buf + ui->annotate_setup_name_cursor - 1,
                  name_buf + ui->annotate_setup_name_cursor,
                  (size_t)(len - ui->annotate_setup_name_cursor) + 1);
          ui->annotate_setup_name_cursor--;
        }
        return true;
      }
      if (key == NCKEY_DEL) {
        if (ui->annotate_setup_name_cursor < len) {
          memmove(name_buf + ui->annotate_setup_name_cursor,
                  name_buf + ui->annotate_setup_name_cursor + 1,
                  (size_t)(len - ui->annotate_setup_name_cursor));
        }
        return true;
      }
      if (key >= 0x20 && key < 0x7f && len + 1 < (int)name_cap) {
        memmove(name_buf + ui->annotate_setup_name_cursor + 1,
                name_buf + ui->annotate_setup_name_cursor,
                (size_t)(len - ui->annotate_setup_name_cursor) + 1);
        name_buf[ui->annotate_setup_name_cursor] = (char)key;
        ui->annotate_setup_name_cursor++;
        return true;
      }
    }
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      // Enter on a non-Start row advances to the next field.
      // Enter on Start (or anywhere from the keyboard with focus
      // already on Start) commits.
      if (ui->annotate_setup_focus != TUI_ANNOTATE_SETUP_START) {
        ui->annotate_setup_focus++;
        if (ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_P1_NAME) {
          ui->annotate_setup_name_cursor =
              (int)strlen(ui->annotate_setup_p1_name);
        } else if (ui->annotate_setup_focus == TUI_ANNOTATE_SETUP_P2_NAME) {
          ui->annotate_setup_name_cursor =
              (int)strlen(ui->annotate_setup_p2_name);
        }
        return true;
      }
      // Commit. Stop bot if running, reinit on lexicon change,
      // empty-board reset, set player names, append one
      // pending entry for P1, drop into annotation mode (no
      // bot started).
      tui_stop_workers(state);
      (void)snprintf(session->chosen_lexicon, sizeof(session->chosen_lexicon),
                     "%s", ui->annotate_setup_lexicon);
      pthread_mutex_lock(&state->mutex);
      (void)snprintf(state->pending_lexicon, sizeof(state->pending_lexicon),
                     "%s", ui->annotate_setup_lexicon);
      pthread_mutex_unlock(&state->mutex);
      if (!session->args.no_config) {
        strncpy(session->to_save.lexicon, session->chosen_lexicon,
                sizeof(session->to_save.lexicon) - 1);
        session->to_save.lexicon[sizeof(session->to_save.lexicon) - 1] = '\0';
        session->to_save.lexicon_set = true;
        tui_config_save(&session->to_save);
      }
      bool reinitialized = false;
      if (!tui_reinit_game_state_if_needed(state, session, &reinitialized)) {
        ui->running = false;
        ui->modal = TUI_MODAL_NONE;
        return true;
      }
      if (reinitialized) {
        tui_game_state_set_time_per_side(state, session->chosen_time);
      }
      pthread_mutex_lock(&state->mutex);
      state->app_mode = TUI_APP_MODE_ANNOTATE;
      tui_game_state_reset_game_for_annotation(state);
      // Tear down all cached tile / arrow planes from the prior
      // game so the next render rebuilds them fresh against the
      // empty annotation board.
      tui_game_render_reset_grids();
      (void)snprintf(state->player_names[0], sizeof(state->player_names[0]),
                     "%s", ui->annotate_setup_p1_name);
      (void)snprintf(state->player_names[1], sizeof(state->player_names[1]),
                     "%s", ui->annotate_setup_p2_name);
      // Seed history with one pending entry for P1 so the
      // History panel reads "1." waiting for input. Rack is
      // NULL because the annotator will fill it in later.
      tui_bot_worker_append_pending_history(state, 0, NULL,
                                            state->time_per_side_seconds);
      // Open the move editor on the seeded turn so the white
      // cursor lands in the move zone immediately — no extra
      // click needed before typing.
      state->edit_history_idx = 0;
      state->edit_field = TUI_EDIT_FIELD_MOVE;
      state->edit_move_buf[0] = '\0';
      state->edit_move_len = 0;
      state->edit_move_cursor = 0;
      state->edit_rack_buf[0] = '\0';
      state->edit_rack_len = 0;
      state->edit_rack_cursor = 0;
      state->edit_rack_user_modified = false;
      tui_game_state_parse_edit_buf(state);
      state->focused_panel = TUI_FOCUS_HISTORY;
      state->history_cursor = 0;
      pthread_mutex_unlock(&state->mutex);
      // No tui_bot_worker_start — annotation mode is human-
      // driven. Move-entry / rack-entry UI will hook in later.
      ui->modal = TUI_MODAL_NONE;
      return true;
    }
    return true;
  }
  return false;
}

// Watch-game setup modal keys.
// Returns true when the key was consumed.
bool tui_input_watch_setup(TuiGameState *state, TuiUiState *ui,
                           TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_WATCH_SETUP) {
    // Click on a setup row: select that row. For the "Start
    // game" row, also trigger commit (Enter). Other rows use
    // ←/→ to cycle values; click on the ◀ / ▶ chevrons fires
    // those adjusts directly so the user doesn't need to
    // switch to the keyboard.
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < TUI_WATCH_SETUP_ITEM_COUNT) {
        const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
        ui->watch_setup_focus = hit;
        if (chev == TUI_MODAL_CHEVRON_LEFT) {
          key = NCKEY_LEFT;
        } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
          key = NCKEY_RIGHT;
        } else if (hit == TUI_WATCH_SETUP_START) {
          key = NCKEY_ENTER;
        } else {
          return true;
        }
      } else {
        return true;
      }
    }
    const bool key_left = key == NCKEY_LEFT || key == 'h' || key == 'H';
    const bool key_right = key == NCKEY_RIGHT || key == 'l' || key == 'L';
    if (key == NCKEY_ESC) {
      ui->modal = TUI_MODAL_STARTUP_MENU;
      return true;
    }
    const int nav = tui_list_nav(key, &input, ui->watch_setup_focus,
                                 TUI_WATCH_SETUP_ITEM_COUNT, NULL,
                                 TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
    if (nav >= 0) {
      ui->watch_setup_focus = nav;
      return true;
    }
    if (key_left || key_right) {
      const int dir = key_right ? 1 : -1;
      if (ui->watch_setup_focus == TUI_WATCH_SETUP_TIME) {
        const int n = tui_time_picker_preset_count();
        const int cur = tui_time_picker_closest_index(ui->watch_setup_time);
        int next = cur + dir;
        if (next < 0) {
          next = 0;
        }
        if (next >= n) {
          next = n - 1;
        }
        ui->watch_setup_time = tui_time_picker_preset_seconds(next);
      } else if (ui->watch_setup_focus == TUI_WATCH_SETUP_LANGUAGE) {
        // Cycle to the next/previous language group, snapping to
        // that group's first lexicon so the Lexicon row below
        // always shows a valid entry for the new language.
        // Mutates only the modal-local lexicon copy — committed
        // to the session on "Start".
        if (ui->lexicon_list == NULL) {
          ui->lexicon_list = tui_lexicon_list_load();
        }
        if (ui->lexicon_list != NULL) {
          int cur =
              tui_lexicon_list_find(ui->lexicon_list, ui->watch_setup_lexicon);
          if (cur < 0) {
            cur = 0;
          }
          const int next =
              tui_lexicon_list_step_language(ui->lexicon_list, cur, dir);
          char buf[TUI_LEXICON_NAME_MAX];
          if (next != cur &&
              tui_lexicon_list_name(ui->lexicon_list, next, buf, sizeof(buf))) {
            (void)snprintf(ui->watch_setup_lexicon,
                           sizeof(ui->watch_setup_lexicon), "%s", buf);
          }
        }
      } else if (ui->watch_setup_focus == TUI_WATCH_SETUP_LEXICON) {
        // Lazy-load the lexicon list on first use; the Settings
        // modal already keeps its own copy alive, so reuse it.
        // Cycling stays within the current language group — to
        // change language, use the Language row above. Mutates
        // only the modal-local copy.
        if (ui->lexicon_list == NULL) {
          ui->lexicon_list = tui_lexicon_list_load();
        }
        if (ui->lexicon_list != NULL) {
          int cur =
              tui_lexicon_list_find(ui->lexicon_list, ui->watch_setup_lexicon);
          if (cur < 0) {
            cur = 0;
          }
          const int next =
              tui_lexicon_list_step_same_language(ui->lexicon_list, cur, dir);
          char buf[TUI_LEXICON_NAME_MAX];
          if (next != cur &&
              tui_lexicon_list_name(ui->lexicon_list, next, buf, sizeof(buf))) {
            (void)snprintf(ui->watch_setup_lexicon,
                           sizeof(ui->watch_setup_lexicon), "%s", buf);
          }
        }
      } else if (ui->watch_setup_focus == TUI_WATCH_SETUP_SIM_PLIES) {
        pthread_mutex_lock(&state->mutex);
        int v = state->sim_plies + dir;
        if (v < 1) {
          v = 1;
        }
        if (v > 1024) {
          v = 1024;
        }
        state->sim_plies = v;
        pthread_mutex_unlock(&state->mutex);
      } else if (ui->watch_setup_focus == TUI_WATCH_SETUP_SIM_CANDIDATES) {
        pthread_mutex_lock(&state->mutex);
        int v = state->sim_candidates + dir * 10;
        if (v < 2) {
          v = 2;
        }
        if (v > 1024) {
          v = 1024;
        }
        state->sim_candidates = v;
        pthread_mutex_unlock(&state->mutex);
      }
      return true;
    }
    if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      if (ui->watch_setup_focus == TUI_WATCH_SETUP_START) {
        // Commit the modal's local copies into the live session
        // settings. Before this point the adjusters touched
        // only watch_setup_lexicon / watch_setup_time, so an
        // Esc cancel leaves the underlying session untouched.
        session->chosen_time = ui->watch_setup_time;
        (void)snprintf(session->chosen_lexicon, sizeof(session->chosen_lexicon),
                       "%s", ui->watch_setup_lexicon);
        pthread_mutex_lock(&state->mutex);
        (void)snprintf(state->pending_lexicon, sizeof(state->pending_lexicon),
                       "%s", ui->watch_setup_lexicon);
        pthread_mutex_unlock(&state->mutex);

        // Replicate the time-picker confirm path: stop any bot
        // that's running, reset state (re-init if lexicon /
        // RIT changed), kick off a fresh bot run with the
        // chosen settings.
        tui_stop_workers(state);
        if (!session->args.no_config) {
          session->to_save.time_per_side_seconds = session->chosen_time;
          session->to_save.time_per_side_set = true;
          strncpy(session->to_save.lexicon, session->chosen_lexicon,
                  sizeof(session->to_save.lexicon) - 1);
          session->to_save.lexicon[sizeof(session->to_save.lexicon) - 1] = '\0';
          session->to_save.lexicon_set = true;
          tui_config_save(&session->to_save);
        }
        bool reinitialized = false;
        if (!tui_reinit_game_state_if_needed(state, session, &reinitialized)) {
          ui->running = false;
          ui->modal = TUI_MODAL_NONE;
          return true;
        }
        if (reinitialized) {
          tui_game_state_set_time_per_side(state, session->chosen_time);
          // tui_game_state_init intentionally leaves the racks
          // empty so the startup menu can render an idle state
          // (Bag full, Racks empty). For a fresh watch game we
          // need real opening racks — same call the same-lexicon
          // branch below makes. Without this, both bots play
          // with empty racks, pass every turn, and the game
          // ends in 6 passes with the bag still at 100.
          pthread_mutex_lock(&state->mutex);
          tui_game_state_reset_game(state, (uint64_t)time(NULL));
          pthread_mutex_unlock(&state->mutex);
        } else {
          pthread_mutex_lock(&state->mutex);
          tui_game_state_set_time_per_side(state, session->chosen_time);
          tui_game_state_reset_game(state, (uint64_t)time(NULL));
          pthread_mutex_unlock(&state->mutex);
        }
        pthread_mutex_lock(&state->mutex);
        state->app_mode = TUI_APP_MODE_WATCH;
        pthread_mutex_unlock(&state->mutex);
        tui_bot_worker_start(state);
        ui->modal = TUI_MODAL_NONE;
      }
      return true;
    }
    return true;
  }
  return false;
}

#include "input_menus.h"

#include "bot_worker.h"
#include "config.h"
#include "game_state.h"
#include "list_nav.h"
#include "render_hit_test.h"
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

const char *tui_analysis_menu_reason(const TuiGameState *state, int item) {
  static const TuiAnalysisAction actions[TUI_ANALYSIS_MENU_ITEM_COUNT] = {
      [TUI_ANALYSIS_MENU_SIM] = TUI_ANALYSIS_SIM,
      [TUI_ANALYSIS_MENU_KIBITZ] = TUI_ANALYSIS_KIBITZ,
      [TUI_ANALYSIS_MENU_RESUME] = TUI_ANALYSIS_RESUME,
      [TUI_ANALYSIS_MENU_STOP] = TUI_ANALYSIS_STOP,
  };
  if (item < 0 || item >= TUI_ANALYSIS_MENU_ITEM_COUNT ||
      item == TUI_ANALYSIS_MENU_BACK) {
    return NULL;
  }
  return tui_analysis_unavailable_reason(state, state->history_cursor,
                                         actions[item]);
}

bool tui_input_analysis_menu(TuiGameState *state, TuiUiState *ui, uint32_t key,
                             ncinput input) {
  if (ui->modal != TUI_MODAL_ANALYSIS_MENU) {
    return false;
  }
  bool enabled[TUI_ANALYSIS_MENU_ITEM_COUNT];
  pthread_mutex_lock(&state->mutex);
  for (int item = 0; item < TUI_ANALYSIS_MENU_ITEM_COUNT; item++) {
    enabled[item] = tui_analysis_menu_reason(state, item) == NULL;
  }
  pthread_mutex_unlock(&state->mutex);
  // Unavailable items stay focusable, unlike other dialogs' dimmed rows:
  // focusing one shows why it can't run in the help line.
  if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
    const int hit = tui_modal_item_at(input.y, input.x);
    if (hit < 0 || hit >= TUI_ANALYSIS_MENU_ITEM_COUNT) {
      return true;
    }
    ui->analysis_menu_focus = hit;
    if (!enabled[hit]) {
      return true;
    }
    key = NCKEY_ENTER;
  }
  const int nav = tui_list_nav(key, &input, ui->analysis_menu_focus,
                               TUI_ANALYSIS_MENU_ITEM_COUNT, NULL,
                               TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
  if (nav >= 0) {
    ui->analysis_menu_focus = nav;
  } else if (key == NCKEY_ESC) {
    ui->modal = TUI_MODAL_NONE;
  } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
    const int item = ui->analysis_menu_focus;
    if (!enabled[item]) {
      return true;
    }
    ui->modal = TUI_MODAL_NONE;
    if (item == TUI_ANALYSIS_MENU_STOP) {
      tui_analysis_worker_stop_and_join(state);
    } else if (item != TUI_ANALYSIS_MENU_BACK) {
      pthread_mutex_lock(&state->mutex);
      if (item == TUI_ANALYSIS_MENU_KIBITZ) {
        tui_analysis_kibitz(state, state->history_cursor);
      } else {
        tui_analysis_worker_start(state, state->history_cursor,
                                  item == TUI_ANALYSIS_MENU_SIM);
      }
      pthread_mutex_unlock(&state->mutex);
    }
  }
  return true;
}

// Quit-confirm modal keys.
// Returns true when the key was consumed.
bool tui_input_quit_confirm(TuiGameState *state, TuiUiState *ui,
                            TuiSession *session, uint32_t key, ncinput input) {
  (void)state;
  (void)session;
  if (ui->modal == TUI_MODAL_QUIT_CONFIRM) {
    // Y / N shortcuts trigger their action regardless of focus.
    // Enter confirms whatever's focused (default No, the safer
    // option). Esc / N returns to whichever modal opened the
    // confirm (main menu when launched via Quit there, or NONE
    // when launched via the command-bar Q).
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < 2) {
        ui->quit_confirm_focus = hit;
        key = NCKEY_ENTER;
      } else {
        return true;
      }
    }
    const int nav = tui_list_nav(key, &input, ui->quit_confirm_focus, 2, NULL,
                                 TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
    if (nav >= 0) {
      ui->quit_confirm_focus = nav;
    } else if (key == NCKEY_ESC || key == 'n' || key == 'N') {
      ui->modal = ui->quit_confirm_return;
    } else if (key == 'y' || key == 'Y') {
      ui->running = false;
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      if (ui->quit_confirm_focus == 1) {
        ui->running = false;
      } else {
        ui->modal = ui->quit_confirm_return;
      }
    }
    return true;
  }
  return false;
}

// Time picker modal keys (starts a new Watch game on Enter).
// Returns true when the key was consumed.
bool tui_input_time_picker(TuiGameState *state, TuiUiState *ui,
                           TuiSession *session, uint32_t key, ncinput input) {
  if (ui->modal == TUI_MODAL_TIME_PICKER) {
    const int preset_count = tui_time_picker_preset_count();
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < preset_count) {
        ui->time_focus = hit;
        key = NCKEY_ENTER;
      } else {
        return true;
      }
    }
    const int nav = tui_list_nav(key, &input, ui->time_focus, preset_count,
                                 NULL, TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
    if (nav >= 0) {
      ui->time_focus = nav;
    } else if (key == NCKEY_ESC) {
      ui->modal = ui->time_picker_return;
    } else if (key >= '1' && key <= (uint32_t)('0' + preset_count)) {
      ui->time_focus = (int)(key - '1');
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      const int new_time = tui_time_picker_preset_seconds(ui->time_focus);
      if (new_time > 0) {
        // Stop the bot only when one is actually running. At first
        // launch the bot is idle (waiting on the startup menu), so
        // the pthread_join would block forever on a never-started
        // thread.
        tui_stop_workers(state);
        session->chosen_time = new_time;
        if (!session->args.no_config) {
          session->to_save.time_per_side_seconds = new_time;
          session->to_save.time_per_side_set = true;
          tui_config_save(&session->to_save);
        }
        // Pending lexicon / RIT changes that need a full re-init?
        // If so, tear down the state and re-init with the new
        // settings (loading fresh tables). Otherwise the fast in-
        // place reset is enough.
        bool reinitialized = false;
        if (!tui_reinit_game_state_if_needed(state, session, &reinitialized)) {
          ui->running = false;
          ui->modal = TUI_MODAL_NONE;
          return true;
        }
        if (reinitialized) {
          tui_game_state_set_time_per_side(state, new_time);
        } else {
          pthread_mutex_lock(&state->mutex);
          tui_game_state_set_time_per_side(state, new_time);
          tui_game_state_reset_game(state, (uint64_t)time(NULL));
          pthread_mutex_unlock(&state->mutex);
        }
        pthread_mutex_lock(&state->mutex);
        state->app_mode = TUI_APP_MODE_WATCH;
        pthread_mutex_unlock(&state->mutex);
        tui_bot_worker_start(state);
      }
      ui->modal = TUI_MODAL_NONE;
    }
    return true;
  }
  return false;
}

// Main menu (Esc) modal keys.
// Returns true when the key was consumed.
bool tui_input_main_menu(TuiGameState *state, TuiUiState *ui,
                         TuiSession *session, uint32_t key, ncinput input) {
  (void)state;
  (void)session;
  if (ui->modal == TUI_MODAL_MAIN_MENU) {
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0) {
        ui->main_menu_focus = hit;
        key = NCKEY_ENTER;
      } else {
        return true;
      }
    }
    const int nav =
        tui_list_nav(key, &input, ui->main_menu_focus, TUI_MENU_ITEM_COUNT,
                     NULL, TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
    if (nav >= 0) {
      ui->main_menu_focus = nav;
    } else if (key == NCKEY_ESC) {
      ui->modal = TUI_MODAL_NONE;
    } else if (key == 'n' || key == 'N') {
      // Mnemonic shortcuts trigger the action immediately, matching
      // the hint shown to the right of each item in the modal. They
      // skip focus-then-Enter so the menu behaves like a launcher.
      // New game now routes through the startup menu so the user
      // can pick load/annotate modes alongside watch.
      ui->modal = TUI_MODAL_STARTUP_MENU;
      ui->startup_menu_focus = TUI_STARTUP_WATCH;
      ui->startup_menu_return = TUI_MODAL_MAIN_MENU;
    } else if (key == 's' || key == 'S') {
      ui->modal = TUI_MODAL_SETTINGS;
      ui->settings_focus = 0;
      ui->settings_return = TUI_MODAL_MAIN_MENU;
    } else if (key == 'q' || key == 'Q') {
      ui->modal = TUI_MODAL_QUIT_CONFIRM;
      ui->quit_confirm_focus = 0;
      ui->quit_confirm_return = TUI_MODAL_MAIN_MENU;
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      if (ui->main_menu_focus == TUI_MENU_NEW_GAME) {
        // Pivot to the startup menu so the user can pick what
        // KIND of new game (watch / load / annotate / vs-cpu) —
        // Watch from there opens the time picker and ends up
        // doing what this branch used to do directly.
        ui->modal = TUI_MODAL_STARTUP_MENU;
        ui->startup_menu_focus = TUI_STARTUP_WATCH;
        ui->startup_menu_return = TUI_MODAL_MAIN_MENU;
      } else if (ui->main_menu_focus == TUI_MENU_SETTINGS) {
        ui->modal = TUI_MODAL_SETTINGS;
        ui->settings_focus = 0;
        ui->settings_return = TUI_MODAL_MAIN_MENU;
      } else if (ui->main_menu_focus == TUI_MENU_QUIT) {
        ui->modal = TUI_MODAL_QUIT_CONFIRM;
        ui->quit_confirm_focus = 0;
        ui->quit_confirm_return = TUI_MODAL_MAIN_MENU;
      } else if (ui->main_menu_focus == TUI_MENU_BACK) {
        ui->modal = TUI_MODAL_NONE;
      }
    }
    return true;
  }
  return false;
}

// Startup menu modal keys.
// Returns true when the key was consumed.
bool tui_input_startup_menu(TuiGameState *state, TuiUiState *ui,
                            const TuiSession *session, uint32_t key,
                            ncinput input) {
  (void)state;
  if (ui->modal == TUI_MODAL_STARTUP_MENU) {
    // Helper: which menu items are currently selectable. Only
    // "Watch computer play" is wired up; others render dimmed
    // and the cursor skips past them. Keep this aligned with
    // the disabled mask inside tui_game_render_startup_menu.
    bool su_enabled[TUI_STARTUP_ITEM_COUNT];
    su_enabled[TUI_STARTUP_WATCH] = true;
    su_enabled[TUI_STARTUP_LOAD_POSITION] = true;
    su_enabled[TUI_STARTUP_LOAD_GAME] = true;
    su_enabled[TUI_STARTUP_ANNOTATE] = true;
    su_enabled[TUI_STARTUP_PLAY_VS_COMPUTER] = true;
    if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
      const int hit = tui_modal_item_at(input.y, input.x);
      if (hit >= 0 && hit < TUI_STARTUP_ITEM_COUNT && su_enabled[hit]) {
        ui->startup_menu_focus = hit;
        key = NCKEY_ENTER;
      } else {
        return true;
      }
    }
    const int nav = tui_list_nav(key, &input, ui->startup_menu_focus,
                                 TUI_STARTUP_ITEM_COUNT, su_enabled,
                                 TUI_LIST_NAV_HOME_END | TUI_LIST_NAV_VI);
    if (nav >= 0) {
      ui->startup_menu_focus = nav;
    } else if (key == NCKEY_ESC) {
      // Esc returns to whichever modal opened the startup menu.
      // First-launch: TUI_MODAL_NONE (dismisses to the bot game
      // already running underneath). Esc → New game: returns to
      // TUI_MODAL_MAIN_MENU so the user can pick Settings/Quit.
      ui->modal = ui->startup_menu_return;
    } else if (key == 'w' || key == 'W') {
      // Mnemonic shortcut: open the Watch setup modal. The setup
      // modal handles starting the game once the user confirms.
      ui->modal = TUI_MODAL_WATCH_SETUP;
      (void)snprintf(ui->watch_setup_lexicon, sizeof(ui->watch_setup_lexicon),
                     "%s", session->chosen_lexicon);
      ui->watch_setup_time = session->chosen_time;
    } else if (key == 'p' || key == 'P') {
      ui->modal = TUI_MODAL_LOAD_POSITION;
      ui->load_position_buf[0] = '\0';
      ui->load_position_len = 0;
      ui->load_position_cursor = 0;
      ui->load_position_parse_ok = false;
      ui->load_position_dirty = false;
      ui->load_position_error[0] = '\0';
    } else if (key == 'g' || key == 'G') {
      ui->modal = TUI_MODAL_LOAD_GAME;
      ui->load_game_buf[0] = '\0';
      ui->load_game_len = 0;
      ui->load_game_cursor = 0;
      ui->load_game_parse_ok = false;
      ui->load_game_dirty = false;
      ui->load_game_error[0] = '\0';
    } else if (key == 'a' || key == 'A') {
      ui->modal = TUI_MODAL_ANNOTATE_SETUP;
      (void)snprintf(ui->annotate_setup_lexicon,
                     sizeof(ui->annotate_setup_lexicon), "%s",
                     session->chosen_lexicon);
      (void)snprintf(ui->annotate_setup_p1_name,
                     sizeof(ui->annotate_setup_p1_name), "Player 1");
      (void)snprintf(ui->annotate_setup_p2_name,
                     sizeof(ui->annotate_setup_p2_name), "Player 2");
      ui->annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
      ui->annotate_setup_name_cursor = (int)strlen(ui->annotate_setup_p1_name);
    } else if (key == 'c' || key == 'C') {
      ui->modal = TUI_MODAL_PLAY_SETUP;
      ui->play_setup_focus = TUI_PLAY_SETUP_START;
      (void)snprintf(ui->play_setup_human_name,
                     sizeof(ui->play_setup_human_name), "You");
      (void)snprintf(ui->play_setup_computer_name,
                     sizeof(ui->play_setup_computer_name), "Computer");
      ui->play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
      ui->play_setup_name_cursor = 0;
      (void)snprintf(ui->watch_setup_lexicon, sizeof(ui->watch_setup_lexicon),
                     "%s", session->chosen_lexicon);
      ui->watch_setup_time = session->chosen_time;
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      if (ui->startup_menu_focus == TUI_STARTUP_WATCH) {
        ui->modal = TUI_MODAL_WATCH_SETUP;
        (void)snprintf(ui->watch_setup_lexicon, sizeof(ui->watch_setup_lexicon),
                       "%s", session->chosen_lexicon);
        ui->watch_setup_time = session->chosen_time;
      } else if (ui->startup_menu_focus == TUI_STARTUP_LOAD_POSITION) {
        ui->modal = TUI_MODAL_LOAD_POSITION;
        ui->load_position_buf[0] = '\0';
        ui->load_position_len = 0;
        ui->load_position_cursor = 0;
        ui->load_position_parse_ok = false;
        ui->load_position_dirty = false;
        ui->load_position_error[0] = '\0';
      } else if (ui->startup_menu_focus == TUI_STARTUP_LOAD_GAME) {
        ui->modal = TUI_MODAL_LOAD_GAME;
        ui->load_game_buf[0] = '\0';
        ui->load_game_len = 0;
        ui->load_game_cursor = 0;
        ui->load_game_parse_ok = false;
        ui->load_game_dirty = false;
        ui->load_game_error[0] = '\0';
      } else if (ui->startup_menu_focus == TUI_STARTUP_ANNOTATE) {
        ui->modal = TUI_MODAL_ANNOTATE_SETUP;
        (void)snprintf(ui->annotate_setup_lexicon,
                       sizeof(ui->annotate_setup_lexicon), "%s",
                       session->chosen_lexicon);
        (void)snprintf(ui->annotate_setup_p1_name,
                       sizeof(ui->annotate_setup_p1_name), "Player 1");
        (void)snprintf(ui->annotate_setup_p2_name,
                       sizeof(ui->annotate_setup_p2_name), "Player 2");
        ui->annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
        ui->annotate_setup_name_cursor =
            (int)strlen(ui->annotate_setup_p1_name);
      } else if (ui->startup_menu_focus == TUI_STARTUP_PLAY_VS_COMPUTER) {
        // Single play-vs-computer setup modal: names, who moves
        // first, time, lexicon, and sim (computer-strength) params.
        // Reuses watch_setup_time / watch_setup_lexicon as the scratch
        // copies for the time / lexicon adjusters.
        ui->modal = TUI_MODAL_PLAY_SETUP;
        ui->play_setup_focus = TUI_PLAY_SETUP_START;
        (void)snprintf(ui->play_setup_human_name,
                       sizeof(ui->play_setup_human_name), "You");
        (void)snprintf(ui->play_setup_computer_name,
                       sizeof(ui->play_setup_computer_name), "Computer");
        ui->play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
        ui->play_setup_name_cursor = 0;
        (void)snprintf(ui->watch_setup_lexicon, sizeof(ui->watch_setup_lexicon),
                       "%s", session->chosen_lexicon);
        ui->watch_setup_time = session->chosen_time;
      }
      // Disabled items are no-op for now. As each mode ships,
      // add its branch here and flip su_enabled[i] true above.
    }
    return true;
  }
  return false;
}

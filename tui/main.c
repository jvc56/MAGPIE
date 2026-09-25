#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gcg.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "bot_worker.h"
#include "config.h"
#include "frame_dump.h"
#include "game_render.h"
#include "game_state.h"
#include "gcg_import.h"
#include "glyph_cache.h"
#include "input_cell_editor.h"
#include "input_load.h"
#include "input_menus.h"
#include "input_mouse.h"
#include "input_settings.h"
#include "input_setup.h"
#include "lexicon_picker.h"
#include "move_entry.h"
#include "onboarding.h"
#include "render_bars.h"
#include "render_board.h"
#include "render_common.h"
#include "render_hit_test.h"
#include "render_modals.h"
#include "render_rack.h"
#include "theme.h"
#include "time_picker.h"
#include "tui_cli_args.h"
#include "tui_clipboard.h"
#include "tui_crash.h"
#include "tui_history_edit.h"
#include "tui_text_edit.h"
#include "tui_ui_state.h"
#include <execinfo.h>
#include <fcntl.h>
#include <locale.h>
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
  TARGET_FPS = 60,
};

static const long FRAME_NS = 1000000000L / TARGET_FPS;

static void render_init_error(struct ncplane *plane, const Theme *theme,
                              const char *lexicon, const char *message) {
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < plane_cols; col++) {
    ncplane_putstr_yx(plane, 0, (int)col, " ");
  }
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — could not start game ");

  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 3, 4, "Failed to load ");
  ncplane_putstr(plane, lexicon != NULL ? lexicon : "(unknown)");
  ncplane_putstr(plane, ":");

  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, 5, 4, message != NULL ? message : "(no detail)");

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4, "Press any key to exit.");
}

int main(int argc, char *argv[]) {
  TuiSession session = {0};
  session.args = parse_args(argc, argv);
  if (session.args.error) {
    return 2;
  }
  if (session.args.show_help) {
    print_usage();
    return 0;
  }

  // Redirect config load/save to the --config path (if given) before any
  // config access below. No-op when NULL.
  tui_config_set_path_override(session.args.config_path);

  setlocale(LC_ALL, "");

  // Redirect stderr to a known file BEFORE notcurses takes over the
  // TTY. notcurses puts the terminal in alt-screen mode, so any
  // `log_fatal` message sent to stderr gets blitted under the TUI
  // and is invisible by the time the crash handler restores the
  // screen. Capturing to a file gives us a paper trail for the
  // engine's last-words message. Best-effort: a failure here is
  // not fatal (the TUI still runs, we just lose the breadcrumb).
  freopen("/tmp/magpie_stderr.log", "w", stderr);
  // Unbuffer stderr so engine log_fatal output reaches disk before
  // abort().
  setvbuf(stderr, NULL, _IONBF, 0);

  notcurses_options opts = {
      // NO_QUIT_SIGHANDLERS prevents notcurses from installing its
      // own SIGABRT/SIGSEGV/etc handlers during init — we install our
      // own immediately below and want them to be the ONLY thing on
      // the signal, otherwise notcurses's chained handler eats the
      // crash before our backtrace dump runs.
      .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_QUIT_SIGHANDLERS,
  };
  struct notcurses *nc = notcurses_core_init(&opts, NULL);
  if (nc == NULL) {
    return 1;
  }
  tui_crash_set_nc(nc);
  install_crash_handlers();
  // Hard-disable scrolling on the std plane: macOS Terminal can otherwise
  // scroll the alt screen when render coords overflow the visible area
  // mid-resize, and that scroll is irreversible.
  ncplane_set_scrolling(notcurses_stdplane(nc), false);
  // Enable mouse-button reports. With NCMICE_BUTTON_EVENT the input
  // stream now delivers NCTYPE_PRESS / NCTYPE_RELEASE / NCSCROLL_*
  // records alongside keystrokes. No handlers wired yet — this is a
  // pure smoke-test of what enabling does to the terminal's feel
  // (notably: text selection now requires the platform modifier,
  // typically Option/Cmd on macOS).
  // BUTTON_EVENT gives press/release for left/right/wheel; DRAG_EVENT
  // adds motion-while-held so the Analysis scrollbar thumb can be
  // dragged smoothly across the track.
  const unsigned mice_eventmask = NCMICE_BUTTON_EVENT | NCMICE_DRAG_EVENT;
  notcurses_mice_enable(nc, mice_eventmask);
  TuiUiState ui = {0};
  ui.mouse_enabled = true;

  // Ask the terminal to report focus-in / focus-out events
  // (xterm DEC mode 1004). When the terminal loses focus — e.g.
  // because the user pressed Cmd+Shift+4 to start a screenshot
  // — we'll temporarily disable mouse reporting so macOS's
  // window server isn't fighting us for cursor capture, which
  // is what causes the screenshot UI's appearance to lag.
  // Notcurses doesn't surface focus events directly, so the
  // terminal's CSI I / CSI O sequences get replayed as raw
  // bytes (Esc, '[', 'I' / 'O') through notcurses_get; the
  // input drain loop watches for that pattern.
  (void)!write(STDOUT_FILENO, "\x1b[?1004h", 8);

  // Load saved settings (if any), then resolve each in order: theme first
  // (it controls the picker palette for the others), then lexicon, then
  // time. Any picker may be skipped if the value is already known and
  // --reconfigure was not passed.
  TuiConfig loaded = {0};
  const bool config_existed =
      !session.args.no_config && tui_config_load(&loaded);
  session.to_save = loaded;
  bool should_save = false;

  // THEME ---------------------------------------------------------------
  ThemeName chosen_theme = THEME_DARK;
  if (session.args.theme_arg != NULL) {
    chosen_theme = theme_get_by_id(session.args.theme_arg)->name;
  } else if (config_existed && loaded.theme_set && !session.args.reconfigure) {
    chosen_theme = loaded.theme;
  } else {
    const ThemeName initial =
        loaded.theme_set ? loaded.theme : theme_auto_detect(nc);
    chosen_theme = tui_onboarding_run(nc, initial);
    session.to_save.theme = chosen_theme;
    session.to_save.theme_set = true;
    should_save = true;
  }
  const Theme *theme = theme_get(chosen_theme);

  // LEXICON -------------------------------------------------------------
  session.chosen_lexicon[0] = '\0';
  if (config_existed && loaded.lexicon_set && !session.args.reconfigure) {
    strncpy(session.chosen_lexicon, loaded.lexicon,
            sizeof(session.chosen_lexicon) - 1);
    session.chosen_lexicon[sizeof(session.chosen_lexicon) - 1] = '\0';
  } else {
    const char *initial = loaded.lexicon_set ? loaded.lexicon : NULL;
    if (!tui_lexicon_picker_run(nc, theme, initial, session.chosen_lexicon,
                                sizeof(session.chosen_lexicon))) {
      notcurses_stop(nc);
      return 0;
    }
    strncpy(session.to_save.lexicon, session.chosen_lexicon,
            sizeof(session.to_save.lexicon) - 1);
    session.to_save.lexicon[sizeof(session.to_save.lexicon) - 1] = '\0';
    session.to_save.lexicon_set = true;
    should_save = true;
  }

  // TIME ----------------------------------------------------------------
  session.chosen_time = 0;
  if (config_existed && loaded.time_per_side_set && !session.args.reconfigure) {
    session.chosen_time = loaded.time_per_side_seconds;
  } else {
    const int initial =
        loaded.time_per_side_set ? loaded.time_per_side_seconds : 0;
    session.chosen_time = tui_time_picker_run(nc, theme, initial);
    if (session.chosen_time < 0) {
      notcurses_stop(nc);
      return 0;
    }
    session.to_save.time_per_side_seconds = session.chosen_time;
    session.to_save.time_per_side_set = true;
    should_save = true;
  }

  if (should_save && !session.args.no_config) {
    tui_config_save(&session.to_save);
  }

  struct ncplane *std_plane = notcurses_stdplane(nc);

  // Initialize the game with the chosen lexicon. Static so it
  // lives in BSS rather than on main()'s stack — sizeof(TuiGameState)
  // is in the tens-of-MB range once ANALYSIS_ROW_CAP * TUI_HISTORY_MAX
  // saved-snapshot rows are accounted for, which would overflow
  // the default 8 MB pthread stack on macOS.
  static TuiGameState game_state = {0};
  char init_error[256] = {0};
  const uint64_t seed = (uint64_t)time(NULL);
  session.initial_load_rit = loaded.load_rit_set ? loaded.load_rit : false;
  if (!tui_game_state_init(session.chosen_lexicon, seed,
                           session.initial_load_rit, &game_state, init_error,
                           sizeof(init_error))) {
    render_init_error(std_plane, theme, session.chosen_lexicon, init_error);
    notcurses_render(nc);
    ncinput input;
    notcurses_get(nc, NULL, &input);
    notcurses_stop(nc);
    return 1;
  }

  tui_game_state_set_time_per_side(&game_state, session.chosen_time);
  // Pixel-grid border thickness: from config when present, else default 2.
  game_state.border_thickness =
      loaded.border_thickness_set ? loaded.border_thickness : 2;
  game_state.blank_uppercase =
      loaded.blank_uppercase_set ? loaded.blank_uppercase : true;
  game_state.premium_labels = loaded.premium_labels_set
                                  ? loaded.premium_labels
                                  : TUI_PREMIUM_LABELS_UPPERCASE;
  game_state.board_scale = loaded.board_scale_set ? loaded.board_scale : 1;
  game_state.antialias = loaded.antialias_set ? loaded.antialias : true;
  game_state.score_subscripts = loaded.score_subscripts_set
                                    ? loaded.score_subscripts
                                    : TUI_SCORE_SUBSCRIPTS_OFF;
  game_state.rack_sort =
      loaded.rack_sort_set ? loaded.rack_sort : TUI_RACK_SORT_ALPHA;
  session.pixel_supported = notcurses_canpixel(nc);
  session.font_available = game_state.glyph_cache != NULL;
  // If the user's saved scale=2 can't be honored on this machine, fall
  // back transparently rather than show a broken board. The user's
  // saved preference stays in tui.toml for next time.
  if (game_state.board_scale >= 2 &&
      (!session.pixel_supported || !session.font_available)) {
    game_state.board_scale = 1;
  }
  // Bot worker stays idle at launch — the startup menu picks the
  // game mode, and the time picker that follows ("Watch computer
  // play") is what actually fires off the first game. The pixel
  // worker is plumbing for rendering, unrelated to game-state, so
  // it can start right away.
  tui_pixel_worker_start(&game_state);

  // Modal state: which (if any) modal is open. Drives keyboard routing
  // and the status-bar control hints.
  ui.running = true;

  // Focus-event detection state. The terminal sends CSI I on
  // focus-in and CSI O on focus-out when DEC mode 1004 is on
  // (which we enabled above). Notcurses doesn't surface focus
  // events directly — it replays the bytes through the input
  // queue as ESC, '[', 'I' or 'O'. This 3-state machine watches
  // for that pattern. `focus_pending_esc` is set when the
  // sequence aborted with just an ESC buffered, so the next
  // drain pass can deliver a real Esc keypress that the modal
  // handlers expect. Mouse mode is auto-disabled on focus-out
  // and re-enabled on focus-in so the macOS screenshot UI
  // doesn't fight the terminal for cursor capture.
  ui.focus_state = 0; // 0 = normal, 1 = saw ESC, 2 = saw ESC '['
  ui.focus_pending_esc = false;
  // First-launch experience: show the startup menu before any game
  // gets played. Picking "Watch computer play" routes through the
  // time picker and resumes the bot-vs-bot flow that used to be
  // the default. Other modes (load position, load game, annotate,
  // play vs computer) are dimmed in the menu until wired.
  //
  // --watch on the command line skips the menu and starts a fresh
  // watch game using the saved (or default) settings. Useful for
  // debugging — reattaching with lldb on each crash without having
  // to click through the menu first.
  ui.modal = TUI_MODAL_STARTUP_MENU;
  if (session.args.watch) {
    pthread_mutex_lock(&game_state.mutex);
    tui_game_state_set_time_per_side(&game_state, session.chosen_time);
    tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
    pthread_mutex_unlock(&game_state.mutex);
    tui_bot_worker_start(&game_state);
    ui.modal = TUI_MODAL_NONE;
  }
  ui.startup_menu_focus = TUI_STARTUP_WATCH;
  ui.main_menu_focus = 0;
  ui.settings_focus = 0;
  // Where to return when Esc is pressed inside the Settings modal.
  // Reached from the main menu → return to the menu so the user can
  // pick another entry. Reached from the command-bar S → return to
  // no modal, since that's where the user was.
  ui.settings_return = TUI_MODAL_MAIN_MENU;
  ui.time_focus = 0;
  // Where Esc inside the time picker should return to. Reached
  // from the main menu's New Game → MAIN_MENU; reached via the
  // command bar's N / /new → NONE.
  ui.time_picker_return = TUI_MODAL_MAIN_MENU;
  // Where Esc inside the startup menu should return to. At app
  // launch there's no prior modal to return to (Esc just dismisses
  // it). When the user opens it via Esc → New game we want Esc to
  // step back to the main menu.
  ui.startup_menu_return = TUI_MODAL_NONE;
  // Watch-setup modal: row focus, pre-set to "Start game" so Enter
  // on first open kicks off the bot game with the displayed
  // defaults (time / lexicon / sim params).
  ui.watch_setup_focus = TUI_WATCH_SETUP_START;
  // Watch setup runs on its own copy of the lexicon + time control
  // so adjusters can preview without mutating the live session
  // settings. Initialized when the modal opens; committed to
  // chosen_lexicon / chosen_time only when the user hits "Start
  // game". Esc closes the modal and the locals are abandoned.
  ui.watch_setup_time = 0;
  // Play-vs-computer setup: editable player names, who moves first, and
  // the focused row / name caret. Defaults focus to Start so a quick
  // Enter launches with the defaults.
  ui.play_setup_focus = TUI_PLAY_SETUP_START;
  snprintf(ui.play_setup_human_name, sizeof(ui.play_setup_human_name), "%s",
           "You");
  snprintf(ui.play_setup_computer_name, sizeof(ui.play_setup_computer_name),
           "%s", "Computer");
  ui.play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
  ui.play_setup_name_cursor = 0;
  // Overtime rule + penalty rate scratch. Seeded from the config (or
  // its defaults when the file / keys are missing) and persisted on
  // Start; survives across modal opens within the session.
  ui.play_setup_overtime_rule = loaded.overtime_rule;
  ui.play_setup_overtime_cap = loaded.overtime_cap_minutes;
  ui.play_setup_penalty_rate = loaded.time_penalty_rate;
  ui.play_setup_challenge_rule = loaded.challenge_rule;
  ui.play_setup_challenge_penalty = loaded.challenge_penalty;
  // Annotate setup: lexicon (◀/▶ cycled, language-scoped) plus
  // two free-form player names. Same modal-local pattern as
  // Watch setup — commits to the session only on Start.
  ui.annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
  // Caret position within the currently-focused name field, in
  // bytes. Bounded by the name's strlen(); shared between P1 and
  // P2 because only one name row is focused at a time.
  ui.annotate_setup_name_cursor = 0;
  // Load-position modal state. Buffer holds the user-entered text
  // (raw CGP or a dragged file path); cursor is the byte offset
  // of the insertion point. The position is parsed live whenever
  // the buffer changes — `dirty` triggers a parse at the top of
  // the next frame; `parse_ok` records the last parse result so
  // Enter can fire only when the CGP is loadable. `error_msg`
  // displays the last parse / file error inside the modal until
  // the next edit clears it.
  ui.load_position_len = 0;
  ui.load_position_cursor = 0;
  ui.load_position_dirty = false;
  ui.load_position_parse_ok = false;
  // Load-game modal state. Mirrors the load-position modal but
  // holds a multi-line GCG game record. GCGs are typically much
  // bigger than CGPs (a 25-turn record can run several KB), so
  // the buffer is correspondingly larger.
  ui.load_game_len = 0;
  ui.load_game_cursor = 0;
  ui.load_game_dirty = false;
  ui.load_game_parse_ok = false;
  // Quit-confirmation modal: focus tracks Yes/No (0 = No, 1 = Yes),
  // default No since it's the safer option. quit_confirm_return is
  // the modal to return to when the user picks No / hits Esc; the
  // caller (main menu Q or command-bar Q) sets this before opening.
  ui.quit_confirm_focus = 0;
  ui.quit_confirm_return = TUI_MODAL_NONE;
  // Modal-style lexicon picker state. Lazily allocated when the user
  // enters the modal; destroyed before exit.
  ui.lexicon_list = NULL;
  ui.lexicon_focus = 0;
  // Frame-pacing anchor: at the top of every iteration we sleep until
  // next_frame_deadline, then advance the deadline by FRAME_NS. Sitting
  // at the top means the various `continue` paths below can't bypass
  // the throttle the way they did with a bottom-of-loop sleep.
  struct timespec next_frame_deadline;
  clock_gettime(CLOCK_MONOTONIC, &next_frame_deadline);
  // Conditional-render bookkeeping. The 2x pixel board is drawn as ~225
  // per-cell sprixels; notcurses_render re-emits them on every frame
  // even when nothing changed, which pins a static screen (e.g. waiting
  // on the human in play-vs-computer) at a few fps and burns CPU. So we
  // only run the render path when something actually changed: input was
  // processed, the bot bumped render_version, a modal is open, the
  // wall-clock second ticked (live clock countdown), or the bot is
  // animating a spinner. Otherwise the last frame stays on screen
  // untouched.
  ui.frame_dirty = true; // render the first frame
  // Input→display latency probe: timestamp when input first dirtied the
  // current (not-yet-rendered) frame, so we can measure keypress-to-pixels.
  struct timespec input_dirty_ts = {0, 0};
  bool input_dirty_pending = false;
  uint64_t rendered_version = ~(uint64_t)0;
  long rendered_wall_sec = -1;
  while (ui.running) {
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long remaining_ns =
          (long)(next_frame_deadline.tv_sec - now.tv_sec) * 1000000000L +
          (long)(next_frame_deadline.tv_nsec - now.tv_nsec);
      if (remaining_ns > 0) {
        // On schedule — sleep the rest of the budget and advance the
        // deadline from where it was, so we hit a consistent 60fps.
        struct timespec sleep_ts = {.tv_sec = remaining_ns / 1000000000L,
                                    .tv_nsec = remaining_ns % 1000000000L};
        nanosleep(&sleep_ts, NULL);
        next_frame_deadline.tv_nsec += FRAME_NS;
      } else {
        // Last render exceeded FRAME_NS (typical when a bot play
        // invalidates the pixel-composite cache). Don't try to catch up
        // — re-anchor the deadline at now + FRAME_NS so subsequent
        // frames are paced from this late-but-current point. Otherwise
        // we'd sprint a few unthrottled frames until we caught up,
        // showing up as 200+fps spikes in the EMA on every move.
        next_frame_deadline = now;
        next_frame_deadline.tv_nsec += FRAME_NS;
      }
      if (next_frame_deadline.tv_nsec >= 1000000000L) {
        next_frame_deadline.tv_sec += next_frame_deadline.tv_nsec / 1000000000L;
        next_frame_deadline.tv_nsec %= 1000000000L;
      }
    }
    // Live-preview parse for the Load-position modal. The buffer
    // dirty flag is set by edits inside the modal's input handler;
    // we run the parse here (once per frame, regardless of how
    // many keys arrived in the burst) so the board / racks behind
    // the modal reflect the latest CGP text and Enter has a
    // definitive parse_ok flag to consult.
    if (ui.modal == TUI_MODAL_LOAD_POSITION && ui.load_position_dirty) {
      ui.load_position_dirty = false;
      char working[2048];
      snprintf(working, sizeof(working), "%s", ui.load_position_buf);
      // Strip leading + trailing whitespace.
      char *start = working;
      while (*start == ' ' || *start == '\t' || *start == '\n' ||
             *start == '\r') {
        start++;
      }
      size_t wlen = strlen(start);
      while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                          start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
        start[--wlen] = '\0';
      }
      // Strip surrounding quotes (terminals wrap dragged paths).
      if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                        (start[0] == '\'' && start[wlen - 1] == '\''))) {
        start[wlen - 1] = '\0';
        start++;
        wlen -= 2;
      }
      if (strncmp(start, "file://", 7) == 0) {
        start += 7;
        wlen -= 7;
      }
      // Heuristic for path vs raw CGP.
      bool looks_like_path =
          wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                       (wlen > 4 && strcmp(start + wlen - 4, ".cgp") == 0));
      if (looks_like_path) {
        for (size_t i = 0; i < wlen; i++) {
          if (start[i] == ' ' || start[i] == '\t' || start[i] == '\n') {
            looks_like_path = false;
            break;
          }
        }
      }
      char cgp_payload[4096];
      cgp_payload[0] = '\0';
      bool resolve_ok = true;
      if (wlen == 0) {
        ui.load_position_error[0] = '\0';
        resolve_ok = false;
      } else if (looks_like_path) {
        char path[1024];
        if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
          const char *home = getenv("HOME");
          if (home != NULL) {
            snprintf(path, sizeof(path), "%s%s", home,
                     start[1] == '\0' ? "" : start + 1);
          } else {
            snprintf(path, sizeof(path), "%s", start);
          }
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
          snprintf(ui.load_position_error, sizeof(ui.load_position_error),
                   "Cannot open %s", path);
          resolve_ok = false;
        } else {
          size_t n = fread(cgp_payload, 1, sizeof(cgp_payload) - 1, fp);
          fclose(fp);
          cgp_payload[n] = '\0';
        }
      } else {
        snprintf(cgp_payload, sizeof(cgp_payload), "%s", start);
      }
      if (resolve_ok) {
        // Stop the bot if a previous load started one (currently
        // we never start the bot on load, but be safe).
        // The analysis-resume worker reads history entries and the
        // endgame ctx — stop it before any reset / reconfigure.
        tui_analysis_worker_stop_and_join(&game_state);
        if (game_state.bot_started) {
          atomic_store(&game_state.bot_stop, true);
          pthread_join(game_state.bot_thread, NULL);
          game_state.bot_started = false;
          atomic_store(&game_state.bot_stop, false);
        }
        ErrorStack *err = error_stack_create();
        pthread_mutex_lock(&game_state.mutex);
        game_load_cgp(game_state.game, cgp_payload, err);
        const bool ok = error_stack_is_empty(err);
        if (ok) {
          // On success, reset per-turn history / cursors so the
          // panels reflect a fresh starting state for the loaded
          // position.
          for (int i = 0; i < game_state.history_count; i++) {
            TuiHistoryEntry *e = &game_state.history[i];
            if (e->board_before != NULL) {
              board_destroy(e->board_before);
              e->board_before = NULL;
            }
            if (e->rack_before != NULL) {
              rack_destroy(e->rack_before);
              e->rack_before = NULL;
            }
            if (e->opp_rack_before != NULL) {
              rack_destroy(e->opp_rack_before);
              e->opp_rack_before = NULL;
            }
            if (e->sim_results_saved != NULL) {
              sim_results_destroy(e->sim_results_saved);
              e->sim_results_saved = NULL;
            }
            if (e->endgame_moves_saved != NULL) {
              free(e->endgame_moves_saved);
              e->endgame_moves_saved = NULL;
              e->endgame_moves_saved_count = 0;
            }
            if (e->loaded_move != NULL) {
              free(e->loaded_move);
              e->loaded_move = NULL;
            }
          }
          game_state.history_count = 0;
          game_state.history_cursor = -1;
          game_state.analysis_cursor = -1;
          game_state.analysis_cursor_column = 0;
          game_state.analysis_anchored_move[0] = '\0';
          game_state.seconds_used[0] = 0.0;
          game_state.seconds_used[1] = 0.0;
          clock_gettime(CLOCK_MONOTONIC, &game_state.turn_started);
          // Seed a pending history entry for the upcoming turn so
          // the History panel shows "1." waiting for input, with
          // the on-turn player's rack snapshotted and clocks
          // reset to full. This matches how the bot worker
          // appends a pending entry at the start of every turn —
          // the user is now "the bot" deciding what comes next.
          const int on_turn = game_get_player_on_turn_index(game_state.game);
          const Rack *on_turn_rack =
              player_get_rack(game_get_player(game_state.game, on_turn));
          tui_bot_worker_append_pending_history(
              &game_state, on_turn, on_turn_rack,
              game_state.time_per_side_seconds);
          ui.load_position_error[0] = '\0';
          ui.load_position_parse_ok = true;
        } else {
          char *msg = error_stack_get_string_and_reset(err);
          snprintf(ui.load_position_error, sizeof(ui.load_position_error), "%s",
                   msg != NULL ? msg : "Parse error");
          free(msg);
          ui.load_position_parse_ok = false;
        }
        pthread_mutex_unlock(&game_state.mutex);
        error_stack_destroy(err);
      } else {
        ui.load_position_parse_ok = false;
      }
    }

    // Live-preview parse for the LOAD_GAME modal. Same shape as
    // LOAD_POSITION's parse pass: trim / unquote / strip file://,
    // decide path-vs-raw, read the file if needed, then feed the
    // GCG through gcg_parser_create + parse_gcg_settings +
    // parse_gcg_events. The events parser internally resets the
    // game and replays moves, so on success the game ends in
    // its final-state position.
    if (ui.modal == TUI_MODAL_LOAD_GAME && ui.load_game_dirty) {
      ui.load_game_dirty = false;
      // Working buffer big enough to copy the entire load buffer.
      // GCGs can be several KB; we keep this on the stack but
      // sized to the modal buffer.
      static char working[sizeof(ui.load_game_buf)];
      snprintf(working, sizeof(working), "%s", ui.load_game_buf);
      char *start = working;
      while (*start == ' ' || *start == '\t' || *start == '\n' ||
             *start == '\r') {
        start++;
      }
      size_t wlen = strlen(start);
      while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                          start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
        start[--wlen] = '\0';
      }
      if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                        (start[0] == '\'' && start[wlen - 1] == '\''))) {
        start[wlen - 1] = '\0';
        start++;
        wlen -= 2;
      }
      if (strncmp(start, "file://", 7) == 0) {
        start += 7;
        wlen -= 7;
      }
      bool looks_like_path =
          wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                       (wlen > 4 && strcmp(start + wlen - 4, ".gcg") == 0));
      if (looks_like_path) {
        // A path can't contain embedded newlines. Multi-line raw
        // GCGs will always fail this check.
        for (size_t i = 0; i < wlen; i++) {
          if (start[i] == '\n') {
            looks_like_path = false;
            break;
          }
        }
      }
      // GCG payload buffer — GCGs can be sizable (a long game
      // with notes can run into the tens of KB), so allocate
      // generously on the heap rather than blowing the stack.
      char *gcg_payload = NULL;
      bool resolve_ok = true;
      if (wlen == 0) {
        ui.load_game_error[0] = '\0';
        resolve_ok = false;
      } else if (looks_like_path) {
        char path[1024];
        if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
          const char *home = getenv("HOME");
          if (home != NULL) {
            snprintf(path, sizeof(path), "%s%s", home,
                     start[1] == '\0' ? "" : start + 1);
          } else {
            snprintf(path, sizeof(path), "%s", start);
          }
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
        // A path typed character by character passes through directory
        // prefixes ("/", "/Users", ...). fopen succeeds on a directory and
        // reads nothing, so reject anything that isn't a regular file.
        struct stat path_stat;
        const bool not_regular_file =
            stat(path, &path_stat) == 0 && !S_ISREG(path_stat.st_mode);
        FILE *fp = not_regular_file ? NULL : fopen(path, "rb");
        if (not_regular_file) {
          snprintf(ui.load_game_error, sizeof(ui.load_game_error),
                   "%s is not a file", path);
          resolve_ok = false;
        } else if (fp == NULL) {
          snprintf(ui.load_game_error, sizeof(ui.load_game_error),
                   "Cannot open %s", path);
          resolve_ok = false;
        } else {
          fseek(fp, 0, SEEK_END);
          long fsize = ftell(fp);
          fseek(fp, 0, SEEK_SET);
          if (fsize < 0 || fsize > (long)(1 << 20)) {
            // Cap at 1 MiB — anything bigger is almost certainly
            // not a real GCG.
            snprintf(ui.load_game_error, sizeof(ui.load_game_error),
                     "%s is too large", path);
            fclose(fp);
            resolve_ok = false;
          } else {
            gcg_payload = malloc((size_t)fsize + 1);
            size_t n = fread(gcg_payload, 1, (size_t)fsize, fp);
            fclose(fp);
            gcg_payload[n] = '\0';
          }
        }
      } else {
        gcg_payload = malloc(wlen + 1);
        memcpy(gcg_payload, start, wlen);
        gcg_payload[wlen] = '\0';
      }
      // Strip a trailing incomplete `>nickname: rack` line.
      // Quackle and cross-tables.com emit this as a hint about the
      // on-turn player's rack at the time the GCG was exported,
      // but it is not a valid GCG event — MAGPIE's strict parser
      // rejects the whole file otherwise. We just truncate it
      // here; loading the events that precede it is what the user
      // actually wants.
      if (resolve_ok && gcg_payload != NULL) {
        size_t plen = strlen(gcg_payload);
        while (plen > 0 &&
               (gcg_payload[plen - 1] == '\n' ||
                gcg_payload[plen - 1] == '\r' || gcg_payload[plen - 1] == ' ' ||
                gcg_payload[plen - 1] == '\t')) {
          gcg_payload[--plen] = '\0';
        }
        size_t line_start = plen;
        while (line_start > 0 && gcg_payload[line_start - 1] != '\n') {
          line_start--;
        }
        if (line_start < plen && gcg_payload[line_start] == '>') {
          int tokens = 0;
          bool in_token = false;
          for (size_t i = line_start; i < plen; i++) {
            const char c = gcg_payload[i];
            const bool ws = c == ' ' || c == '\t';
            if (!ws && !in_token) {
              tokens++;
              in_token = true;
            } else if (ws) {
              in_token = false;
            }
          }
          // Real GCG event lines have at least 4 whitespace-
          // separated tokens (the end-rack-points form). Anything
          // shorter is the trailing rack hint.
          if (tokens < 4) {
            gcg_payload[line_start] = '\0';
          }
        }
        // The GCG parser treats input with no lines as a fatal error, so
        // never hand it an empty payload (an empty file, or text that was
        // only the trailing rack hint).
        if (gcg_payload[0] == '\0') {
          snprintf(ui.load_game_error, sizeof(ui.load_game_error), "Empty GCG");
          resolve_ok = false;
        }
      }
      if (resolve_ok) {
        // The analysis-resume worker reads history entries and the
        // endgame ctx — stop it before any reset / reconfigure.
        tui_analysis_worker_stop_and_join(&game_state);
        if (game_state.bot_started) {
          atomic_store(&game_state.bot_stop, true);
          pthread_join(game_state.bot_thread, NULL);
          game_state.bot_started = false;
          atomic_store(&game_state.bot_stop, false);
        }
        ErrorStack *err = error_stack_create();
        GameHistory *history = game_history_create();
        GCGParser *parser = gcg_parser_create(gcg_payload, history,
                                              game_state.active_lexicon, err);
        bool ok = error_stack_is_empty(err);
        if (ok) {
          parse_gcg_settings(parser, err);
          ok = error_stack_is_empty(err);
        }
        pthread_mutex_lock(&game_state.mutex);
        if (ok) {
          parse_gcg_events(parser, game_state.game, err);
          ok = error_stack_is_empty(err);
        }
        if (ok) {
          for (int i = 0; i < game_state.history_count; i++) {
            TuiHistoryEntry *e = &game_state.history[i];
            if (e->board_before != NULL) {
              board_destroy(e->board_before);
              e->board_before = NULL;
            }
            if (e->rack_before != NULL) {
              rack_destroy(e->rack_before);
              e->rack_before = NULL;
            }
            if (e->opp_rack_before != NULL) {
              rack_destroy(e->opp_rack_before);
              e->opp_rack_before = NULL;
            }
            if (e->sim_results_saved != NULL) {
              sim_results_destroy(e->sim_results_saved);
              e->sim_results_saved = NULL;
            }
            if (e->endgame_moves_saved != NULL) {
              free(e->endgame_moves_saved);
              e->endgame_moves_saved = NULL;
              e->endgame_moves_saved_count = 0;
            }
            if (e->loaded_move != NULL) {
              free(e->loaded_move);
              e->loaded_move = NULL;
            }
          }
          game_state.history_count = 0;
          game_state.history_cursor = -1;
          game_state.analysis_cursor = -1;
          game_state.analysis_cursor_column = 0;
          game_state.analysis_anchored_move[0] = '\0';
          game_state.seconds_used[0] = 0.0;
          game_state.seconds_used[1] = 0.0;
          clock_gettime(CLOCK_MONOTONIC, &game_state.turn_started);

          // Wipe analysis state left over from any prior session in
          // this process. Without this, loading a GCG after watching
          // a Magpie-vs-Magpie game leaves the previous game's
          // endgame leaderboard and sim plays stranded in the
          // Analysis panel. The structs are owned by the
          // TuiGameState, so destroying + recreating is the safest
          // way to drop their internal data without needing a
          // MoveList for sim_results_reset.
          tui_endgame_snapshot_clear(&game_state.endgame_snapshot);
          atomic_store(&game_state.endgame_results_active, false);
          atomic_store(&game_state.endgame_results_turn_idx, -1);
          if (game_state.sim_results != NULL) {
            sim_results_destroy(game_state.sim_results);
          }
          game_state.sim_results = sim_results_create(0.005);
          atomic_store(&game_state.sim_results_active, false);
          atomic_store(&game_state.sim_results_turn_idx, -1);
          atomic_store(&game_state.peg_results_active, false);
          atomic_store(&game_state.peg_results_turn_idx, -1);

          // Surface real player names from the GCG so the pill
          // headers read "Quackle" / "New Player 1" instead of
          // the generic "P1" / "P2". Empty when not set.
          for (int p = 0; p < 2; p++) {
            const char *pname = game_history_player_get_name(history, p);
            if (pname != NULL) {
              snprintf(game_state.player_names[p],
                       sizeof(game_state.player_names[p]), "%s", pname);
            } else {
              game_state.player_names[p][0] = '\0';
            }
          }

          tui_gcg_import_history(&game_state, history);

          ui.load_game_error[0] = '\0';
          ui.load_game_parse_ok = true;
        } else {
          char *msg = error_stack_get_string_and_reset(err);
          snprintf(ui.load_game_error, sizeof(ui.load_game_error), "%s",
                   msg != NULL ? msg : "Parse error");
          free(msg);
          ui.load_game_parse_ok = false;
        }
        pthread_mutex_unlock(&game_state.mutex);
        gcg_parser_destroy(parser);
        game_history_destroy(history);
        error_stack_destroy(err);
      } else {
        ui.load_game_parse_ok = false;
      }
      free(gcg_payload);
    }

    // Decide whether this frame needs a render at all (see the
    // conditional-render note above the loop). Skipping the render path
    // on an unchanged frame avoids re-emitting the ~225-sprixel 2x board
    // every tick, which is what pinned a static screen at a few fps.
    struct timespec render_now;
    clock_gettime(CLOCK_MONOTONIC, &render_now);
    const uint64_t cur_render_version = atomic_load(&game_state.render_version);
    bool bot_animating = false;
    pthread_mutex_lock(&game_state.mutex);
    if (game_state.history_count > 0) {
      const TuiHistoryEntry *last_entry =
          &game_state.history[game_state.history_count - 1];
      // A pending bot turn shows an animated spinner — keep rendering so
      // it animates. The human's own pending turn has no spinner, so it
      // doesn't force renders (that's the static idle case we optimize).
      bot_animating = last_entry->pending &&
                      !(game_state.app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                        last_entry->player_idx == game_state.human_player_idx);
    }
    pthread_mutex_unlock(&game_state.mutex);
    const bool need_render = ui.frame_dirty || ui.modal != TUI_MODAL_NONE ||
                             cur_render_version != rendered_version ||
                             render_now.tv_sec != rendered_wall_sec ||
                             bot_animating;
    if (need_render) {
      // Time the FULL render path (cell composition + pixel ncblits AND
      // the notcurses_render emit) so the fps readout reflects the real
      // per-frame cost, not just the graphics emit. The mutex wait is
      // timed separately for the perf trace — a slow frame whose time is
      // all lock_us means contention with the bot worker, not rendering.
      struct timespec render_begin;
      clock_gettime(CLOCK_MONOTONIC, &render_begin);
      pthread_mutex_lock(&game_state.mutex);
      struct timespec lock_acquired;
      clock_gettime(CLOCK_MONOTONIC, &lock_acquired);
      const long lock_us =
          (long)(lock_acquired.tv_sec - render_begin.tv_sec) * 1000000L +
          (long)(lock_acquired.tv_nsec - render_begin.tv_nsec) / 1000L;
      tui_game_render(std_plane, theme, &game_state, session.chosen_time,
                      ui.modal);
      pthread_mutex_unlock(&game_state.mutex);
      if (ui.modal == TUI_MODAL_MAIN_MENU) {
        tui_game_render_menu(std_plane, theme, ui.main_menu_focus);
      } else if (ui.modal == TUI_MODAL_SETTINGS) {
        const char *current_lexicon = session.to_save.lexicon_set
                                          ? session.to_save.lexicon
                                          : session.chosen_lexicon;
        const bool current_load_rit = session.to_save.load_rit_set
                                          ? session.to_save.load_rit
                                          : session.initial_load_rit;
        tui_game_render_settings(
            std_plane, theme, ui.settings_focus, game_state.board_scale,
            game_state.antialias, game_state.score_subscripts,
            game_state.border_thickness, session.pixel_supported,
            session.font_available, game_state.premium_labels,
            game_state.blank_uppercase, game_state.rack_sort, current_lexicon,
            current_load_rit);
      } else if (ui.modal == TUI_MODAL_TIME_PICKER) {
        tui_game_render_time_picker(std_plane, theme, ui.time_focus);
      } else if (ui.modal == TUI_MODAL_LEXICON_PICKER &&
                 ui.lexicon_list != NULL) {
        tui_game_render_lexicon_picker(std_plane, theme, ui.lexicon_list,
                                       ui.lexicon_focus);
      } else if (ui.modal == TUI_MODAL_QUIT_CONFIRM) {
        tui_game_render_quit_confirm(std_plane, theme, ui.quit_confirm_focus);
      } else if (ui.modal == TUI_MODAL_STARTUP_MENU) {
        tui_game_render_startup_menu(std_plane, theme, ui.startup_menu_focus);
      } else if (ui.modal == TUI_MODAL_WATCH_SETUP) {
        // Render from the modal's own local copy of lexicon + time
        // so adjusters preview against the in-modal value, not the
        // live session value.
        if (ui.lexicon_list == NULL) {
          ui.lexicon_list = tui_lexicon_list_load();
        }
        char lang_buf[32] = "(unknown)";
        if (ui.lexicon_list != NULL) {
          const int idx =
              tui_lexicon_list_find(ui.lexicon_list, ui.watch_setup_lexicon);
          if (idx >= 0) {
            tui_lexicon_list_language_name(ui.lexicon_list, idx, lang_buf,
                                           sizeof(lang_buf));
          }
        }
        tui_game_render_watch_setup(
            std_plane, theme, ui.watch_setup_focus, ui.watch_setup_time,
            lang_buf, ui.watch_setup_lexicon, game_state.sim_plies,
            game_state.sim_candidates);
      } else if (ui.modal == TUI_MODAL_LOAD_POSITION) {
        tui_game_render_load_position(std_plane, theme, ui.load_position_buf,
                                      ui.load_position_cursor,
                                      ui.load_position_error);
      } else if (ui.modal == TUI_MODAL_LOAD_GAME) {
        tui_game_render_load_game(std_plane, theme, ui.load_game_buf,
                                  ui.load_game_cursor, ui.load_game_error);
      } else if (ui.modal == TUI_MODAL_ANNOTATE_SETUP) {
        tui_game_render_annotate_setup(
            std_plane, theme, ui.annotate_setup_focus,
            ui.annotate_setup_lexicon, ui.annotate_setup_p1_name,
            ui.annotate_setup_p2_name, ui.annotate_setup_name_cursor);
      } else if (ui.modal == TUI_MODAL_PLAY_SETUP) {
        if (ui.lexicon_list == NULL) {
          ui.lexicon_list = tui_lexicon_list_load();
        }
        char play_lang_buf[32] = "(unknown)";
        if (ui.lexicon_list != NULL) {
          const int idx =
              tui_lexicon_list_find(ui.lexicon_list, ui.watch_setup_lexicon);
          if (idx >= 0) {
            tui_lexicon_list_language_name(ui.lexicon_list, idx, play_lang_buf,
                                           sizeof(play_lang_buf));
          }
        }
        tui_game_render_play_setup(
            std_plane, theme, ui.play_setup_focus, ui.play_setup_human_name,
            ui.play_setup_computer_name, ui.play_setup_first_move,
            ui.play_setup_name_cursor, ui.watch_setup_time,
            ui.play_setup_overtime_rule, ui.play_setup_overtime_cap,
            ui.play_setup_penalty_rate, ui.play_setup_challenge_rule,
            ui.play_setup_challenge_penalty, play_lang_buf,
            ui.watch_setup_lexicon, game_state.sim_plies,
            game_state.sim_candidates);
      }
      // Time the UI thread's full render path so the debug overlay
      // can surface the worst-case frame in the last second. Captures
      // notcurses_render too, where the Kitty graphics emit lives.
      struct timespec frame_start;
      clock_gettime(CLOCK_MONOTONIC, &frame_start);
      notcurses_render(nc);
      struct timespec frame_end;
      clock_gettime(CLOCK_MONOTONIC, &frame_end);
      // Full render time (compose + blit + emit) drives the fps readout.
      const long frame_us =
          (long)(frame_end.tv_sec - render_begin.tv_sec) * 1000000L +
          (long)(frame_end.tv_nsec - render_begin.tv_nsec) / 1000L;
      // notcurses_render (graphics emit) time, for the perf trace only.
      const long emit_us =
          (long)(frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
          (long)(frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
      tui_debug_record_frame_us(frame_us);
      // Keypress-to-pixels latency: from when input first dirtied this
      // frame to when its render finished. -1 when this render wasn't
      // triggered by input (e.g. a clock tick).
      long input_lag_us = -1;
      if (input_dirty_pending) {
        input_lag_us =
            (long)(frame_end.tv_sec - input_dirty_ts.tv_sec) * 1000000L +
            (long)(frame_end.tv_nsec - input_dirty_ts.tv_nsec) / 1000L;
        input_dirty_pending = false;
      }
      // Publish the latest measured keypress latency to the status bar.
      // Only update on input-triggered frames so the last value persists
      // (clock-tick frames carry no latency and would otherwise blank it).
      if (input_lag_us >= 0) {
        tui_debug_set_input_lag_us(input_lag_us);
      }
      // Snapshot notcurses' sprixel emission counters so the debug
      // overlay can show whether re-emits happen on idle frames.
      {
        ncstats *st = notcurses_stats_alloc(nc);
        if (st != NULL) {
          notcurses_stats(nc, st);
          tui_debug_record_sprixel_stats(st->sprixelemissions,
                                         st->sprixelelisions);
          // Opt-in perf trace (MAGPIE_FPS_DEBUG=1) — logged to
          // /tmp/magpie_stderr.log. For each rendered frame: notcurses_render
          // wall time, sprixels emitted vs elided this frame (high emit = the
          // board planes are NOT eliding), and board tile blits this frame.
          if (getenv("MAGPIE_FPS_DEBUG") != NULL) {
            static uint64_t dbg_emit;
            static uint64_t dbg_elide;
            static unsigned long dbg_rack;
            static unsigned long dbg_rasters;
            const unsigned long cur_rack = tui_debug_rack_blits();
            const unsigned long cur_rasters = tui_debug_glyph_rasters();
            fprintf(stderr,
                    "[fps] full_us=%ld lock_us=%ld emit_us=%ld emit+=%llu "
                    "elide+=%llu blits=%d rack+=%lu rast+=%lu inv=%lu "
                    "input_lag_us=%ld\n",
                    frame_us, lock_us, emit_us,
                    (unsigned long long)(st->sprixelemissions - dbg_emit),
                    (unsigned long long)(st->sprixelelisions - dbg_elide),
                    tui_debug_last_tile_blits(), cur_rack - dbg_rack,
                    cur_rasters - dbg_rasters, tui_debug_tile_invalidations(),
                    input_lag_us);
            dbg_rack = cur_rack;
            dbg_rasters = cur_rasters;
            dbg_emit = st->sprixelemissions;
            dbg_elide = st->sprixelelisions;
          }
          free(st);
        }
      }
      rendered_version = cur_render_version;
      rendered_wall_sec = render_now.tv_sec;
      ui.frame_dirty = false;
    } // end if (need_render)

    // Service a pending SIGUSR1 screenshot request. Done after the frame
    // timing/stats so the (heavy) composite + PNG encode doesn't inflate
    // the measured frame time. Composites the captured pixel planes off-
    // terminal; see frame_dump.c.
    if (tui_frame_dump_pending()) {
      tui_frame_dump_write(nc, std_plane, theme, NULL);
    }

    // Input is polled non-blocking. Each frame, drain ALL pending
    // input before re-rendering — otherwise a paste (which arrives
    // as a burst of individual key events) re-renders once per
    // character and crawls visibly on screen. The inner do/while
    // keeps polling until notcurses_get returns 0 (queue empty);
    // handler `continue` statements naturally re-poll for the next
    // key inside this inner loop rather than skipping to the next
    // frame.
    do {
      const struct timespec nonblocking = {0, 0};
      ncinput input;
      uint32_t key;
      // If the previous drain pass ended with a buffered ESC that
      // never resolved into a focus event, synthesize an Esc
      // keypress now so the normal handlers fire (just one frame
      // late). Otherwise pull the next key from notcurses.
      // True only for this iteration when we manufactured the Esc
      // from a previously-buffered byte. Keeps the focus-event
      // detector below from re-buffering the same Esc forever —
      // which used to manifest as "first Esc does nothing, second
      // Esc finally fires" because each synthesized Esc fell back
      // into focus_state=1 and got stalled.
      bool synthesized_esc = false;
      if (ui.focus_pending_esc) {
        memset(&input, 0, sizeof(input));
        input.id = NCKEY_ESC;
        input.evtype = NCTYPE_PRESS;
        key = NCKEY_ESC;
        ui.focus_pending_esc = false;
        synthesized_esc = true;
      } else {
        key = notcurses_get(nc, &nonblocking, &input);
      }
      if (key == (uint32_t)-1) {
        ui.running = false;
        break;
      }
      if (key == 0) {
        // Input queue is empty. If we have an in-flight ESC
        // waiting for a follow-up byte, the burst has finished
        // without forming a focus sequence — schedule a real Esc
        // keypress for the next drain pass.
        if (ui.focus_state >= 1) {
          ui.focus_pending_esc = true;
        }
        // The buffered '[' (focus_state == 2) is dropped silently;
        // a bare ESC + '[' isn't meaningful to any of our modals,
        // so re-injecting it would be cosmetic noise. Keep the
        // simpler path.
        ui.focus_state = 0;
        // No more input this frame — drop out of the drain loop so
        // the outer while re-renders.
        break;
      }
      // A real key/mouse event arrived — mark the frame dirty so the
      // conditional-render gate above renders the result next tick.
      ui.frame_dirty = true;
      if (!input_dirty_pending) {
        clock_gettime(CLOCK_MONOTONIC, &input_dirty_ts);
        input_dirty_pending = true;
      }
      // Focus-event detection. We buffer ESC and ESC '[' silently
      // (the existing handlers never reach them while the sequence
      // is still in flight). On the third byte, either we
      // complete a focus event (CSI I / CSI O) — disable/enable
      // mouse mode and consume — or the sequence breaks and the
      // current byte falls through to normal handling. The
      // previously-buffered ESC/'[' are dropped; an Esc alone
      // followed in the same burst by an arbitrary key isn't a
      // pattern any of our modals expect.
      //
      // synthesized_esc skips this — that Esc came from our own
      // re-injection and is already known to be a real keypress;
      // re-buffering it would just deadlock.
      if (!synthesized_esc && ui.focus_state == 0 && key == NCKEY_ESC &&
          input.evtype != NCTYPE_RELEASE) {
        ui.focus_state = 1;
        continue;
      }
      if (ui.focus_state == 1) {
        if (key == '[') {
          ui.focus_state = 2;
          continue;
        }
        // Mismatch — treat the buffered ESC as a real Esc by
        // re-injecting it next iteration, then fall through with
        // the current key.
        ui.focus_pending_esc = true;
        ui.focus_state = 0;
        // Fall through; current key handled normally below.
      } else if (ui.focus_state == 2) {
        if (key == 'I' || key == 'O') {
          const bool focus_in = (key == 'I');
          if (focus_in && !ui.mouse_enabled) {
            notcurses_mice_enable(nc, mice_eventmask);
            ui.mouse_enabled = true;
          } else if (!focus_in && ui.mouse_enabled) {
            notcurses_mice_disable(nc);
            ui.mouse_enabled = false;
          }
          ui.focus_state = 0;
          continue;
        }
        // Mismatch on the third byte. Drop the buffered '['
        // and synthesize the original Esc on the next pass.
        ui.focus_pending_esc = true;
        ui.focus_state = 0;
        // Fall through.
      }
      if (input.evtype == NCTYPE_RELEASE) {
        // A scrollbar drag ends on any release event regardless of
        // where the cursor is — the user may have let go anywhere
        // on the screen.
        if (game_state.analysis_scrollbar_dragging) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.analysis_scrollbar_dragging = false;
          pthread_mutex_unlock(&game_state.mutex);
        }
        continue;
      }

      // ── Annotation cell editor ────────────────────────────────────
      // Two independent edit fields per pending entry: MOVE (row 1
      // "8H POND") and RACK (row 2 "AEINRT"). edit_field selects
      // which buffer is taking keystrokes; the other buffer keeps
      // its content so clicking between rows preserves work in
      // progress.
      // Mouse events fall through to the panel-router / click
      // handler below even while a field is being edited — that's
      // how clicking on the other row of the same entry switches
      // fields, and how clicking on a different panel deselects
      // the field.
      const bool is_mouse_event =
          key == NCKEY_BUTTON1 || key == NCKEY_BUTTON2 ||
          key == NCKEY_BUTTON3 || key == NCKEY_BUTTON4 ||
          key == NCKEY_BUTTON5 || key == NCKEY_BUTTON6 ||
          key == NCKEY_BUTTON7 || key == NCKEY_BUTTON8 ||
          key == NCKEY_BUTTON9 || key == NCKEY_BUTTON10 ||
          key == NCKEY_BUTTON11 || key == NCKEY_MOTION;
      if (ui.modal == TUI_MODAL_NONE && game_state.edit_history_idx >= 0 &&
          !is_mouse_event) {
        if (tui_input_cell_editor(&game_state, key, input)) {
          continue;
        }
      }

      if (tui_input_mouse(&game_state, std_plane, ui.modal, key, input)) {
        continue;
      }
      if (key == NCKEY_RESIZE) {
        unsigned new_rows = 0;
        unsigned new_cols = 0;
        notcurses_refresh(nc, &new_rows, &new_cols);
        ncplane_resize_simple(std_plane, new_rows, new_cols);
        // A font-size change is delivered as a resize and shifts every
        // cached pixel composite to the wrong size. Drop all child planes
        // so the next render rebuilds them at the new cell-pixel ratio.
        tui_game_render_reset_grids();
        continue;
      }

      if (tui_input_load_position(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_load_game(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_watch_setup(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_annotate_setup(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_play_setup(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_startup_menu(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_main_menu(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_settings(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_time_picker(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_lexicon_picker(&game_state, &ui, &session, key, input)) {
        continue;
      }

      if (tui_input_quit_confirm(&game_state, &ui, &session, key, input)) {
        continue;
      }

      // When the History cursor sits on a pending entry and the
      // user hasn't opened the editor yet, Tab / Enter / → / ↓ all
      // drop into the move cell. Up / Left stay reserved for
      // entry-to-entry navigation. This is the keyboard mirror of
      // the click-to-edit path — annotation users shouldn't need
      // the mouse to start typing the next field.
      if (ui.modal == TUI_MODAL_NONE && game_state.edit_history_idx < 0 &&
          game_state.focused_panel == TUI_FOCUS_HISTORY &&
          game_state.history_cursor >= 0 &&
          game_state.history_cursor < game_state.history_count &&
          game_state.history[game_state.history_cursor].pending &&
          (key == NCKEY_TAB || key == '\t' || key == NCKEY_ENTER ||
           key == '\r' || key == '\n' || key == NCKEY_RIGHT ||
           key == NCKEY_DOWN)) {
        pthread_mutex_lock(&game_state.mutex);
        const int target = game_state.history_cursor;
        const TuiHistoryEntry *e = &game_state.history[target];
        // Seed the buffers from the entry only when nothing has
        // been typed yet — preserves any in-flight text the user
        // had left in the buffer.
        if (game_state.edit_move_len == 0) {
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
        }
        if (game_state.edit_rack_len == 0) {
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          // Committed rack text on the entry is treated as
          // user-authored: don't snap it back to "match the
          // move's inferred letters" on the next keystroke.
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
        }
        game_state.edit_history_idx = target;
        // ↓ lands on the RACK row (mirrors the inside-edit
        // semantics where ↓ in MOVE switches to RACK). Everything
        // else opens the MOVE field. In play-vs-computer the rack
        // row is read-only (racks come from the bag), so ↓ opens
        // MOVE like everything else.
        game_state.edit_field =
            (key == NCKEY_DOWN &&
             game_state.app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER)
                ? TUI_EDIT_FIELD_RACK
                : TUI_EDIT_FIELD_MOVE;
        tui_game_state_parse_edit_buf(&game_state);
        pthread_mutex_unlock(&game_state.mutex);
        continue;
      }

      if (key == NCKEY_ESC) {
        ui.modal = TUI_MODAL_MAIN_MENU;
        ui.main_menu_focus = 0;
      } else if (key >= '0' && key <= '5') {
        // Direct panel focus hotkeys (no modal). '0' focuses the
        // command bar; '1'-'5' focus the corresponding panel. These
        // work globally regardless of which panel is currently
        // focused — typing digits never gets captured by a panel.
        pthread_mutex_lock(&game_state.mutex);
        const int new_focus = (int)(key - '0');
        if (new_focus != 0 && game_state.slash_active) {
          game_state.slash_active = false;
          game_state.slash_len = 0;
          game_state.slash_cursor = 0;
          game_state.slash_buf[0] = '\0';
        }
        game_state.focused_panel = new_focus;
        pthread_mutex_unlock(&game_state.mutex);
      } else if ((key == NCKEY_TAB || key == '\t') &&
                 !game_state.slash_active) {
        // Tab cycles forward through 0..5 (Command → Board → ... →
        // Analysis → Command); Shift-Tab cycles the other way.
        // Useful as a discoverability path — press Tab repeatedly to
        // walk every focus state. While slash mode is active Tab
        // means "autocomplete" instead, so it falls through to the
        // [0]-focused handler below.
        pthread_mutex_lock(&game_state.mutex);
        const int delta = ncinput_shift_p(&input) ? 5 : 1; // 5 = (-1 mod 6)
        const int new_focus = (game_state.focused_panel + delta) % 6;
        if (new_focus != 0 && game_state.slash_active) {
          game_state.slash_active = false;
          game_state.slash_len = 0;
          game_state.slash_cursor = 0;
          game_state.slash_buf[0] = '\0';
        }
        game_state.focused_panel = new_focus;
        pthread_mutex_unlock(&game_state.mutex);
      } else if (key == '/' && !game_state.slash_active) {
        // Global "/" — focuses [0] Command if it isn't already and
        // begins a slash-command. Lets the user kick off a command
        // from any panel without first pressing 0 / Tab to land on
        // the command bar.
        pthread_mutex_lock(&game_state.mutex);
        game_state.focused_panel = 0;
        game_state.slash_active = true;
        game_state.slash_len = 0;
        game_state.slash_cursor = 0;
        game_state.slash_buf[0] = '\0';
        pthread_mutex_unlock(&game_state.mutex);
      } else if ((key == 'c' || key == 'C') && !game_state.slash_active) {
        // Global copy hotkey: current position → clipboard as CGP.
        // Works from any panel focus; editing contexts (board entry,
        // history cells, slash input, modals) consume their keys
        // before this chain so a typed 'c' never lands here.
        pthread_mutex_lock(&game_state.mutex);
        tui_copy_position_cgp(&game_state);
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_BOARD &&
                 (key == NCKEY_ENTER || key == '\r' || key == '\n') &&
                 game_state.app_mode != TUI_APP_MODE_WATCH) {
        // Keyboard path into board move-entry (design doc: "Enter while
        // the Board panel is focused"): anchors at the board center
        // when open, else the last origin, else the first empty cell —
        // then the normal board-entry keys (letters, arrows, Space,
        // Backspace, Enter, Esc) take over. Mouse-free move entry.
        pthread_mutex_lock(&game_state.mutex);
        tui_board_entry_begin_keyboard(&game_state);
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_ANALYSIS &&
                 (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
                  key == NCKEY_RIGHT || key == 'k' || key == 'K' ||
                  key == 'j' || key == 'J' || key == 'h' || key == 'H' ||
                  key == 'l' || key == 'L' || key == NCKEY_PGUP ||
                  key == NCKEY_PGDOWN || key == NCKEY_HOME ||
                  key == NCKEY_END)) {
        // Analysis panel nav. -1 = cursor on the [5>] label;
        // 0..N-1 = on a visible candidate row. Up/Down (and j/k)
        // moves the cursor row; Left/Right (and h/l) toggles the
        // column: LEFT/h → RANK (cursor pins to a row index),
        // RIGHT/l → MOVE (cursor pins to the move at the current
        // row and follows it as the sim reorders). PageUp/PageDown
        // jump by the visible window height; Home/End jump to the
        // first/last candidate.
        const bool key_up = key == NCKEY_UP || key == 'k' || key == 'K';
        const bool key_down = key == NCKEY_DOWN || key == 'j' || key == 'J';
        const bool key_left = key == NCKEY_LEFT || key == 'h' || key == 'H';
        const bool key_right = key == NCKEY_RIGHT || key == 'l' || key == 'L';
        const bool key_pgup = key == NCKEY_PGUP;
        const bool key_pgdn = key == NCKEY_PGDOWN;
        const bool key_home = key == NCKEY_HOME;
        const bool key_end = key == NCKEY_END;
        pthread_mutex_lock(&game_state.mutex);
        const int total = game_state.last_rendered_analysis_row_count;
        const int view_h = atomic_load(&game_state.analysis_visible_rows);
        if (key_home) {
          game_state.analysis_cursor = 0;
        } else if (key_end && total > 0) {
          game_state.analysis_cursor = total - 1;
        } else if ((key_pgup || key_pgdn) && view_h > 0) {
          const int step = view_h - 1 > 1 ? view_h - 1 : 1;
          int target =
              game_state.analysis_cursor < 0 ? 0 : game_state.analysis_cursor;
          target += key_pgdn ? step : -step;
          if (target < 0) {
            target = 0;
          }
          if (total > 0 && target >= total) {
            target = total - 1;
          }
          game_state.analysis_cursor = target;
        } else if (key_up || key_down) {
          const int last = total - 1;
          if (key_down) {
            if (game_state.analysis_cursor < last) {
              game_state.analysis_cursor++;
            }
          } else {
            if (game_state.analysis_cursor > -1) {
              game_state.analysis_cursor--;
            }
          }
          // Re-anchor when in MOVE column so the cursor follows the
          // new row's move from this point on.
          if (game_state.analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE) {
            const int idx = game_state.analysis_cursor;
            if (idx >= 0 && idx < game_state.last_rendered_analysis_row_count) {
              snprintf(game_state.analysis_anchored_move,
                       sizeof(game_state.analysis_anchored_move), "%s",
                       game_state.last_rendered_analysis_rows[idx].move);
            } else {
              game_state.analysis_anchored_move[0] = '\0';
            }
          }
        } else if (key_left) {
          // Drop to RANK column. Discard anchor — the cursor sticks
          // to whatever row it's currently sitting at.
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        } else if (key_right) {
          // Switch to MOVE column. Capture the current row's move
          // text so the cursor will follow it as the sim reorders.
          const int idx = game_state.analysis_cursor;
          if (idx >= 0 && idx < game_state.last_rendered_analysis_row_count) {
            snprintf(game_state.analysis_anchored_move,
                     sizeof(game_state.analysis_anchored_move), "%s",
                     game_state.last_rendered_analysis_rows[idx].move);
            game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_MOVE;
          }
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_HISTORY &&
                 (key == NCKEY_HOME || key == NCKEY_END)) {
        // Home/End jump the cursor to the first / newest entry without
        // stepping through every turn — End is the quick "take me to
        // the live pending turn" gesture (and what a scripted driver
        // uses to reach the human's pending cell in play-vs-computer).
        pthread_mutex_lock(&game_state.mutex);
        const int prev_cursor = game_state.history_cursor;
        if (game_state.history_count > 0) {
          game_state.history_cursor =
              (key == NCKEY_HOME) ? 0 : game_state.history_count - 1;
        }
        if (game_state.history_cursor != prev_cursor) {
          game_state.analysis_cursor = 0;
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_HISTORY &&
                 (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
                  key == NCKEY_RIGHT || key == 'k' || key == 'K' ||
                  key == 'j' || key == 'J' || key == 'h' || key == 'H' ||
                  key == 'l' || key == 'L')) {
        // History panel keyboard nav (cursor is on a label, not in
        // an entry's edit fields). The full step-through sequence is
        //   [4>] → 1> → 1.MOVE → 1.RACK → 2> → 2.MOVE → 2.RACK → 3> → ...
        // Plain Right from a "N>" label enters the editor on turn N's
        // MOVE field; plain Left from "N>" enters the editor on the
        // PREVIOUS turn's RACK (or stays at -1 if already there).
        // Shift+arrow keeps the original label-only behavior (jumps
        // 1> → 2> → 3>); h/j/k/l aliases follow the same rules.
        const bool forward = key == NCKEY_DOWN || key == NCKEY_RIGHT ||
                             key == 'j' || key == 'J' || key == 'l' ||
                             key == 'L';
        const bool is_horizontal = key == NCKEY_LEFT || key == NCKEY_RIGHT ||
                                   key == 'h' || key == 'H' || key == 'l' ||
                                   key == 'L';
        const bool shift = ncinput_shift_p(&input);
        pthread_mutex_lock(&game_state.mutex);
        const int last = game_state.history_count - 1;
        const int prev_cursor = game_state.history_cursor;
        const bool step_into = is_horizontal && !shift;
        if (step_into && forward && game_state.history_cursor >= 0 &&
            game_state.history_cursor <= last) {
          // N> → N.MOVE
          TuiHistoryEntry *e = &game_state.history[game_state.history_cursor];
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
          game_state.edit_rack_carryover[0] = '\0';
          game_state.edit_history_idx = game_state.history_cursor;
          game_state.edit_field = TUI_EDIT_FIELD_MOVE;
          tui_game_state_parse_edit_buf(&game_state);
        } else if (step_into && !forward && game_state.history_cursor > 0) {
          // N> → (N-1).RACK
          const int target = game_state.history_cursor - 1;
          TuiHistoryEntry *e = &game_state.history[target];
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
          game_state.edit_rack_carryover[0] = '\0';
          game_state.edit_history_idx = target;
          game_state.history_cursor = target;
          game_state.edit_field = TUI_EDIT_FIELD_RACK;
          tui_game_state_parse_edit_buf(&game_state);
        } else if (forward) {
          if (game_state.history_cursor < last) {
            game_state.history_cursor++;
          }
        } else {
          if (game_state.history_cursor > -1) {
            game_state.history_cursor--;
          }
        }
        // Whenever the History cursor lands on a new turn, snap the
        // Analysis cursor back to row 0 so the panel highlights the
        // play actually made for that turn (or the top play of the
        // live in-progress analysis when on the label / a pending
        // entry). Keeps the board preview consistent with the row
        // the user is looking at.
        if (game_state.history_cursor != prev_cursor) {
          game_state.analysis_cursor = 0;
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == 0) {
        // Slash-mode input loop: typing /, letters, Tab, Backspace,
        // Enter, Esc all map to slash buffer behavior rather than
        // global hotkeys. Falls through to the alphabetical hotkeys
        // (N/S/Q) only when slash mode is NOT active.
        if (game_state.slash_active) {
          if (key == NCKEY_ESC) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_active = false;
            game_state.slash_len = 0;
            game_state.slash_cursor = 0;
            game_state.slash_buf[0] = '\0';
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_LEFT) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor > 0) {
              game_state.slash_cursor--;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_RIGHT) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor < game_state.slash_len) {
              game_state.slash_cursor++;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_HOME) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_cursor = 0;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_END) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_cursor = game_state.slash_len;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_DEL) {
            // Forward-delete: remove char at cursor.
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor < game_state.slash_len) {
              memmove(game_state.slash_buf + game_state.slash_cursor,
                      game_state.slash_buf + game_state.slash_cursor + 1,
                      (size_t)(game_state.slash_len - game_state.slash_cursor));
              game_state.slash_len--;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_BACKSPACE || key == 0x7F || key == '\b') {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor > 0) {
              // Remove the char before the cursor.
              memmove(
                  game_state.slash_buf + game_state.slash_cursor - 1,
                  game_state.slash_buf + game_state.slash_cursor,
                  (size_t)(game_state.slash_len - game_state.slash_cursor + 1));
              game_state.slash_len--;
              game_state.slash_cursor--;
            } else if (game_state.slash_len == 0) {
              // Backspace at the very start of an empty buffer exits.
              game_state.slash_active = false;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_TAB || key == '\t') {
            // Tab completes against the unique prefix match.
            static const char *cmd_names[] = {
                "copy", "exit", "new", "quit", "resume", "settings", "stop"};
            static const int n_cmds =
                (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
            const char *match = NULL;
            int n_match = 0;
            for (int i = 0; i < n_cmds; i++) {
              if ((int)strlen(cmd_names[i]) >= game_state.slash_len &&
                  strncmp(cmd_names[i], game_state.slash_buf,
                          (size_t)game_state.slash_len) == 0) {
                match = cmd_names[i];
                n_match++;
              }
            }
            if (n_match == 1 && match != NULL) {
              pthread_mutex_lock(&game_state.mutex);
              snprintf(game_state.slash_buf, sizeof(game_state.slash_buf), "%s",
                       match);
              game_state.slash_len = (int)strlen(match);
              game_state.slash_cursor = game_state.slash_len;
              pthread_mutex_unlock(&game_state.mutex);
            }
          } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
            // Execute the typed-or-completed command. Fall back to a
            // unique prefix match if user pressed Enter without
            // completing first.
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "%s", game_state.slash_buf);
            if (strcmp(cmd, "new") == 0 || strcmp(cmd, "n") == 0) {
              ui.modal = TUI_MODAL_TIME_PICKER;
              ui.time_focus =
                  tui_time_picker_closest_index(session.chosen_time);
              ui.time_picker_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "settings") == 0) {
              ui.modal = TUI_MODAL_SETTINGS;
              ui.settings_focus = 0;
              ui.settings_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
              ui.modal = TUI_MODAL_QUIT_CONFIRM;
              ui.quit_confirm_focus = 0;
              ui.quit_confirm_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "copy") == 0) {
              pthread_mutex_lock(&game_state.mutex);
              tui_copy_position_cgp(&game_state);
              pthread_mutex_unlock(&game_state.mutex);
            } else if (strcmp(cmd, "resume") == 0) {
              pthread_mutex_lock(&game_state.mutex);
              tui_analysis_worker_start(&game_state, game_state.history_cursor);
              pthread_mutex_unlock(&game_state.mutex);
            } else if (strcmp(cmd, "stop") == 0) {
              tui_analysis_worker_stop_and_join(&game_state);
            } else {
              // Try a unique prefix match.
              static const char *cmd_names[] = {
                  "copy", "exit", "new", "quit", "resume", "settings", "stop"};
              static const int n_cmds =
                  (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
              const char *match = NULL;
              int n_match = 0;
              for (int i = 0; i < n_cmds; i++) {
                if ((int)strlen(cmd_names[i]) >= game_state.slash_len &&
                    strncmp(cmd_names[i], game_state.slash_buf,
                            (size_t)game_state.slash_len) == 0) {
                  match = cmd_names[i];
                  n_match++;
                }
              }
              if (n_match == 1 && match != NULL) {
                if (strcmp(match, "new") == 0) {
                  ui.modal = TUI_MODAL_TIME_PICKER;
                  ui.time_focus =
                      tui_time_picker_closest_index(session.chosen_time);
                  ui.time_picker_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "settings") == 0) {
                  ui.modal = TUI_MODAL_SETTINGS;
                  ui.settings_focus = 0;
                  ui.settings_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "quit") == 0 ||
                           strcmp(match, "exit") == 0) {
                  ui.modal = TUI_MODAL_QUIT_CONFIRM;
                  ui.quit_confirm_focus = 0;
                  ui.quit_confirm_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "copy") == 0) {
                  pthread_mutex_lock(&game_state.mutex);
                  tui_copy_position_cgp(&game_state);
                  pthread_mutex_unlock(&game_state.mutex);
                } else if (strcmp(match, "resume") == 0) {
                  pthread_mutex_lock(&game_state.mutex);
                  tui_analysis_worker_start(&game_state,
                                            game_state.history_cursor);
                  pthread_mutex_unlock(&game_state.mutex);
                } else if (strcmp(match, "stop") == 0) {
                  tui_analysis_worker_stop_and_join(&game_state);
                }
              }
            }
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_active = false;
            game_state.slash_len = 0;
            game_state.slash_cursor = 0;
            game_state.slash_buf[0] = '\0';
            pthread_mutex_unlock(&game_state.mutex);
          } else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            // Insert (lowercased) at the cursor position rather than
            // always appending. Shifts the buffer tail right.
            const char ch = (key >= 'A' && key <= 'Z')
                                ? (char)(key + ('a' - 'A'))
                                : (char)key;
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_len < (int)sizeof(game_state.slash_buf) - 1) {
              memmove(
                  game_state.slash_buf + game_state.slash_cursor + 1,
                  game_state.slash_buf + game_state.slash_cursor,
                  (size_t)(game_state.slash_len - game_state.slash_cursor + 1));
              game_state.slash_buf[game_state.slash_cursor] = ch;
              game_state.slash_len++;
              game_state.slash_cursor++;
            }
            pthread_mutex_unlock(&game_state.mutex);
          }
        } else if (key == 'q' || key == 'Q') {
          ui.modal = TUI_MODAL_QUIT_CONFIRM;
          ui.quit_confirm_focus = 0;
          ui.quit_confirm_return = TUI_MODAL_NONE;
        } else if (key == 's' || key == 'S') {
          ui.modal = TUI_MODAL_SETTINGS;
          ui.settings_focus = 0;
          ui.settings_return = TUI_MODAL_NONE;
        } else if (key == 'n' || key == 'N') {
          ui.modal = TUI_MODAL_TIME_PICKER;
          ui.time_focus = tui_time_picker_closest_index(session.chosen_time);
          ui.time_picker_return = TUI_MODAL_NONE;
        }
      }
    } while (ui.running);

    // If this frame's input drain dirtied the frame, render it ASAP instead
    // of sleeping a full pacing interval first. The top-of-loop sleep would
    // otherwise add ~1 frame (~16ms) of keypress-to-pixels latency on top of
    // the (sub-millisecond) render — measured as the dominant input lag at
    // 1x, where the render itself is negligible. Collapsing the deadline to
    // "now" makes the next iteration skip the sleep and render immediately;
    // frame_dirty is always cleared by the render block, so steady-state
    // frames with no input still pace normally via the deadline advance.
    if (ui.frame_dirty) {
      clock_gettime(CLOCK_MONOTONIC, &next_frame_deadline);
    }
  }

  tui_game_state_destroy(&game_state);
  if (ui.lexicon_list != NULL) {
    tui_lexicon_list_destroy(ui.lexicon_list);
  }
  // Symmetric with the startup enable — politely turn off focus
  // reporting so the terminal isn't left in an unexpected mode
  // after we exit.
  (void)!write(STDOUT_FILENO, "\x1b[?1004l", 8);
  notcurses_stop(nc);
  return 0;
}

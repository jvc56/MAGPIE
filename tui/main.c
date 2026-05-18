#include "bot_worker.h"
#include "config.h"
#include "game_render.h"
#include "game_state.h"
#include "lexicon_picker.h"
#include "onboarding.h"
#include "theme.h"
#include "time_picker.h"
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
#include <time.h>
#include <unistd.h>

enum {
  TARGET_FPS = 60,
};

static const long FRAME_NS = 1000000000L / TARGET_FPS;

// ── Crash diagnostics ─────────────────────────────────────────────────────
//
// When the engine hits log_fatal (which abort()s) or anything else
// trips SIGSEGV/SIGBUS, we want to (a) put the terminal back in a
// usable state and (b) leave behind enough information to debug.
//
// notcurses_stop is NOT async-signal-safe but it's our only practical
// way to restore the tty from a handler — without it the user is left
// staring at smashed terminal state. The backtrace goes to a fixed
// file path so it survives even when stderr is restored to the tty
// and immediately scrolled away.

#define MAGPIE_CRASH_LOG "/tmp/magpie_crash.log"

static struct notcurses *g_crash_nc; // set when nc is alive; NULL otherwise

static void crash_write(int fd, const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  (void)!write(fd, s, n);
}

static void crash_handler(int signo) {
  // Restore the tty first so any text written afterward is readable.
  // Yes, technically not async-signal-safe — but the alternative is
  // an unusable terminal, which is worse.
  if (g_crash_nc != NULL) {
    struct notcurses *nc = g_crash_nc;
    g_crash_nc = NULL;
    notcurses_stop(nc);
  }

  int fd = open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fd = STDERR_FILENO;
  }
  crash_write(fd, "magpie_tui crashed: signal ");
  // signo is at most 2 digits; itoa manually since async-signal-safe.
  char buf[8];
  int n = 0;
  int s = signo < 0 ? -signo : signo;
  if (s == 0) {
    buf[n++] = '0';
  }
  while (s > 0 && n < (int)sizeof(buf)) {
    buf[n++] = (char)('0' + (s % 10));
    s /= 10;
  }
  for (int i = 0; i < n / 2; i++) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  (void)!write(fd, buf, (size_t)n);
  crash_write(fd, " (");
  const char *name = "?";
  switch (signo) {
  case SIGABRT:
    name = "SIGABRT";
    break;
  case SIGSEGV:
    name = "SIGSEGV";
    break;
  case SIGBUS:
    name = "SIGBUS";
    break;
  case SIGFPE:
    name = "SIGFPE";
    break;
  case SIGILL:
    name = "SIGILL";
    break;
  }
  crash_write(fd, name);
  crash_write(fd, ")\n\nBacktrace:\n");

  void *frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, fd);
  crash_write(fd, "\n");
  if (fd != STDERR_FILENO) {
    close(fd);
  }

  // Also dump to stderr so a `2>` redirect sees it.
  crash_write(STDERR_FILENO, "magpie_tui crashed: ");
  crash_write(STDERR_FILENO, name);
  crash_write(STDERR_FILENO, " — backtrace written to " MAGPIE_CRASH_LOG "\n");

  // Re-raise with the default handler so the process exits with the
  // right status (and gets dumped to a corefile when configured).
  signal(signo, SIG_DFL);
  raise(signo);
}

static void install_crash_handlers(void) {
  struct sigaction sa = {0};
  sa.sa_handler = crash_handler;
  sa.sa_flags = SA_RESETHAND; // one-shot; subsequent fault uses default
  sigemptyset(&sa.sa_mask);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  // Touch the log file so we can verify the handler at least got
  // installed — if /tmp/magpie_crash.log doesn't exist post-crash,
  // we know the install never ran or got clobbered.
  int fd = open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    crash_write(fd, "magpie_tui started, crash handlers installed\n");
    close(fd);
  }
}

typedef struct {
  const char *theme_arg;
  bool reconfigure;
  bool no_config;
  bool show_help;
  bool error;
} CliArgs;

static void print_usage(void) {
  fputs("Usage: magpie_tui [options]\n"
        "\n"
        "Options:\n"
        "  --theme <name>   one-shot theme override; one of:\n"
        "                     dark, light, dim, high_contrast\n"
        "  --reconfigure    re-run all setup pickers (theme, lexicon, time)\n"
        "  --no-config      skip reading and writing the saved settings\n"
        "  --help, -h       show this help and exit\n"
        "\n"
        "On first run interactive pickers ask for theme, lexicon, and time\n"
        "control. Settings are saved to $XDG_CONFIG_HOME/magpie/tui.toml\n"
        "(default ~/.config/magpie/tui.toml). Subsequent runs reuse those\n"
        "settings unless --reconfigure is passed.\n",
        stderr);
}

static CliArgs parse_args(int argc, char *argv[]) {
  CliArgs args = {
      .theme_arg = NULL,
      .reconfigure = false,
      .no_config = false,
      .show_help = false,
      .error = false,
  };
  for (int idx = 1; idx < argc; idx++) {
    const char *arg = argv[idx];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      args.show_help = true;
    } else if (strcmp(arg, "--reconfigure") == 0) {
      args.reconfigure = true;
    } else if (strcmp(arg, "--no-config") == 0) {
      args.no_config = true;
    } else if (strcmp(arg, "--theme") == 0) {
      if (idx + 1 >= argc) {
        fputs("magpie_tui: --theme requires an argument\n", stderr);
        args.error = true;
        return args;
      }
      const char *theme_id = argv[++idx];
      if (theme_get_by_id(theme_id) == NULL) {
        fprintf(stderr,
                "magpie_tui: unknown theme '%s'. "
                "Valid: dark, light, dim, high_contrast\n",
                theme_id);
        args.error = true;
        return args;
      }
      args.theme_arg = theme_id;
    } else {
      fprintf(stderr, "magpie_tui: unknown argument '%s'\n", arg);
      args.error = true;
      return args;
    }
  }
  return args;
}

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
  const CliArgs args = parse_args(argc, argv);
  if (args.error) {
    return 2;
  }
  if (args.show_help) {
    print_usage();
    return 0;
  }

  setlocale(LC_ALL, "");

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
  g_crash_nc = nc;
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
  notcurses_mice_enable(nc, NCMICE_BUTTON_EVENT);

  // Load saved settings (if any), then resolve each in order: theme first
  // (it controls the picker palette for the others), then lexicon, then
  // time. Any picker may be skipped if the value is already known and
  // --reconfigure was not passed.
  TuiConfig loaded = {0};
  const bool config_existed = !args.no_config && tui_config_load(&loaded);
  TuiConfig to_save = loaded;
  bool should_save = false;

  // THEME ---------------------------------------------------------------
  ThemeName chosen_theme = THEME_DARK;
  if (args.theme_arg != NULL) {
    chosen_theme = theme_get_by_id(args.theme_arg)->name;
  } else if (config_existed && loaded.theme_set && !args.reconfigure) {
    chosen_theme = loaded.theme;
  } else {
    const ThemeName initial =
        loaded.theme_set ? loaded.theme : theme_auto_detect(nc);
    chosen_theme = tui_onboarding_run(nc, initial);
    to_save.theme = chosen_theme;
    to_save.theme_set = true;
    should_save = true;
  }
  const Theme *theme = theme_get(chosen_theme);

  // LEXICON -------------------------------------------------------------
  char chosen_lexicon[TUI_LEXICON_NAME_MAX];
  chosen_lexicon[0] = '\0';
  if (config_existed && loaded.lexicon_set && !args.reconfigure) {
    strncpy(chosen_lexicon, loaded.lexicon, sizeof(chosen_lexicon) - 1);
    chosen_lexicon[sizeof(chosen_lexicon) - 1] = '\0';
  } else {
    const char *initial = loaded.lexicon_set ? loaded.lexicon : NULL;
    if (!tui_lexicon_picker_run(nc, theme, initial, chosen_lexicon,
                                sizeof(chosen_lexicon))) {
      notcurses_stop(nc);
      return 0;
    }
    strncpy(to_save.lexicon, chosen_lexicon, sizeof(to_save.lexicon) - 1);
    to_save.lexicon[sizeof(to_save.lexicon) - 1] = '\0';
    to_save.lexicon_set = true;
    should_save = true;
  }

  // TIME ----------------------------------------------------------------
  int chosen_time = 0;
  if (config_existed && loaded.time_per_side_set && !args.reconfigure) {
    chosen_time = loaded.time_per_side_seconds;
  } else {
    const int initial =
        loaded.time_per_side_set ? loaded.time_per_side_seconds : 0;
    chosen_time = tui_time_picker_run(nc, theme, initial);
    if (chosen_time < 0) {
      notcurses_stop(nc);
      return 0;
    }
    to_save.time_per_side_seconds = chosen_time;
    to_save.time_per_side_set = true;
    should_save = true;
  }

  if (should_save && !args.no_config) {
    tui_config_save(&to_save);
  }

  struct ncplane *std_plane = notcurses_stdplane(nc);

  // Initialize the game with the chosen lexicon.
  TuiGameState game_state = {0};
  char init_error[256] = {0};
  const uint64_t seed = (uint64_t)time(NULL);
  const bool initial_load_rit = loaded.load_rit_set ? loaded.load_rit : false;
  if (!tui_game_state_init(chosen_lexicon, seed, initial_load_rit, &game_state,
                           init_error, sizeof(init_error))) {
    render_init_error(std_plane, theme, chosen_lexicon, init_error);
    notcurses_render(nc);
    ncinput input;
    notcurses_get(nc, NULL, &input);
    notcurses_stop(nc);
    return 1;
  }

  tui_game_state_set_time_per_side(&game_state, chosen_time);
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
  const bool pixel_supported = notcurses_canpixel(nc);
  const bool font_available = game_state.glyph_cache != NULL;
  // If the user's saved scale=2 can't be honored on this machine, fall
  // back transparently rather than show a broken board. The user's
  // saved preference stays in tui.toml for next time.
  if (game_state.board_scale >= 2 && (!pixel_supported || !font_available)) {
    game_state.board_scale = 1;
  }
  tui_bot_worker_start(&game_state);
  tui_pixel_worker_start(&game_state);

  // Modal state: which (if any) modal is open. Drives keyboard routing
  // and the status-bar control hints.
  bool running = true;
  TuiModalState modal = TUI_MODAL_NONE;
  int main_menu_focus = 0;
  int settings_focus = 0;
  // Where to return when Esc is pressed inside the Settings modal.
  // Reached from the main menu → return to the menu so the user can
  // pick another entry. Reached from the command-bar S → return to
  // no modal, since that's where the user was.
  TuiModalState settings_return = TUI_MODAL_MAIN_MENU;
  int time_focus = 0;
  // Where Esc inside the time picker should return to. Reached
  // from the main menu's New Game → MAIN_MENU; reached via the
  // command bar's N / /new → NONE.
  TuiModalState time_picker_return = TUI_MODAL_MAIN_MENU;
  // Quit-confirmation modal: focus tracks Yes/No (0 = No, 1 = Yes),
  // default No since it's the safer option. quit_confirm_return is
  // the modal to return to when the user picks No / hits Esc; the
  // caller (main menu Q or command-bar Q) sets this before opening.
  int quit_confirm_focus = 0;
  TuiModalState quit_confirm_return = TUI_MODAL_NONE;
  // Modal-style lexicon picker state. Lazily allocated when the user
  // enters the modal; destroyed before exit.
  LexiconList *lexicon_list = NULL;
  int lexicon_focus = 0;
  // Settings rows for Antialias / Subscript / Border are hidden when
  // the board isn't rendering at 2x — they're 2x-only settings. The
  // renderer in game_render.c filters identically; this predicate
  // mirrors it so up/down navigation can skip past hidden rows.
  // Forward-declared lambda style: we capture the relevant state by
  // re-evaluating each call rather than threading args in.
#define SETTINGS_2X_ONLY(idx)                                                  \
  ((idx) == TUI_SETTINGS_AA || (idx) == TUI_SETTINGS_SUBSCRIPTS ||             \
   (idx) == TUI_SETTINGS_BORDER)
  // Frame-pacing anchor: at the top of every iteration we sleep until
  // next_frame_deadline, then advance the deadline by FRAME_NS. Sitting
  // at the top means the various `continue` paths below can't bypass
  // the throttle the way they did with a bottom-of-loop sleep.
  struct timespec next_frame_deadline;
  clock_gettime(CLOCK_MONOTONIC, &next_frame_deadline);
  while (running) {
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
    pthread_mutex_lock(&game_state.mutex);
    tui_game_render(std_plane, theme, &game_state, chosen_time, modal);
    pthread_mutex_unlock(&game_state.mutex);
    if (modal == TUI_MODAL_MAIN_MENU) {
      tui_game_render_menu(std_plane, theme, main_menu_focus);
    } else if (modal == TUI_MODAL_SETTINGS) {
      const char *current_lexicon =
          to_save.lexicon_set ? to_save.lexicon : chosen_lexicon;
      const bool current_load_rit =
          to_save.load_rit_set ? to_save.load_rit : initial_load_rit;
      tui_game_render_settings(
          std_plane, theme, settings_focus, game_state.board_scale,
          game_state.antialias, game_state.score_subscripts,
          game_state.border_thickness, pixel_supported, font_available,
          game_state.premium_labels, game_state.blank_uppercase,
          current_lexicon, current_load_rit);
    } else if (modal == TUI_MODAL_TIME_PICKER) {
      tui_game_render_time_picker(std_plane, theme, time_focus);
    } else if (modal == TUI_MODAL_LEXICON_PICKER && lexicon_list != NULL) {
      tui_game_render_lexicon_picker(std_plane, theme, lexicon_list,
                                     lexicon_focus);
    } else if (modal == TUI_MODAL_QUIT_CONFIRM) {
      tui_game_render_quit_confirm(std_plane, theme, quit_confirm_focus);
    }
    // Time the UI thread's full render path so the debug overlay
    // can surface the worst-case frame in the last second. Captures
    // notcurses_render too, where the Kitty graphics emit lives.
    extern void tui_debug_record_frame_us(long);
    struct timespec frame_start;
    clock_gettime(CLOCK_MONOTONIC, &frame_start);
    notcurses_render(nc);
    struct timespec frame_end;
    clock_gettime(CLOCK_MONOTONIC, &frame_end);
    const long frame_us =
        (long)(frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
        (long)(frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
    tui_debug_record_frame_us(frame_us);

    // Input is polled non-blocking; the throttle lives at the top of
    // the loop so `continue` paths can't bypass it.
    const struct timespec nonblocking = {0, 0};
    ncinput input;
    const uint32_t key = notcurses_get(nc, &nonblocking, &input);
    if (key == (uint32_t)-1) {
      running = false;
      break;
    }
    if (key == 0) {
      continue;
    }
    if (input.evtype == NCTYPE_RELEASE) {
      continue;
    }
    // Left mouse click → focus the panel under the cursor. Only
    // NCKEY_BUTTON1 (left-button press) for now; scroll wheel and
    // right-click are reserved for future per-panel interactions.
    if (key == NCKEY_BUTTON1 && modal == TUI_MODAL_NONE) {
      const int hit =
          tui_game_panel_at(std_plane, &game_state, input.y, input.x);
      if (hit >= 0) {
        pthread_mutex_lock(&game_state.mutex);
        if (hit != 0 && game_state.slash_active) {
          game_state.slash_active = false;
          game_state.slash_len = 0;
          game_state.slash_cursor = 0;
          game_state.slash_buf[0] = '\0';
        }
        // First click into a panel just focuses it (with the cursor
        // reset to the label). A click on History when it's ALREADY
        // focused reads the per-entry hit map to move the in-panel
        // cursor — clicks on the title / chrome row snap back to
        // -1, clicks on a turn jump to that entry.
        if (hit == TUI_FOCUS_HISTORY &&
            game_state.focused_panel == TUI_FOCUS_HISTORY) {
          const int target = tui_history_cursor_at(input.y, input.x);
          if (target >= -1) {
            game_state.history_cursor = target;
          }
        } else {
          game_state.focused_panel = hit;
        }
        pthread_mutex_unlock(&game_state.mutex);
      }
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

    if (modal == TUI_MODAL_MAIN_MENU) {
      if (key == NCKEY_ESC) {
        modal = TUI_MODAL_NONE;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        if (main_menu_focus > 0) {
          main_menu_focus--;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        if (main_menu_focus < TUI_MENU_ITEM_COUNT - 1) {
          main_menu_focus++;
        }
      } else if (key == 'n' || key == 'N') {
        // Mnemonic shortcuts trigger the action immediately, matching
        // the hint shown to the right of each item in the modal. They
        // skip focus-then-Enter so the menu behaves like a launcher.
        modal = TUI_MODAL_TIME_PICKER;
        time_focus = tui_time_picker_closest_index(chosen_time);
        time_picker_return = TUI_MODAL_MAIN_MENU;
      } else if (key == 's' || key == 'S') {
        modal = TUI_MODAL_SETTINGS;
        settings_focus = 0;
        settings_return = TUI_MODAL_MAIN_MENU;
      } else if (key == 'q' || key == 'Q') {
        modal = TUI_MODAL_QUIT_CONFIRM;
        quit_confirm_focus = 0;
        quit_confirm_return = TUI_MODAL_MAIN_MENU;
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        if (main_menu_focus == TUI_MENU_NEW_GAME) {
          // Pivot to the time-picker modal. The new-game side effects
          // (stop bot, reset, restart) happen when the user confirms
          // a time inside that modal; Esc there returns to this menu.
          modal = TUI_MODAL_TIME_PICKER;
          time_focus = tui_time_picker_closest_index(chosen_time);
          time_picker_return = TUI_MODAL_MAIN_MENU;
        } else if (main_menu_focus == TUI_MENU_SETTINGS) {
          modal = TUI_MODAL_SETTINGS;
          settings_focus = 0;
          settings_return = TUI_MODAL_MAIN_MENU;
        } else if (main_menu_focus == TUI_MENU_QUIT) {
          modal = TUI_MODAL_QUIT_CONFIRM;
          quit_confirm_focus = 0;
          quit_confirm_return = TUI_MODAL_MAIN_MENU;
        } else if (main_menu_focus == TUI_MENU_BACK) {
          modal = TUI_MODAL_NONE;
        }
      }
      continue;
    }

    if (modal == TUI_MODAL_SETTINGS) {
      // Any left/right keystroke at this modal might mutate visual
      // state (scale, AA, border, premium labels, blanks). Bumping
      // render_version once at the top invalidates the pixel-blit
      // caches that key off it, regardless of which branch below
      // actually toggled something — cheap and avoids scattering
      // atomic_fetch_add through every handler.
      if (key == NCKEY_LEFT || key == 'h' || key == 'H' || key == NCKEY_RIGHT ||
          key == 'l' || key == 'L') {
        atomic_fetch_add(&game_state.render_version, 1);
      }
      if (key == NCKEY_ESC) {
        // Esc returns to whichever modal opened Settings — main menu
        // when reached via Esc → Settings (so the user can navigate
        // to another menu entry without re-opening from scratch), or
        // back to no modal when reached via the command-bar S
        // shortcut.
        modal = settings_return;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        const bool effective_2x =
            pixel_supported && font_available && game_state.board_scale >= 2;
        int idx = settings_focus - 1;
        while (idx > 0 && SETTINGS_2X_ONLY(idx) && !effective_2x) {
          idx--;
        }
        if (idx >= 0) {
          settings_focus = idx;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        const bool effective_2x =
            pixel_supported && font_available && game_state.board_scale >= 2;
        int idx = settings_focus + 1;
        while (idx < TUI_SETTINGS_ITEM_COUNT - 1 && SETTINGS_2X_ONLY(idx) &&
               !effective_2x) {
          idx++;
        }
        if (idx < TUI_SETTINGS_ITEM_COUNT) {
          settings_focus = idx;
        }
      } else if (key == NCKEY_LEFT || key == 'h' || key == 'H') {
        if (settings_focus == TUI_SETTINGS_SCALE && pixel_supported &&
            font_available) {
          // Scale is a 2-state toggle (1, 2). Both arrows flip it.
          pthread_mutex_lock(&game_state.mutex);
          game_state.board_scale = game_state.board_scale == 2 ? 1 : 2;
          const int v = game_state.board_scale;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.board_scale = v;
            to_save.board_scale_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_AA && pixel_supported &&
                   font_available && game_state.board_scale >= 2) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.antialias = !game_state.antialias;
          const bool v = game_state.antialias;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.antialias = v;
            to_save.antialias_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                   pixel_supported && font_available &&
                   game_state.board_scale >= 2) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.score_subscripts =
              (TuiScoreSubscripts)((game_state.score_subscripts +
                                    TUI_SCORE_SUBSCRIPTS_COUNT - 1) %
                                   TUI_SCORE_SUBSCRIPTS_COUNT);
          const TuiScoreSubscripts v = game_state.score_subscripts;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.score_subscripts = v;
            to_save.score_subscripts_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_BORDER && pixel_supported) {
          pthread_mutex_lock(&game_state.mutex);
          if (game_state.border_thickness > 0) {
            game_state.border_thickness--;
          }
          const int v = game_state.border_thickness;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.border_thickness = v;
            to_save.border_thickness_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_PREMIUM) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.premium_labels =
              (TuiPremiumLabels)((game_state.premium_labels +
                                  TUI_PREMIUM_LABELS_COUNT - 1) %
                                 TUI_PREMIUM_LABELS_COUNT);
          const TuiPremiumLabels v = game_state.premium_labels;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.premium_labels = v;
            to_save.premium_labels_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_BLANKS) {
          // Blanks is a two-state toggle, so left and right both flip it.
          pthread_mutex_lock(&game_state.mutex);
          game_state.blank_uppercase = !game_state.blank_uppercase;
          const bool v = game_state.blank_uppercase;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.blank_uppercase = v;
            to_save.blank_uppercase_set = true;
            tui_config_save(&to_save);
          }
        }
      } else if (key == NCKEY_RIGHT || key == 'l' || key == 'L') {
        if (settings_focus == TUI_SETTINGS_SCALE && pixel_supported &&
            font_available) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.board_scale = game_state.board_scale == 2 ? 1 : 2;
          const int v = game_state.board_scale;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.board_scale = v;
            to_save.board_scale_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_AA && pixel_supported &&
                   font_available && game_state.board_scale >= 2) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.antialias = !game_state.antialias;
          const bool v = game_state.antialias;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.antialias = v;
            to_save.antialias_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                   pixel_supported && font_available &&
                   game_state.board_scale >= 2) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.score_subscripts =
              (TuiScoreSubscripts)((game_state.score_subscripts + 1) %
                                   TUI_SCORE_SUBSCRIPTS_COUNT);
          const TuiScoreSubscripts v = game_state.score_subscripts;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.score_subscripts = v;
            to_save.score_subscripts_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_BORDER && pixel_supported) {
          pthread_mutex_lock(&game_state.mutex);
          if (game_state.border_thickness < 6) {
            game_state.border_thickness++;
          }
          const int v = game_state.border_thickness;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.border_thickness = v;
            to_save.border_thickness_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_PREMIUM) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.premium_labels =
              (TuiPremiumLabels)((game_state.premium_labels + 1) %
                                 TUI_PREMIUM_LABELS_COUNT);
          const TuiPremiumLabels v = game_state.premium_labels;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.premium_labels = v;
            to_save.premium_labels_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_BLANKS) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.blank_uppercase = !game_state.blank_uppercase;
          const bool v = game_state.blank_uppercase;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            to_save.blank_uppercase = v;
            to_save.blank_uppercase_set = true;
            tui_config_save(&to_save);
          }
        } else if (settings_focus == TUI_SETTINGS_RIT) {
          // RIT is a deferred toggle — write to config; the live
          // game keeps using whatever was loaded at game-state init.
          // The pending-change banner picks up the divergence and the
          // setting takes effect on the next New Game.
          const bool prev =
              to_save.load_rit_set ? to_save.load_rit : initial_load_rit;
          to_save.load_rit = !prev;
          to_save.load_rit_set = true;
          pthread_mutex_lock(&game_state.mutex);
          game_state.pending_load_rit = to_save.load_rit;
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            tui_config_save(&to_save);
          }
        }
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        if (settings_focus == TUI_SETTINGS_BACK) {
          modal = settings_return;
        } else if (settings_focus == TUI_SETTINGS_LEXICON) {
          // Pivot to the modal-style lexicon picker. Load the list
          // lazily; reuse it across opens. Focus the currently-
          // selected lexicon so the right language is expanded.
          if (lexicon_list == NULL) {
            lexicon_list = tui_lexicon_list_load();
          }
          if (lexicon_list != NULL) {
            const char *current =
                to_save.lexicon_set ? to_save.lexicon : chosen_lexicon;
            const int found = tui_lexicon_list_find(lexicon_list, current);
            lexicon_focus = found >= 0 ? found : 0;
            modal = TUI_MODAL_LEXICON_PICKER;
          }
        }
      }
      continue;
    }

    if (modal == TUI_MODAL_TIME_PICKER) {
      const int preset_count = tui_time_picker_preset_count();
      if (key == NCKEY_ESC) {
        modal = time_picker_return;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        if (time_focus > 0) {
          time_focus--;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        if (time_focus < preset_count - 1) {
          time_focus++;
        }
      } else if (key >= '1' && key <= (uint32_t)('0' + preset_count)) {
        time_focus = (int)(key - '1');
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        const int new_time = tui_time_picker_preset_seconds(time_focus);
        if (new_time > 0) {
          // Stop the bot.
          atomic_store(&game_state.bot_stop, true);
          pthread_join(game_state.bot_thread, NULL);
          game_state.bot_started = false;
          atomic_store(&game_state.bot_stop, false);
          chosen_time = new_time;
          if (!args.no_config) {
            to_save.time_per_side_seconds = new_time;
            to_save.time_per_side_set = true;
            tui_config_save(&to_save);
          }
          // Pending lexicon / RIT changes that need a full re-init?
          // If so, tear down the state and re-init with the new
          // settings (loading fresh tables). Otherwise the fast in-
          // place reset is enough.
          const bool needs_reinit =
              strcmp(game_state.pending_lexicon, game_state.active_lexicon) !=
                  0 ||
              game_state.pending_load_rit != game_state.active_load_rit;
          if (needs_reinit) {
            char new_lexicon[TUI_LEXICON_NAME_MAX];
            snprintf(new_lexicon, sizeof(new_lexicon), "%s",
                     game_state.pending_lexicon);
            const bool new_load_rit = game_state.pending_load_rit;
            tui_game_state_destroy(&game_state);
            char reinit_error[256] = {0};
            if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL),
                                     new_load_rit, &game_state, reinit_error,
                                     sizeof(reinit_error))) {
              // Re-init failed; fall back to the previously active
              // settings so the user isn't left without a playable
              // game. We've already torn the state down, so we have
              // to retry with the old values.
              if (!tui_game_state_init(chosen_lexicon, (uint64_t)time(NULL),
                                       initial_load_rit, &game_state,
                                       reinit_error, sizeof(reinit_error))) {
                running = false;
                modal = TUI_MODAL_NONE;
                continue;
              }
            } else {
              snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                       new_lexicon);
            }
            tui_game_state_set_time_per_side(&game_state, new_time);
          } else {
            pthread_mutex_lock(&game_state.mutex);
            tui_game_state_set_time_per_side(&game_state, new_time);
            tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
            pthread_mutex_unlock(&game_state.mutex);
          }
          tui_bot_worker_start(&game_state);
        }
        modal = TUI_MODAL_NONE;
      }
      continue;
    }

    if (modal == TUI_MODAL_LEXICON_PICKER) {
      const int n = tui_lexicon_list_count(lexicon_list);
      if (key == NCKEY_ESC) {
        modal = TUI_MODAL_SETTINGS;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        if (lexicon_focus > 0) {
          lexicon_focus--;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        if (lexicon_focus < n - 1) {
          lexicon_focus++;
        }
      } else if (key == NCKEY_HOME || key == 'g') {
        lexicon_focus = 0;
      } else if (key == NCKEY_END || key == 'G') {
        lexicon_focus = n - 1;
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        char picked[TUI_LEXICON_NAME_MAX] = {0};
        if (tui_lexicon_list_name(lexicon_list, lexicon_focus, picked,
                                  sizeof(picked))) {
          snprintf(to_save.lexicon, sizeof(to_save.lexicon), "%s", picked);
          to_save.lexicon_set = true;
          pthread_mutex_lock(&game_state.mutex);
          snprintf(game_state.pending_lexicon,
                   sizeof(game_state.pending_lexicon), "%s", picked);
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            tui_config_save(&to_save);
          }
        }
        modal = TUI_MODAL_SETTINGS;
      }
      continue;
    }

    if (modal == TUI_MODAL_QUIT_CONFIRM) {
      // Y / N shortcuts trigger their action regardless of focus.
      // Enter confirms whatever's focused (default No, the safer
      // option). Esc / N returns to whichever modal opened the
      // confirm (main menu when launched via Quit there, or NONE
      // when launched via the command-bar Q).
      if (key == NCKEY_ESC) {
        modal = quit_confirm_return;
      } else if (key == 'y' || key == 'Y') {
        running = false;
      } else if (key == 'n' || key == 'N') {
        modal = quit_confirm_return;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        if (quit_confirm_focus > 0) {
          quit_confirm_focus--;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        if (quit_confirm_focus < 1) {
          quit_confirm_focus++;
        }
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        if (quit_confirm_focus == 1) {
          running = false;
        } else {
          modal = quit_confirm_return;
        }
      }
      continue;
    }

    if (key == NCKEY_ESC) {
      modal = TUI_MODAL_MAIN_MENU;
      main_menu_focus = 0;
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
    } else if ((key == NCKEY_TAB || key == '\t') && !game_state.slash_active) {
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
    } else if (game_state.focused_panel == TUI_FOCUS_HISTORY &&
               (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
                key == NCKEY_RIGHT || key == 'k' || key == 'K' || key == 'j' ||
                key == 'J' || key == 'h' || key == 'H' || key == 'l' ||
                key == 'L')) {
      // History panel keyboard nav. Cursor positions: -1 = on the
      // "[4>" label (default upon entering focus); 0..N-1 = on
      // history entry idx. Down/Right move toward newer entries
      // (-1 → 0 → 1 → … → N-1); Up/Left move back toward the
      // label. Vim's h/j/k/l aliases work too for the keyboard-
      // centric users.
      const bool forward = key == NCKEY_DOWN || key == NCKEY_RIGHT ||
                           key == 'j' || key == 'J' || key == 'l' || key == 'L';
      pthread_mutex_lock(&game_state.mutex);
      const int last = game_state.history_count - 1;
      if (forward) {
        if (game_state.history_cursor < last) {
          game_state.history_cursor++;
        }
      } else {
        if (game_state.history_cursor > -1) {
          game_state.history_cursor--;
        }
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
          static const char *cmd_names[] = {"exit", "new", "quit", "settings"};
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
            modal = TUI_MODAL_TIME_PICKER;
            time_focus = tui_time_picker_closest_index(chosen_time);
            time_picker_return = TUI_MODAL_NONE;
          } else if (strcmp(cmd, "settings") == 0) {
            modal = TUI_MODAL_SETTINGS;
            settings_focus = 0;
            settings_return = TUI_MODAL_NONE;
          } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            modal = TUI_MODAL_QUIT_CONFIRM;
            quit_confirm_focus = 0;
            quit_confirm_return = TUI_MODAL_NONE;
          } else {
            // Try a unique prefix match.
            static const char *cmd_names[] = {"exit", "new", "quit",
                                              "settings"};
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
                modal = TUI_MODAL_TIME_PICKER;
                time_focus = tui_time_picker_closest_index(chosen_time);
                time_picker_return = TUI_MODAL_NONE;
              } else if (strcmp(match, "settings") == 0) {
                modal = TUI_MODAL_SETTINGS;
                settings_focus = 0;
                settings_return = TUI_MODAL_NONE;
              } else if (strcmp(match, "quit") == 0 ||
                         strcmp(match, "exit") == 0) {
                modal = TUI_MODAL_QUIT_CONFIRM;
                quit_confirm_focus = 0;
                quit_confirm_return = TUI_MODAL_NONE;
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
          const char ch = (key >= 'A' && key <= 'Z') ? (char)(key + ('a' - 'A'))
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
        modal = TUI_MODAL_QUIT_CONFIRM;
        quit_confirm_focus = 0;
        quit_confirm_return = TUI_MODAL_NONE;
      } else if (key == 's' || key == 'S') {
        modal = TUI_MODAL_SETTINGS;
        settings_focus = 0;
        settings_return = TUI_MODAL_NONE;
      } else if (key == 'n' || key == 'N') {
        modal = TUI_MODAL_TIME_PICKER;
        time_focus = tui_time_picker_closest_index(chosen_time);
        time_picker_return = TUI_MODAL_NONE;
      }
    }
  }

  tui_game_state_destroy(&game_state);
  if (lexicon_list != NULL) {
    tui_lexicon_list_destroy(lexicon_list);
  }
  notcurses_stop(nc);
  return 0;
}

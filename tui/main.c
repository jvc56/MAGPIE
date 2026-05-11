#include "bot_worker.h"
#include "config.h"
#include "game_render.h"
#include "game_state.h"
#include "lexicon_picker.h"
#include "onboarding.h"
#include "theme.h"
#include "time_picker.h"
#include <locale.h>
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
  TARGET_FPS = 60,
};

static const long FRAME_NS = 1000000000L / TARGET_FPS;

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
  notcurses_options opts = {
      .flags = NCOPTION_SUPPRESS_BANNERS,
  };
  struct notcurses *nc = notcurses_core_init(&opts, NULL);
  if (nc == NULL) {
    return 1;
  }
  // Hard-disable scrolling on the std plane: macOS Terminal can otherwise
  // scroll the alt screen when render coords overflow the visible area
  // mid-resize, and that scroll is irreversible.
  ncplane_set_scrolling(notcurses_stdplane(nc), false);

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
  if (!tui_game_state_init(chosen_lexicon, seed, &game_state, init_error,
                           sizeof(init_error))) {
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

  // Modal state: which (if any) modal is open. Drives keyboard routing
  // and the status-bar control hints.
  bool running = true;
  TuiModalState modal = TUI_MODAL_NONE;
  int main_menu_focus = 0;
  int settings_focus = 0;
  while (running) {
    pthread_mutex_lock(&game_state.mutex);
    tui_game_render(std_plane, theme, &game_state, chosen_time, modal);
    pthread_mutex_unlock(&game_state.mutex);
    if (modal == TUI_MODAL_MAIN_MENU) {
      tui_game_render_menu(std_plane, theme, main_menu_focus);
    } else if (modal == TUI_MODAL_SETTINGS) {
      tui_game_render_settings(std_plane, theme, settings_focus,
                               game_state.board_scale, game_state.antialias,
                               game_state.score_subscripts,
                               game_state.border_thickness, pixel_supported,
                               font_available, game_state.premium_labels,
                               game_state.blank_uppercase);
    }
    notcurses_render(nc);

    const struct timespec wait = {.tv_sec = 0, .tv_nsec = FRAME_NS};
    ncinput input;
    const uint32_t key = notcurses_get(nc, &wait, &input);
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
    if (key == NCKEY_RESIZE) {
      unsigned new_rows = 0;
      unsigned new_cols = 0;
      notcurses_refresh(nc, &new_rows, &new_cols);
      ncplane_resize_simple(std_plane, new_rows, new_cols);
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
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        if (main_menu_focus == TUI_MENU_SETTINGS) {
          modal = TUI_MODAL_SETTINGS;
          settings_focus = 0;
        } else if (main_menu_focus == TUI_MENU_QUIT) {
          running = false;
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
      if (key == NCKEY_LEFT || key == 'h' || key == 'H' ||
          key == NCKEY_RIGHT || key == 'l' || key == 'L') {
        atomic_fetch_add(&game_state.render_version, 1);
      }
      if (key == NCKEY_ESC) {
        // Esc returns to the main menu so the user can pick another
        // entry without re-opening from scratch.
        modal = TUI_MODAL_MAIN_MENU;
      } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
        if (settings_focus > 0) {
          settings_focus--;
        }
      } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
        if (settings_focus < TUI_SETTINGS_ITEM_COUNT - 1) {
          settings_focus++;
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
        }
      } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
        if (settings_focus == TUI_SETTINGS_BACK) {
          modal = TUI_MODAL_MAIN_MENU;
        }
      }
      continue;
    }

    if (key == NCKEY_ESC) {
      modal = TUI_MODAL_MAIN_MENU;
      main_menu_focus = 0;
    }
    // q/Q intentionally unbound — the user will be typing tiles. Quit
    // through Esc → Quit (or Ctrl-C signal).
  }

  tui_game_state_destroy(&game_state);
  notcurses_stop(nc);
  return 0;
}

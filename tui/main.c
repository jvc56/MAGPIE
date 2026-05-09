#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <notcurses/notcurses.h>
#include "config.h"
#include "onboarding.h"
#include "theme.h"

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
  fputs(
      "Usage: magpie_tui [options]\n"
      "\n"
      "Options:\n"
      "  --theme <name>   one-shot theme override; one of:\n"
      "                     dark, light, dim, high_contrast\n"
      "  --reconfigure    force the theme picker even when a saved choice "
      "exists\n"
      "  --no-config      skip reading and writing the saved theme\n"
      "  --help, -h       show this help and exit\n"
      "\n"
      "On first run an interactive theme picker is shown and the choice is\n"
      "saved to $XDG_CONFIG_HOME/magpie/tui.toml (default "
      "~/.config/magpie/tui.toml).\n"
      "Subsequent runs use that saved theme unless overridden.\n",
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

static ThemeName resolve_theme(struct notcurses *nc, const CliArgs *args,
                               TuiConfig *config_out, bool *should_save) {
  *should_save = false;

  if (args->theme_arg != NULL) {
    // parse_args already validated the id, so this lookup must succeed.
    const Theme *override = theme_get_by_id(args->theme_arg);
    return override->name;
  }

  const ThemeName auto_default = theme_auto_detect(nc);

  if (args->no_config) {
    return auto_default;
  }

  TuiConfig loaded = {.theme = THEME_DARK, .theme_set = false};
  const bool config_existed = tui_config_load(&loaded);

  const bool need_picker =
      args->reconfigure || !config_existed || !loaded.theme_set;
  if (need_picker) {
    const ThemeName initial =
        (config_existed && loaded.theme_set) ? loaded.theme : auto_default;
    const ThemeName chosen = tui_onboarding_run(nc, initial);
    config_out->theme = chosen;
    config_out->theme_set = true;
    *should_save = true;
    return chosen;
  }

  return loaded.theme;
}

static void render_frame(struct ncplane *plane, const Theme *theme,
                         uint64_t frame_idx) {
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < plane_cols; col++) {
    ncplane_putstr_yx(plane, 0, (int)col, " ");
  }
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI ");

  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 2, 4,
                    "Phase 1 skeleton — board renderer comes next.");

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, 4, 4, "Theme: ");
  theme_apply_fg(plane, theme->accent_fg);
  ncplane_putstr(plane, theme->label);

  theme_apply_fg(plane, theme->dim_fg);
  char counter[64];
  if (snprintf(counter, sizeof(counter), "frame %llu",
               (unsigned long long)frame_idx) > 0) {
    ncplane_putstr_yx(plane, 5, 4, counter);
  }

  theme_apply_fg(plane, theme->status_fg);
  if (plane_rows >= 2) {
    ncplane_putstr_yx(plane, (int)plane_rows - 2, 4, "● ready  ");
  }
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "(q or Esc to quit)");
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

  TuiConfig config = {.theme = THEME_DARK, .theme_set = false};
  bool should_save = false;
  const ThemeName chosen_theme =
      resolve_theme(nc, &args, &config, &should_save);
  if (should_save && !args.no_config) {
    tui_config_save(&config);
  }

  struct ncplane *std_plane = notcurses_stdplane(nc);
  const Theme *theme = theme_get(chosen_theme);

  uint64_t frame_idx = 0;
  bool running = true;
  while (running) {
    render_frame(std_plane, theme, frame_idx);
    notcurses_render(nc);
    frame_idx++;

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
    if (key == 'q' || key == 'Q' || key == NCKEY_ESC) {
      running = false;
    }
  }

  notcurses_stop(nc);
  return 0;
}

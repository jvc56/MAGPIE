#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <notcurses/notcurses.h>
#include "config.h"
#include "lexicon_picker.h"
#include "onboarding.h"
#include "theme.h"
#include "time_picker.h"

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

static void format_time_label(int seconds, char *buf, size_t buf_size) {
  if (seconds >= 60 && seconds % 60 == 0) {
    snprintf(buf, buf_size, "%d min", seconds / 60);
  } else if (seconds >= 60) {
    snprintf(buf, buf_size, "%d:%02d", seconds / 60, seconds % 60);
  } else {
    snprintf(buf, buf_size, "%d sec", seconds);
  }
}

static void render_frame(struct ncplane *plane, const Theme *theme,
                         const char *lexicon, int time_seconds,
                         uint64_t frame_idx) {
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
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI ");

  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 2, 4,
                    "Phase 1 skeleton — board renderer comes next.");

  // Settings summary.
  const int summary_top = 4;
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, summary_top, 4, "Theme:   ");
  theme_apply_fg(plane, theme->accent_fg);
  ncplane_putstr(plane, theme->label);

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, summary_top + 1, 4, "Lexicon: ");
  theme_apply_fg(plane, theme->accent_fg);
  ncplane_putstr(plane, lexicon != NULL ? lexicon : "(none)");

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, summary_top + 2, 4, "Time:    ");
  theme_apply_fg(plane, theme->accent_fg);
  char time_label[32];
  format_time_label(time_seconds, time_label, sizeof(time_label));
  ncplane_putstr(plane, time_label);
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, " per side");

  theme_apply_fg(plane, theme->dim_fg);
  char counter[64];
  if (snprintf(counter, sizeof(counter), "frame %llu",
               (unsigned long long)frame_idx) > 0) {
    ncplane_putstr_yx(plane, summary_top + 4, 4, counter);
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
  uint64_t frame_idx = 0;
  bool running = true;
  while (running) {
    render_frame(std_plane, theme, chosen_lexicon, chosen_time, frame_idx);
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

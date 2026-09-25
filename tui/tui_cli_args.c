#include "tui_cli_args.h"

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage(void) {
  fputs(
      "Usage: magpie_tui [options]\n"
      "\n"
      "Options:\n"
      "  --theme <name>   one-shot theme override; one of:\n"
      "                     dark, light, dim, high_contrast\n"
      "  --reconfigure    re-run all setup pickers (theme, lexicon, time)\n"
      "  --no-config      skip reading and writing the saved settings\n"
      "  --config <path>  read and write settings at <path> instead of the\n"
      "                   default location (for tests / alternate profiles)\n"
      "  --watch          skip the startup menu and start watching the bots\n"
      "                   play immediately with saved (or default) settings\n"
      "  --help, -h       show this help and exit\n"
      "\n"
      "On first run interactive pickers ask for theme, lexicon, and time\n"
      "control. Settings are saved to $XDG_CONFIG_HOME/magpie/tui.toml\n"
      "(default ~/.config/magpie/tui.toml), or to --config <path> if given.\n"
      "Subsequent runs reuse those settings unless --reconfigure is passed.\n",
      stderr);
}
CliArgs parse_args(int argc, char *argv[]) {
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
    } else if (strcmp(arg, "--watch") == 0) {
      args.watch = true;
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
    } else if (strcmp(arg, "--config") == 0) {
      if (idx + 1 >= argc) {
        fputs("magpie_tui: --config requires a path argument\n", stderr);
        args.error = true;
        return args;
      }
      args.config_path = argv[++idx];
    } else {
      fprintf(stderr, "magpie_tui: unknown argument '%s'\n", arg);
      args.error = true;
      return args;
    }
  }
  return args;
}

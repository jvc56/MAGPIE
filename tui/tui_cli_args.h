#ifndef TUI_TUI_CLI_ARGS_H
#define TUI_TUI_CLI_ARGS_H

#include <stdbool.h>

typedef struct {
  const char *theme_arg;
  const char *config_path;
  bool reconfigure;
  bool no_config;
  bool watch;
  bool show_help;
  bool error;
} CliArgs;

CliArgs parse_args(int argc, char *argv[]);
void print_usage(void);

#endif

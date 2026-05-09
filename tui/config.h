#ifndef TUI_CONFIG_H
#define TUI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "theme.h"

enum {
  TUI_CONFIG_PATH_MAX = 4096,
  TUI_LEXICON_NAME_MAX = 32,
};

typedef struct {
  ThemeName theme;
  bool theme_set;
  char lexicon[TUI_LEXICON_NAME_MAX];
  bool lexicon_set;
  int time_per_side_seconds;
  bool time_per_side_set;
} TuiConfig;

// Resolves config path: $XDG_CONFIG_HOME/magpie/tui.toml or
// $HOME/.config/magpie/tui.toml. Returns false if neither is usable.
bool tui_config_resolve_path(char *buf, size_t buf_size);

// Loads config from disk into *config (zeroed first). Returns true if the
// file existed and was readable; sets config->theme_set if a recognized
// theme key was found. Returns false if the file is missing.
bool tui_config_load(TuiConfig *config);

// Persists config, creating parent directories as needed. Returns true on
// success.
bool tui_config_save(const TuiConfig *config);

#endif

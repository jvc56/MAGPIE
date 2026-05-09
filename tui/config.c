#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "theme.h"

static bool ensure_parent_dirs(const char *file_path) {
  char path_copy[TUI_CONFIG_PATH_MAX];
  const size_t len = strlen(file_path);
  if (len + 1 > sizeof(path_copy)) {
    return false;
  }
  memcpy(path_copy, file_path, len + 1);

  char *last_slash = strrchr(path_copy, '/');
  if (last_slash == NULL) {
    return true;
  }
  *last_slash = '\0';
  if (path_copy[0] == '\0') {
    return true;
  }

  // mkdir each component along path_copy. Walking left-to-right, terminate
  // at each '/' and create the prefix.
  for (char *cursor = path_copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
        return false;
      }
      *cursor = '/';
    }
  }
  if (mkdir(path_copy, 0755) != 0 && errno != EEXIST) {
    return false;
  }
  return true;
}

bool tui_config_resolve_path(char *buf, size_t buf_size) {
  if (buf == NULL || buf_size == 0) {
    return false;
  }
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg != NULL && xdg[0] != '\0') {
    const int written = snprintf(buf, buf_size, "%s/magpie/tui.toml", xdg);
    return written > 0 && (size_t)written < buf_size;
  }
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return false;
  }
  const int written =
      snprintf(buf, buf_size, "%s/.config/magpie/tui.toml", home);
  return written > 0 && (size_t)written < buf_size;
}

static char *trim_inplace(char *line) {
  while (*line == ' ' || *line == '\t') {
    line++;
  }
  size_t len = strlen(line);
  while (len > 0 &&
         (line[len - 1] == ' ' || line[len - 1] == '\t' ||
          line[len - 1] == '\r' || line[len - 1] == '\n')) {
    line[--len] = '\0';
  }
  return line;
}

bool tui_config_load(TuiConfig *config) {
  if (config == NULL) {
    return false;
  }
  config->theme = THEME_DARK;
  config->theme_set = false;

  char path[TUI_CONFIG_PATH_MAX];
  if (!tui_config_resolve_path(path, sizeof(path))) {
    return false;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }

  char line[512];
  while (fgets(line, sizeof(line), file) != NULL) {
    char *trimmed = trim_inplace(line);
    if (*trimmed == '\0' || *trimmed == '#' || *trimmed == '[') {
      continue;
    }
    char *equals = strchr(trimmed, '=');
    if (equals == NULL) {
      continue;
    }
    // Split into key/value, trimming each side.
    char *key_end = equals;
    *key_end = '\0';
    while (key_end > trimmed &&
           (key_end[-1] == ' ' || key_end[-1] == '\t')) {
      key_end--;
      *key_end = '\0';
    }
    char *value = trim_inplace(equals + 1);
    if (*value == '"') {
      value++;
      char *quote_end = strchr(value, '"');
      if (quote_end != NULL) {
        *quote_end = '\0';
      }
    }

    if (strcmp(trimmed, "theme") == 0) {
      const Theme *theme = theme_get_by_id(value);
      if (theme != NULL) {
        config->theme = theme->name;
        config->theme_set = true;
      }
    }
  }

  fclose(file);
  return true;
}

bool tui_config_save(const TuiConfig *config) {
  if (config == NULL) {
    return false;
  }
  char path[TUI_CONFIG_PATH_MAX];
  if (!tui_config_resolve_path(path, sizeof(path))) {
    return false;
  }
  if (!ensure_parent_dirs(path)) {
    return false;
  }
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return false;
  }

  fputs("[tui]\n", file);
  if (config->theme_set) {
    const Theme *theme = theme_get(config->theme);
    fprintf(file, "theme = \"%s\"\n", theme->id);
  }

  if (fclose(file) != 0) {
    return false;
  }
  return true;
}

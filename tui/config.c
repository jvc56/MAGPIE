#include "config.h"

#include "theme.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
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
  config->lexicon[0] = '\0';
  config->lexicon_set = false;
  config->time_per_side_seconds = 0;
  config->time_per_side_set = false;
  config->border_thickness = 2;
  config->border_thickness_set = false;
  config->blank_uppercase = true;
  config->blank_uppercase_set = false;
  config->premium_labels = TUI_PREMIUM_LABELS_UPPERCASE;
  config->premium_labels_set = false;
  config->board_scale = 1;
  config->board_scale_set = false;
  config->antialias = true;
  config->antialias_set = false;
  config->score_subscripts = TUI_SCORE_SUBSCRIPTS_OFF;
  config->score_subscripts_set = false;

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
    while (key_end > trimmed && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
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
    } else if (strcmp(trimmed, "lexicon") == 0) {
      if (value[0] != '\0') {
        size_t len = strlen(value);
        if (len >= sizeof(config->lexicon)) {
          len = sizeof(config->lexicon) - 1;
        }
        memcpy(config->lexicon, value, len);
        config->lexicon[len] = '\0';
        config->lexicon_set = true;
      }
    } else if (strcmp(trimmed, "time_per_side_seconds") == 0) {
      char *endptr = NULL;
      const long parsed = strtol(value, &endptr, 10);
      if (endptr != value && parsed > 0 && parsed <= 24 * 60 * 60) {
        config->time_per_side_seconds = (int)parsed;
        config->time_per_side_set = true;
      }
    } else if (strcmp(trimmed, "border_thickness") == 0) {
      char *endptr = NULL;
      const long parsed = strtol(value, &endptr, 10);
      if (endptr != value && parsed >= 0 && parsed <= 8) {
        config->border_thickness = (int)parsed;
        config->border_thickness_set = true;
      }
    } else if (strcmp(trimmed, "blank_uppercase") == 0) {
      if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        config->blank_uppercase = true;
        config->blank_uppercase_set = true;
      } else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        config->blank_uppercase = false;
        config->blank_uppercase_set = true;
      }
    } else if (strcmp(trimmed, "premium_labels") == 0) {
      if (strcmp(value, "uppercase") == 0) {
        config->premium_labels = TUI_PREMIUM_LABELS_UPPERCASE;
        config->premium_labels_set = true;
      } else if (strcmp(value, "lowercase") == 0) {
        config->premium_labels = TUI_PREMIUM_LABELS_LOWERCASE;
        config->premium_labels_set = true;
      } else if (strcmp(value, "none") == 0) {
        config->premium_labels = TUI_PREMIUM_LABELS_NONE;
        config->premium_labels_set = true;
      }
    } else if (strcmp(trimmed, "board_scale") == 0) {
      char *endptr = NULL;
      const long parsed = strtol(value, &endptr, 10);
      if (endptr != value && (parsed == 1 || parsed == 2)) {
        config->board_scale = (int)parsed;
        config->board_scale_set = true;
      }
    } else if (strcmp(trimmed, "antialias") == 0) {
      if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        config->antialias = true;
        config->antialias_set = true;
      } else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        config->antialias = false;
        config->antialias_set = true;
      }
    } else if (strcmp(trimmed, "score_subscripts") == 0) {
      if (strcmp(value, "off") == 0) {
        config->score_subscripts = TUI_SCORE_SUBSCRIPTS_OFF;
        config->score_subscripts_set = true;
      } else if (strcmp(value, "nonzero") == 0) {
        config->score_subscripts = TUI_SCORE_SUBSCRIPTS_NONZERO;
        config->score_subscripts_set = true;
      } else if (strcmp(value, "all") == 0) {
        config->score_subscripts = TUI_SCORE_SUBSCRIPTS_ALL;
        config->score_subscripts_set = true;
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
  if (config->lexicon_set && config->lexicon[0] != '\0') {
    fprintf(file, "lexicon = \"%s\"\n", config->lexicon);
  }
  if (config->time_per_side_set && config->time_per_side_seconds > 0) {
    fprintf(file, "time_per_side_seconds = %d\n",
            config->time_per_side_seconds);
  }
  if (config->border_thickness_set) {
    fprintf(file, "border_thickness = %d\n", config->border_thickness);
  }
  if (config->blank_uppercase_set) {
    fprintf(file, "blank_uppercase = %s\n",
            config->blank_uppercase ? "true" : "false");
  }
  if (config->premium_labels_set) {
    const char *value = "uppercase";
    switch (config->premium_labels) {
    case TUI_PREMIUM_LABELS_LOWERCASE:
      value = "lowercase";
      break;
    case TUI_PREMIUM_LABELS_NONE:
      value = "none";
      break;
    case TUI_PREMIUM_LABELS_UPPERCASE:
    case TUI_PREMIUM_LABELS_COUNT:
    default:
      value = "uppercase";
      break;
    }
    fprintf(file, "premium_labels = \"%s\"\n", value);
  }
  if (config->board_scale_set) {
    fprintf(file, "board_scale = %d\n", config->board_scale);
  }
  if (config->antialias_set) {
    fprintf(file, "antialias = %s\n", config->antialias ? "true" : "false");
  }
  if (config->score_subscripts_set) {
    const char *value = "off";
    switch (config->score_subscripts) {
    case TUI_SCORE_SUBSCRIPTS_NONZERO:
      value = "nonzero";
      break;
    case TUI_SCORE_SUBSCRIPTS_ALL:
      value = "all";
      break;
    case TUI_SCORE_SUBSCRIPTS_OFF:
    case TUI_SCORE_SUBSCRIPTS_COUNT:
    default:
      value = "off";
      break;
    }
    fprintf(file, "score_subscripts = \"%s\"\n", value);
  }

  if (fclose(file) != 0) {
    return false;
  }
  return true;
}

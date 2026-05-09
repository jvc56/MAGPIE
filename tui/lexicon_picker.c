#include "lexicon_picker.h"

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <notcurses/notcurses.h>
#include "theme.h"

enum {
  LEXICON_NAME_MAX = 64,
  LEXICON_LIST_MAX = 256,
  LEXICON_DIR_PATHS = 3,
};

typedef struct {
  char names[LEXICON_LIST_MAX][LEXICON_NAME_MAX];
  int count;
  const char *source_dir;
} LexiconList;

static int compare_names(const void *lhs, const void *rhs) {
  return strcmp((const char *)lhs, (const char *)rhs);
}

static bool ends_with_kwg(const char *name) {
  const size_t len = strlen(name);
  if (len <= 4) {
    return false;
  }
  return strcmp(name + len - 4, ".kwg") == 0;
}

static bool scan_lexica_dir(const char *dir_path, LexiconList *list) {
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    return false;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && list->count < LEXICON_LIST_MAX) {
    if (!ends_with_kwg(entry->d_name)) {
      continue;
    }
    const size_t name_len = strlen(entry->d_name) - 4;
    if (name_len == 0 || name_len >= LEXICON_NAME_MAX) {
      continue;
    }
    memcpy(list->names[list->count], entry->d_name, name_len);
    list->names[list->count][name_len] = '\0';
    list->count++;
  }
  closedir(dir);
  if (list->count > 0) {
    qsort(list->names, (size_t)list->count, sizeof(list->names[0]),
          compare_names);
    list->source_dir = dir_path;
    return true;
  }
  return false;
}

static bool load_lexicon_list(LexiconList *list) {
  list->count = 0;
  list->source_dir = NULL;
  static const char *candidate_dirs[LEXICON_DIR_PATHS] = {
      "data/lexica",
      "../data/lexica",
      "./data/lexica",
  };
  for (int idx = 0; idx < LEXICON_DIR_PATHS; idx++) {
    if (scan_lexica_dir(candidate_dirs[idx], list)) {
      return true;
    }
  }
  return false;
}

static void fill_row(struct ncplane *plane, int row, unsigned cols) {
  for (unsigned col = 0; col < cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
}

static void render_picker(struct ncplane *plane, const Theme *theme,
                          const LexiconList *list, int focus,
                          int scroll_offset, int visible_rows) {
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  fill_row(plane, 0, plane_cols);
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — choose a lexicon ");

  theme_apply_bg(plane, theme->bg);

  const int list_top = 3;
  const int last_visible = scroll_offset + visible_rows;
  for (int row_idx = 0; row_idx < visible_rows; row_idx++) {
    const int item_idx = scroll_offset + row_idx;
    if (item_idx >= list->count) {
      break;
    }
    const int row = list_top + row_idx;
    if (item_idx == focus) {
      theme_apply_fg(plane, theme->accent_fg);
      ncplane_putstr_yx(plane, row, 4, "▶ ");
      theme_apply_fg(plane, theme->fg);
      ncplane_putstr(plane, list->names[item_idx]);
    } else {
      theme_apply_fg(plane, theme->dim_fg);
      ncplane_putstr_yx(plane, row, 6, list->names[item_idx]);
    }
  }

  // Scroll indicator if there's more above/below.
  theme_apply_fg(plane, theme->dim_fg);
  if (scroll_offset > 0) {
    ncplane_putstr_yx(plane, list_top - 1, 4, "  ↑ more above");
  }
  if (last_visible < list->count) {
    ncplane_putstr_yx(plane, list_top + visible_rows, 4, "  ↓ more below");
  }

  // Hints + count.
  char count_line[64];
  if (snprintf(count_line, sizeof(count_line), "%d lexica from %s",
               list->count,
               list->source_dir != NULL ? list->source_dir : "data/lexica") >
      0) {
    ncplane_putstr_yx(plane, (int)plane_rows - 4, 4, count_line);
  }
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4,
                    "↑/↓ or j/k navigate   PgUp/PgDn page   Enter confirm  "
                    " Esc cancel");
}

static void render_empty(struct ncplane *plane, const Theme *theme) {
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  fill_row(plane, 0, plane_cols);
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — no lexica found ");

  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 3, 4,
                    "Could not find any .kwg files under data/lexica.");

  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, 5, 4,
                    "Run ./download_data.sh from the repository root and "
                    "try again.");

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4,
                    "Press any key to dismiss.");
}

bool tui_lexicon_picker_run(struct notcurses *nc, const Theme *theme,
                            const char *initial, char *out_buf,
                            size_t out_buf_size) {
  if (nc == NULL || theme == NULL || out_buf == NULL || out_buf_size == 0) {
    return false;
  }
  static LexiconList list;
  if (!load_lexicon_list(&list)) {
    struct ncplane *empty_plane = notcurses_stdplane(nc);
    render_empty(empty_plane, theme);
    notcurses_render(nc);
    ncinput input;
    while (notcurses_get(nc, NULL, &input) == 0) {
      // Drain spurious zero returns; should not normally happen with
      // blocking get. Safety net only.
    }
    return false;
  }

  int focus = 0;
  if (initial != NULL && initial[0] != '\0') {
    for (int item_idx = 0; item_idx < list.count; item_idx++) {
      if (strcmp(list.names[item_idx], initial) == 0) {
        focus = item_idx;
        break;
      }
    }
  }

  struct ncplane *std_plane = notcurses_stdplane(nc);
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(std_plane, &plane_rows, &plane_cols);
  // Reserve top header (3 rows) + bottom hints/count (5 rows) = 8.
  int visible_rows = (int)plane_rows - 8;
  if (visible_rows < 1) {
    visible_rows = 1;
  }
  if (visible_rows > list.count) {
    visible_rows = list.count;
  }
  int scroll_offset = 0;

  while (true) {
    if (focus < scroll_offset) {
      scroll_offset = focus;
    } else if (focus >= scroll_offset + visible_rows) {
      scroll_offset = focus - visible_rows + 1;
    }
    if (scroll_offset < 0) {
      scroll_offset = 0;
    }

    render_picker(std_plane, theme, &list, focus, scroll_offset,
                  visible_rows);
    notcurses_render(nc);

    ncinput input;
    const uint32_t key = notcurses_get(nc, NULL, &input);
    if (key == (uint32_t)-1) {
      return false;
    }
    if (input.evtype == NCTYPE_RELEASE) {
      continue;
    }

    if (key == NCKEY_UP || key == 'k' || key == 'K') {
      focus = (focus + list.count - 1) % list.count;
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      focus = (focus + 1) % list.count;
    } else if (key == NCKEY_PGUP) {
      focus -= visible_rows;
      if (focus < 0) {
        focus = 0;
      }
    } else if (key == NCKEY_PGDOWN) {
      focus += visible_rows;
      if (focus >= list.count) {
        focus = list.count - 1;
      }
    } else if (key == NCKEY_HOME || key == 'g') {
      focus = 0;
    } else if (key == NCKEY_END || key == 'G') {
      focus = list.count - 1;
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      const char *chosen = list.names[focus];
      const size_t chosen_len = strlen(chosen);
      if (chosen_len + 1 > out_buf_size) {
        return false;
      }
      memcpy(out_buf, chosen, chosen_len + 1);
      return true;
    } else if (key == NCKEY_ESC || key == 'q' || key == 'Q') {
      return false;
    }
  }
}

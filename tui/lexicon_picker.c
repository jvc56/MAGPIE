#include "lexicon_picker.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <notcurses/notcurses.h>
#include "theme.h"
#include "tui_resize.h"

enum {
  LEXICON_NAME_MAX = 64,
  LEXICON_LIST_MAX = 256,
  LEXICON_DIR_PATHS = 3,
};

// Mirrors ld_get_type_from_lex_name in src/ent/letter_distribution.h. Update
// this table when the engine adds new lexicon-prefix → language mappings.
typedef enum {
  LEX_LANG_ENGLISH,
  LEX_LANG_FRENCH,
  LEX_LANG_GERMAN,
  LEX_LANG_CATALAN,
  LEX_LANG_DUTCH,
  LEX_LANG_NORWEGIAN,
  LEX_LANG_POLISH,
  LEX_LANG_OTHER,
  LEX_LANG_COUNT,
} LexLang;

static const char *lang_label(LexLang lang) {
  switch (lang) {
  case LEX_LANG_ENGLISH:
    return "English";
  case LEX_LANG_FRENCH:
    return "French";
  case LEX_LANG_GERMAN:
    return "German";
  case LEX_LANG_CATALAN:
    return "Catalan";
  case LEX_LANG_DUTCH:
    return "Dutch";
  case LEX_LANG_NORWEGIAN:
    return "Norwegian";
  case LEX_LANG_POLISH:
    return "Polish";
  case LEX_LANG_OTHER:
  case LEX_LANG_COUNT:
    return "Other";
  }
  return "Other";
}

static bool has_iprefix(const char *str, const char *prefix) {
  while (*prefix != '\0') {
    if (*str == '\0') {
      return false;
    }
    if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) {
      return false;
    }
    str++;
    prefix++;
  }
  return true;
}

static LexLang classify_lexicon(const char *name) {
  if (has_iprefix(name, "CSW") || has_iprefix(name, "NWL") ||
      has_iprefix(name, "OSPD") || has_iprefix(name, "OSW") ||
      has_iprefix(name, "TWL") || has_iprefix(name, "America") ||
      has_iprefix(name, "CEL")) {
    return LEX_LANG_ENGLISH;
  }
  if (has_iprefix(name, "FRA")) {
    return LEX_LANG_FRENCH;
  }
  // Note: must check OSPS before NSF/RD/DSW/DISC because other prefixes
  // share initial letters; ordering here mirrors the engine.
  if (has_iprefix(name, "OSPS")) {
    return LEX_LANG_POLISH;
  }
  if (has_iprefix(name, "DISC")) {
    return LEX_LANG_CATALAN;
  }
  if (has_iprefix(name, "DSW")) {
    return LEX_LANG_DUTCH;
  }
  if (has_iprefix(name, "NSF")) {
    return LEX_LANG_NORWEGIAN;
  }
  if (has_iprefix(name, "RD")) {
    return LEX_LANG_GERMAN;
  }
  return LEX_LANG_OTHER;
}

typedef struct {
  char name[LEXICON_NAME_MAX];
  LexLang lang;
  int word_count;  // -1 if the .txt sibling could not be read
} LexiconEntry;

static int count_lines_in_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return -1;
  }
  int count = 0;
  char buf[8192];
  size_t bytes_read;
  while ((bytes_read = fread(buf, 1, sizeof(buf), file)) > 0) {
    for (size_t i = 0; i < bytes_read; i++) {
      if (buf[i] == '\n') {
        count++;
      }
    }
  }
  fclose(file);
  return count;
}

static void format_with_commas(int value, char *out, size_t out_size) {
  if (out_size == 0) {
    return;
  }
  char raw[16];
  const int raw_len = snprintf(raw, sizeof(raw), "%d", value);
  if (raw_len <= 0 || raw_len >= (int)sizeof(raw)) {
    out[0] = '\0';
    return;
  }
  size_t out_idx = 0;
  for (int idx = 0; idx < raw_len; idx++) {
    if (idx > 0 && (raw_len - idx) % 3 == 0) {
      if (out_idx + 1 >= out_size) {
        break;
      }
      out[out_idx++] = ',';
    }
    if (out_idx + 1 >= out_size) {
      break;
    }
    out[out_idx++] = raw[idx];
  }
  out[out_idx < out_size ? out_idx : out_size - 1] = '\0';
}

typedef struct {
  LexiconEntry entries[LEXICON_LIST_MAX];
  int count;
  const char *source_dir;
  // For each entry, the display row it occupies once language headers are
  // inserted before each group.
  int entry_display_row[LEXICON_LIST_MAX];
  int total_display_rows;
} LexiconList;

static int compare_entries(const void *lhs, const void *rhs) {
  const LexiconEntry *left = (const LexiconEntry *)lhs;
  const LexiconEntry *right = (const LexiconEntry *)rhs;
  if (left->lang != right->lang) {
    return (int)left->lang - (int)right->lang;
  }
  // Within a language, sort by word count descending. Entries with an
  // unknown count (-1) fall to the bottom of the group.
  if (left->word_count != right->word_count) {
    if (left->word_count < 0) {
      return 1;
    }
    if (right->word_count < 0) {
      return -1;
    }
    return left->word_count > right->word_count ? -1 : 1;
  }
  // Stable tie-break.
  return strcmp(left->name, right->name);
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
    LexiconEntry *out = &list->entries[list->count];
    memcpy(out->name, entry->d_name, name_len);
    out->name[name_len] = '\0';
    out->lang = classify_lexicon(out->name);
    char txt_path[512];
    const int path_written =
        snprintf(txt_path, sizeof(txt_path), "%s/%s.txt", dir_path, out->name);
    if (path_written > 0 && (size_t)path_written < sizeof(txt_path)) {
      out->word_count = count_lines_in_file(txt_path);
    } else {
      out->word_count = -1;
    }
    list->count++;
  }
  closedir(dir);
  if (list->count == 0) {
    return false;
  }

  qsort(list->entries, (size_t)list->count, sizeof(list->entries[0]),
        compare_entries);
  // Compute display rows: a header precedes each language group.
  LexLang prev_lang = LEX_LANG_COUNT;
  int display_row = 0;
  for (int idx = 0; idx < list->count; idx++) {
    if (list->entries[idx].lang != prev_lang) {
      // Reserve a row for the language header.
      display_row++;
      prev_lang = list->entries[idx].lang;
    }
    list->entry_display_row[idx] = display_row;
    display_row++;
  }
  list->total_display_rows = display_row;
  list->source_dir = dir_path;
  return true;
}

static bool load_lexicon_list(LexiconList *list) {
  list->count = 0;
  list->source_dir = NULL;
  list->total_display_rows = 0;
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
  tui_sync_plane_to_terminal(plane);
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

  // Walk display rows (headers + entries) in order, emitting only the
  // ones inside the visible window.
  LexLang prev_lang = LEX_LANG_COUNT;
  int display_row = 0;
  for (int idx = 0; idx < list->count; idx++) {
    if (list->entries[idx].lang != prev_lang) {
      if (display_row >= scroll_offset && display_row < last_visible) {
        const int screen_row = list_top + (display_row - scroll_offset);
        theme_apply_fg(plane, theme->status_fg);
        ncplane_putstr_yx(plane, screen_row, 2, "── ");
        ncplane_putstr(plane, lang_label(list->entries[idx].lang));
        ncplane_putstr(plane, " ──");
      }
      prev_lang = list->entries[idx].lang;
      display_row++;
    }
    if (display_row >= scroll_offset && display_row < last_visible) {
      const int screen_row = list_top + (display_row - scroll_offset);
      if (idx == focus) {
        theme_apply_fg(plane, theme->accent_fg);
        ncplane_putstr_yx(plane, screen_row, 4, "▶ ");
        theme_apply_fg(plane, theme->fg);
        ncplane_putstr(plane, list->entries[idx].name);
      } else {
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, screen_row, 6, list->entries[idx].name);
      }
      if (list->entries[idx].word_count >= 0) {
        // Right-align the count column so digits line up.
        char count_str[16];
        format_with_commas(list->entries[idx].word_count, count_str,
                           sizeof(count_str));
        char count_line[32];
        snprintf(count_line, sizeof(count_line), "%9s words", count_str);
        const int count_col = 18;
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, screen_row, count_col, count_line);
      }
    }
    display_row++;
  }

  // Scroll indicators.
  theme_apply_fg(plane, theme->dim_fg);
  if (scroll_offset > 0) {
    ncplane_putstr_yx(plane, list_top - 1, 4, "  ↑ more above");
  }
  if (last_visible < list->total_display_rows) {
    ncplane_putstr_yx(plane, list_top + visible_rows, 4, "  ↓ more below");
  }

  char count_line[80];
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

// Adjust scroll_offset so the entry's display row is visible. When the
// entry is the first of its language group, also try to keep its header
// row in view.
static int clamp_scroll(const LexiconList *list, int focus, int scroll_offset,
                        int visible_rows) {
  const int focus_row = list->entry_display_row[focus];
  // If this entry is the first of its language, the header sits one row
  // above. Aim to keep that header visible too.
  int target_top = focus_row;
  const bool first_of_lang =
      focus == 0 || list->entries[focus].lang != list->entries[focus - 1].lang;
  if (first_of_lang && focus_row > 0) {
    target_top = focus_row - 1;
  }

  if (target_top < scroll_offset) {
    scroll_offset = target_top;
  }
  if (focus_row >= scroll_offset + visible_rows) {
    scroll_offset = focus_row - visible_rows + 1;
  }
  if (scroll_offset < 0) {
    scroll_offset = 0;
  }
  const int max_scroll = list->total_display_rows - visible_rows;
  if (scroll_offset > max_scroll) {
    scroll_offset = max_scroll;
  }
  if (scroll_offset < 0) {
    scroll_offset = 0;
  }
  return scroll_offset;
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
      // Defensive drain; blocking get should not return zero in practice.
    }
    return false;
  }

  int focus = 0;
  if (initial != NULL && initial[0] != '\0') {
    for (int item_idx = 0; item_idx < list.count; item_idx++) {
      if (strcmp(list.entries[item_idx].name, initial) == 0) {
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
  if (visible_rows > list.total_display_rows) {
    visible_rows = list.total_display_rows;
  }
  int scroll_offset = 0;

  while (true) {
    scroll_offset = clamp_scroll(&list, focus, scroll_offset, visible_rows);
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
    if (key == NCKEY_RESIZE) {
      unsigned new_rows = 0;
      unsigned new_cols = 0;
      notcurses_refresh(nc, &new_rows, &new_cols);
      ncplane_resize_simple(std_plane, new_rows, new_cols);
      visible_rows = (int)new_rows - 8;
      if (visible_rows < 1) {
        visible_rows = 1;
      }
      if (visible_rows > list.total_display_rows) {
        visible_rows = list.total_display_rows;
      }
      continue;
    }

    if (key == NCKEY_UP || key == 'k' || key == 'K') {
      focus = (focus + list.count - 1) % list.count;
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      focus = (focus + 1) % list.count;
    } else if (key == NCKEY_PGUP) {
      // Move up by visible_rows entries (approx; we navigate by entries
      // rather than display rows).
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
      const char *chosen = list.entries[focus].name;
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

#include "lexicon_picker.h"

#include "theme.h"
#include "tui_resize.h"
#include <ctype.h>
#include <dirent.h>
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// Within a language, lexica list current editions first, then legacy
// ones, then variants (phony-trained lists like CSW24PH1400), so the
// ones people play come first and the rest sit in an overflow section.
typedef enum {
  LEX_TIER_CURRENT,
  LEX_TIER_LEGACY,
  LEX_TIER_VARIANT,
  LEX_TIER_COUNT,
} LexTier;

// English lexicon families, in listing order; other languages' lexica
// are all LEX_FAMILY_OTHER.
typedef enum {
  LEX_FAMILY_CSW,
  LEX_FAMILY_NORTH_AMERICAN, // NWL, TWL, OSPD
  LEX_FAMILY_OSW,            // OSW, OSWI: Collins's predecessors
  LEX_FAMILY_OTHER,
} LexFamily;

typedef struct {
  char name[LEXICON_NAME_MAX];
  LexLang lang;
  LexTier tier;
  LexFamily family;
  int year;       // edition year (2024 for CSW24); 0 when the name has none
  int word_count; // -1 if the .txt sibling could not be read
  bool has_wmp;   // sibling .wmp file present
  bool has_rit;   // sibling .rit file present
} LexiconEntry;

static int count_lines_in_file(const char *path) {
  FILE *file = fopen(path, "re");
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
  (void)fclose(file);
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

static LexFamily lexicon_family(const char *name) {
  if (has_iprefix(name, "CSW")) {
    return LEX_FAMILY_CSW;
  }
  if (has_iprefix(name, "NWL") || has_iprefix(name, "TWL") ||
      has_iprefix(name, "OSPD")) {
    return LEX_FAMILY_NORTH_AMERICAN;
  }
  if (has_iprefix(name, "OSW")) {
    return LEX_FAMILY_OSW;
  }
  return LEX_FAMILY_OTHER;
}

// The edition year in a lexicon name: 2024 for CSW24, 1998 for TWL98,
// 4 for OSW4; 0 when the name has no digits after its letters.
static int lexicon_year(const char *name) {
  const char *ch = name;
  while (*ch != '\0' && !isdigit((unsigned char)*ch)) {
    ch++;
  }
  int digits = 0;
  int value = 0;
  while (isdigit((unsigned char)*ch)) {
    value = value * 10 + (*ch - '0');
    digits++;
    ch++;
  }
  enum { CENTURY_PIVOT = 50 };
  if (digits == 2) {
    value += value < CENTURY_PIVOT ? 2000 : 1900;
  }
  return value;
}

// The installed lexicon `name` is a variant of (the one whose name is
// the longest proper prefix of it: CSW24 for CSW24PH1400, OSWI for
// OSWIPH1000), or NULL when it isn't a variant.
static const LexiconEntry *variant_base(const LexiconEntry *entries, int count,
                                        const char *name) {
  const LexiconEntry *base = NULL;
  const size_t name_len = strlen(name);
  for (int idx = 0; idx < count; idx++) {
    const size_t base_len = strlen(entries[idx].name);
    if (base_len < name_len &&
        strncmp(entries[idx].name, name, base_len) == 0 &&
        (base == NULL || base_len > strlen(base->name))) {
      base = &entries[idx];
    }
  }
  return base;
}

// Sets each entry's family, edition year, and tier: variants of another
// installed lexicon are variants (listed under their base's edition);
// the newest edition of each family is current, except for English
// families no longer played (OSW); other editions are legacy.
static void rank_lexica(LexiconEntry *entries, int count) {
  for (int idx = 0; idx < count; idx++) {
    LexiconEntry *entry = &entries[idx];
    entry->family = lexicon_family(entry->name);
    const LexiconEntry *base = variant_base(entries, count, entry->name);
    entry->year = lexicon_year(base != NULL ? base->name : entry->name);
    entry->tier = base != NULL ? LEX_TIER_VARIANT : LEX_TIER_LEGACY;
  }
  for (int idx = 0; idx < count; idx++) {
    LexiconEntry *entry = &entries[idx];
    if (entry->tier == LEX_TIER_VARIANT || entry->family == LEX_FAMILY_OSW) {
      continue;
    }
    bool newest = true;
    for (int other = 0; other < count && newest; other++) {
      const LexiconEntry *rival = &entries[other];
      newest =
          !(rival->tier != LEX_TIER_VARIANT && rival->lang == entry->lang &&
            rival->family == entry->family && rival->year > entry->year);
    }
    if (newest) {
      entry->tier = LEX_TIER_CURRENT;
    }
  }
}

// The header over a run of a language's lexica in one tier.
static void group_label(const LexiconEntry *entry, char *out, size_t out_size) {
  static const char *const tier_suffix[LEX_TIER_COUNT] = {
      [LEX_TIER_CURRENT] = "",
      [LEX_TIER_LEGACY] = " \xc2\xb7 legacy",
      [LEX_TIER_VARIANT] = " \xc2\xb7 variants",
  };
  (void)snprintf(out, out_size, "%s%s", lang_label(entry->lang),
                 tier_suffix[entry->tier]);
}

static bool same_group(const LexiconEntry *lhs, const LexiconEntry *rhs) {
  return lhs->lang == rhs->lang && lhs->tier == rhs->tier;
}

struct LexiconList {
  LexiconEntry entries[LEXICON_LIST_MAX];
  int count;
  const char *source_dir;
  // For each entry, the display row it occupies once language headers are
  // inserted before each group.
  int entry_display_row[LEXICON_LIST_MAX];
  int total_display_rows;
};

static int compare_entries(const void *lhs, const void *rhs) {
  const LexiconEntry *left = (const LexiconEntry *)lhs;
  const LexiconEntry *right = (const LexiconEntry *)rhs;
  if (left->lang != right->lang) {
    return (int)left->lang - (int)right->lang;
  }
  // Within a language: current, legacy, then variants; within those, by
  // family, then newest edition first.
  if (left->tier != right->tier) {
    return (int)left->tier - (int)right->tier;
  }
  if (left->family != right->family) {
    return (int)left->family - (int)right->family;
  }
  if (left->year != right->year) {
    return left->year > right->year ? -1 : 1;
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
  const struct dirent *entry;
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
    // Stat the optional .wmp / .rit siblings. fopen-and-close avoids
    // pulling in <sys/stat.h>; both files are small headers so the
    // cost is negligible and lexica/ is small anyway.
    char side_path[512];
    (void)snprintf(side_path, sizeof(side_path), "%s/%s.wmp", dir_path,
                   out->name);
    FILE *f = fopen(side_path, "rbe");
    out->has_wmp = (f != NULL);
    if (f != NULL) {
      (void)fclose(f);
    }
    (void)snprintf(side_path, sizeof(side_path), "%s/%s.rit", dir_path,
                   out->name);
    f = fopen(side_path, "rbe");
    out->has_rit = (f != NULL);
    if (f != NULL) {
      (void)fclose(f);
    }
    list->count++;
  }
  closedir(dir);
  if (list->count == 0) {
    return false;
  }

  rank_lexica(list->entries, list->count);
  qsort(list->entries, (size_t)list->count, sizeof(list->entries[0]),
        compare_entries);
  // Compute display rows: a header precedes each language's tier group.
  int display_row = 0;
  for (int idx = 0; idx < list->count; idx++) {
    if (idx == 0 || !same_group(&list->entries[idx - 1], &list->entries[idx])) {
      // Reserve a row for the group header.
      display_row++;
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
                          const LexiconList *list, int focus, int scroll_offset,
                          int visible_rows) {
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
  int display_row = 0;
  for (int idx = 0; idx < list->count; idx++) {
    if (idx == 0 || !same_group(&list->entries[idx - 1], &list->entries[idx])) {
      if (display_row >= scroll_offset && display_row < last_visible) {
        const int screen_row = list_top + (display_row - scroll_offset);
        char label[48];
        group_label(&list->entries[idx], label, sizeof(label));
        theme_apply_fg(plane, theme->status_fg);
        ncplane_putstr_yx(plane, screen_row, 2, "── ");
        ncplane_putstr(plane, label);
        ncplane_putstr(plane, " ──");
      }
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
        (void)snprintf(count_line, sizeof(count_line), "%9s words", count_str);
        const int count_col = 18;
        theme_apply_fg(plane, theme->dim_fg);
        ncplane_putstr_yx(plane, screen_row, count_col, count_line);
      }
      // WMP / RIT availability flags. The label always renders;
      // accent_fg when present (lexicon ships the data table) and
      // dim_fg when missing. Two narrow columns right of the word
      // count.
      const int wmp_col = 36;
      const int rit_col = 42;
      theme_apply_fg(plane, list->entries[idx].has_wmp ? theme->accent_fg
                                                       : theme->dim_fg);
      ncplane_putstr_yx(plane, screen_row, wmp_col, "wmp");
      theme_apply_fg(plane, list->entries[idx].has_rit ? theme->accent_fg
                                                       : theme->dim_fg);
      ncplane_putstr_yx(plane, screen_row, rit_col, "rit");
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
  if (snprintf(count_line, sizeof(count_line), "%d lexica from %s", list->count,
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
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4, "Press any key to dismiss.");
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
      focus == 0 ||
      !same_group(&list->entries[focus - 1], &list->entries[focus]);
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
    render_picker(std_plane, theme, &list, focus, scroll_offset, visible_rows);
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

// ── Modal-style picker (in-session) ──────────────────────────────────────
//
// Heap-allocated list driven by main.c's input loop. The render
// function paints into the shared modal plane and only expands the
// language containing the focused entry — all other languages
// collapse to their header row so the modal stays compact.

LexiconList *tui_lexicon_list_load(void) {
  LexiconList *list = (LexiconList *)calloc(1, sizeof(LexiconList));
  if (list == NULL) {
    return NULL;
  }
  if (!load_lexicon_list(list)) {
    free(list);
    return NULL;
  }
  return list;
}

void tui_lexicon_list_destroy(LexiconList *list) { free(list); }

int tui_lexicon_list_count(const LexiconList *list) {
  return list == NULL ? 0 : list->count;
}

int tui_lexicon_list_find(const LexiconList *list, const char *name) {
  if (list == NULL || name == NULL) {
    return -1;
  }
  for (int idx = 0; idx < list->count; idx++) {
    if (strcmp(list->entries[idx].name, name) == 0) {
      return idx;
    }
  }
  return -1;
}

bool tui_lexicon_list_name(const LexiconList *list, int idx, char *out_buf,
                           size_t out_buf_size) {
  if (list == NULL || idx < 0 || idx >= list->count || out_buf == NULL ||
      out_buf_size == 0) {
    return false;
  }
  (void)snprintf(out_buf, out_buf_size, "%s", list->entries[idx].name);
  return true;
}

bool tui_lexicon_list_language_name(const LexiconList *list, int idx,
                                    char *out_buf, size_t out_buf_size) {
  if (list == NULL || idx < 0 || idx >= list->count || out_buf == NULL ||
      out_buf_size == 0) {
    return false;
  }
  (void)snprintf(out_buf, out_buf_size, "%s",
                 lang_label(list->entries[idx].lang));
  return true;
}

int tui_lexicon_list_step_same_language(const LexiconList *list, int idx,
                                        int dir) {
  if (list == NULL || idx < 0 || idx >= list->count || dir == 0) {
    return idx;
  }
  const LexLang current = list->entries[idx].lang;
  const int next = idx + (dir > 0 ? 1 : -1);
  if (next < 0 || next >= list->count) {
    return idx;
  }
  if (list->entries[next].lang != current) {
    return idx;
  }
  return next;
}

int tui_lexicon_list_step_language(const LexiconList *list, int idx, int dir) {
  if (list == NULL || idx < 0 || idx >= list->count || dir == 0) {
    return idx;
  }
  const LexLang current = list->entries[idx].lang;
  // Entries are sorted by language, so each language group is a
  // contiguous run. Walk past the current run, then land on the
  // first entry of the next/prev run.
  if (dir > 0) {
    int probe = idx + 1;
    while (probe < list->count && list->entries[probe].lang == current) {
      probe++;
    }
    return probe < list->count ? probe : idx;
  }
  int probe = idx - 1;
  if (probe < 0) {
    return idx;
  }
  const LexLang target = list->entries[probe].lang;
  // Walk back to the FIRST entry in the previous group so cycling
  // back lands on a stable anchor rather than the last of that
  // group.
  while (probe > 0 && list->entries[probe - 1].lang == target) {
    probe--;
  }
  return probe;
}

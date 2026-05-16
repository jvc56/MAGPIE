#include "time_picker.h"

#include "theme.h"
#include "tui_resize.h"
#include <notcurses/notcurses.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  int seconds;
  const char *label;
  const char *blurb;
} TimePreset;

static const TimePreset presets[] = {
    {60, "1 minute", "ultra"},
    {180, "3 minutes", "blitz"},
    {600, "10 minutes", "rapid"},
    {1500, "25 minutes", "classical"},
};

enum {
  PRESET_COUNT = sizeof(presets) / sizeof(presets[0]),
  DEFAULT_PRESET_INDEX = 2, // 10 minutes (rapid)
};

static void fill_row(struct ncplane *plane, int row, unsigned cols) {
  for (unsigned col = 0; col < cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
}

static void render_picker(struct ncplane *plane, const Theme *theme,
                          int focus) {
  tui_sync_plane_to_terminal(plane);
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  fill_row(plane, 0, plane_cols);
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — choose a time control ");

  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 2, 4,
                    "Total time per side. Each player's clock counts down "
                    "while it is their turn.");

  const int list_top = 5;
  for (int idx = 0; idx < PRESET_COUNT; idx++) {
    const int row = list_top + idx;
    const TimePreset *preset = &presets[idx];
    if (idx == focus) {
      theme_apply_fg(plane, theme->accent_fg);
      ncplane_putstr_yx(plane, row, 4, "▶ ");
      theme_apply_fg(plane, theme->fg);
      char line[64];
      if (snprintf(line, sizeof(line), "%-12s %s", preset->label,
                   preset->blurb) > 0) {
        ncplane_putstr(plane, line);
      }
    } else {
      theme_apply_fg(plane, theme->dim_fg);
      char line[64];
      if (snprintf(line, sizeof(line), "%-12s %s", preset->label,
                   preset->blurb) > 0) {
        ncplane_putstr_yx(plane, row, 6, line);
      }
    }
  }

  theme_apply_fg(plane, theme->dim_fg);
  char hint[96];
  snprintf(hint, sizeof(hint),
           "↑/↓ or j/k navigate   1–%d jump   Enter confirm   Esc cancel",
           PRESET_COUNT);
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4, hint);
}

int tui_time_picker_preset_count(void) { return PRESET_COUNT; }

int tui_time_picker_preset_seconds(int idx) {
  if (idx < 0 || idx >= PRESET_COUNT) {
    return -1;
  }
  return presets[idx].seconds;
}

const char *tui_time_picker_preset_label(int idx) {
  if (idx < 0 || idx >= PRESET_COUNT) {
    return "";
  }
  return presets[idx].label;
}

const char *tui_time_picker_preset_blurb(int idx) {
  if (idx < 0 || idx >= PRESET_COUNT) {
    return "";
  }
  return presets[idx].blurb;
}

int tui_time_picker_closest_index(int initial_seconds) {
  if (initial_seconds <= 0) {
    return DEFAULT_PRESET_INDEX;
  }
  int closest_idx = DEFAULT_PRESET_INDEX;
  int closest_diff = -1;
  for (int idx = 0; idx < PRESET_COUNT; idx++) {
    int diff = presets[idx].seconds - initial_seconds;
    if (diff < 0) {
      diff = -diff;
    }
    if (closest_diff < 0 || diff < closest_diff) {
      closest_diff = diff;
      closest_idx = idx;
    }
  }
  return closest_idx;
}

int tui_time_picker_run(struct notcurses *nc, const Theme *theme,
                        int initial_seconds) {
  if (nc == NULL || theme == NULL) {
    return -1;
  }
  int focus = tui_time_picker_closest_index(initial_seconds);

  struct ncplane *std_plane = notcurses_stdplane(nc);
  while (true) {
    render_picker(std_plane, theme, focus);
    notcurses_render(nc);

    ncinput input;
    const uint32_t key = notcurses_get(nc, NULL, &input);
    if (key == (uint32_t)-1) {
      return -1;
    }
    if (input.evtype == NCTYPE_RELEASE) {
      continue;
    }
    if (key == NCKEY_RESIZE) {
      unsigned new_rows = 0;
      unsigned new_cols = 0;
      notcurses_refresh(nc, &new_rows, &new_cols);
      ncplane_resize_simple(std_plane, new_rows, new_cols);
      continue;
    }

    if (key == NCKEY_UP || key == 'k' || key == 'K') {
      focus = (focus + PRESET_COUNT - 1) % PRESET_COUNT;
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      focus = (focus + 1) % PRESET_COUNT;
    } else if (key >= '1' && key <= '0' + PRESET_COUNT) {
      focus = (int)(key - '1');
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      return presets[focus].seconds;
    } else if (key == NCKEY_ESC || key == 'q' || key == 'Q') {
      return -1;
    }
  }
}

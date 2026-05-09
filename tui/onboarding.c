#include "onboarding.h"

#include <stddef.h>
#include <stdint.h>
#include <notcurses/notcurses.h>
#include "theme.h"

static void fill_row(struct ncplane *plane, int row, unsigned cols) {
  for (unsigned col = 0; col < cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
}

static void render_picker(struct ncplane *plane, ThemeName focus) {
  const Theme *theme = theme_get(focus);

  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  // Header bar across the top.
  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  fill_row(plane, 0, plane_cols);
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — choose a theme ");

  // Theme list.
  theme_apply_bg(plane, theme->bg);
  const int list_top = 3;
  for (int theme_idx = 0; theme_idx < THEME_COUNT; theme_idx++) {
    const Theme *option = theme_get((ThemeName)theme_idx);
    const int row = list_top + theme_idx;
    if ((ThemeName)theme_idx == focus) {
      theme_apply_fg(plane, theme->accent_fg);
      ncplane_putstr_yx(plane, row, 4, "▶ ");
      theme_apply_fg(plane, theme->fg);
      ncplane_putstr(plane, option->label);
    } else {
      theme_apply_fg(plane, theme->dim_fg);
      ncplane_putstr_yx(plane, row, 6, option->label);
    }
  }

  // Key hints.
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, list_top + THEME_COUNT + 2, 4,
                    "↑/↓ or j/k navigate   1–4 jump   Enter confirm   "
                    "Esc cancel");

  // Preview block — exercises the slots that the placeholder and the
  // phase-2 board renderer will use, so the user can see contrast and
  // tile colors before committing.
  const int preview_top = list_top + THEME_COUNT + 5;
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, preview_top, 4, "── preview ──");

  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, preview_top + 2, 4, "Body text  ·  ");
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "dim helper  ·  ");
  theme_apply_fg(plane, theme->accent_fg);
  ncplane_putstr(plane, "accent");

  theme_apply_fg(plane, theme->tile_fg);
  theme_apply_bg(plane, theme->tile_bg);
  ncplane_putstr_yx(plane, preview_top + 4, 4, " S  C  R  A  B ");

  theme_apply_fg(plane, theme->status_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, preview_top + 6, 4, "● ready");
}

ThemeName tui_onboarding_run(struct notcurses *nc, ThemeName initial) {
  if (nc == NULL) {
    return initial;
  }
  struct ncplane *std_plane = notcurses_stdplane(nc);
  ThemeName focus = initial;
  if ((int)focus < 0 || focus >= THEME_COUNT) {
    focus = THEME_DARK;
  }

  while (true) {
    render_picker(std_plane, focus);
    notcurses_render(nc);

    ncinput input;
    const uint32_t key = notcurses_get(nc, NULL, &input);
    if (key == (uint32_t)-1) {
      return initial;
    }
    if (input.evtype == NCTYPE_RELEASE) {
      continue;
    }

    if (key == NCKEY_UP || key == 'k' || key == 'K') {
      focus = (ThemeName)((focus + THEME_COUNT - 1) % THEME_COUNT);
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      focus = (ThemeName)((focus + 1) % THEME_COUNT);
    } else if (key >= '1' && key <= '0' + THEME_COUNT) {
      focus = (ThemeName)(key - '1');
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      return focus;
    } else if (key == NCKEY_ESC || key == 'q' || key == 'Q') {
      return initial;
    }
  }
}

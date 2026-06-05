#include "onboarding.h"

#include "config.h"
#include "game_render.h"
#include "preview_game.h"
#include "theme.h"
#include "tui_resize.h"
#include <notcurses/notcurses.h>
#include <stddef.h>
#include <stdint.h>

enum {
  // The preview board is BOARD_DIM cells = 15 rows tall, with each cell
  // 2 cols wide → 30 cols. Plus a 1-col gutter from the theme list and a
  // 1-col right margin → 32 cols of horizontal headroom.
  PREVIEW_BOARD_CELLS = 15,
  PREVIEW_BOARD_COLS = PREVIEW_BOARD_CELLS * 2,
  // Default to a 2-pixel grid when the terminal can render pixel
  // graphics; this matches the in-game default and gives the preview
  // the same look the user will see during play.
  PREVIEW_BORDER_THICKNESS = 2,
};

static void fill_row(struct ncplane *plane, int row, unsigned cols) {
  for (unsigned col = 0; col < cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
}

static void render_picker(struct ncplane *plane, ThemeName focus,
                          const TuiPreviewGame *preview) {
  const Theme *theme = theme_get(focus);

  tui_sync_plane_to_terminal(plane);
  theme_apply_base(plane, theme);
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

  // Preview region. The board is what the user is really evaluating —
  // it exercises board_bg, tile_fg/bg, blank_tile_fg, the premium-square
  // colors, and the pixel-grid borders all at once. A small palette
  // strip below it covers the chrome colors that don't appear on the
  // board.
  const int preview_label_row = list_top + THEME_COUNT + 5;
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, preview_label_row, 4, "── preview ──");

  const int board_top = list_top;
  const int board_left = (int)plane_cols >= 36 + PREVIEW_BOARD_COLS + 2
                             ? (int)plane_cols - PREVIEW_BOARD_COLS - 4
                             : 36;
  const bool have_room_for_board =
      preview != NULL && preview->game != NULL && preview->ld != NULL &&
      (int)plane_cols >= board_left + PREVIEW_BOARD_COLS + 1 &&
      (int)plane_rows >= board_top + PREVIEW_BOARD_CELLS + 1;
  if (have_room_for_board) {
    struct notcurses *nc = ncplane_notcurses(plane);
    const int thickness =
        (nc != NULL && notcurses_canpixel(nc)) ? PREVIEW_BORDER_THICKNESS : 0;
    tui_render_board_at(plane, board_top, board_left, theme, preview->game,
                        preview->ld, /*blank_uppercase=*/true,
                        TUI_PREMIUM_LABELS_UPPERCASE, thickness);
  }

  // Chrome palette strip under the label.
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, preview_label_row + 2, 4, "Body text  ·  ");
  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr(plane, "dim helper  ·  ");
  theme_apply_fg(plane, theme->accent_fg);
  ncplane_putstr(plane, "accent");

  theme_apply_fg(plane, theme->status_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, preview_label_row + 4, 4, "● ready");
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

  // Build the autoplay preview once. If no lexicon is available we still
  // run the picker — the board area silently drops out, the chrome
  // palette strip still demonstrates the theme.
  TuiPreviewGame preview = {0};
  const bool preview_ready = tui_preview_game_init(&preview);

  ThemeName chosen = initial;
  while (true) {
    render_picker(std_plane, focus, preview_ready ? &preview : NULL);
    notcurses_render(nc);

    ncinput input;
    const uint32_t key = notcurses_get(nc, NULL, &input);
    if (key == (uint32_t)-1) {
      chosen = initial;
      break;
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
      focus = (ThemeName)((focus + THEME_COUNT - 1) % THEME_COUNT);
    } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
      focus = (ThemeName)((focus + 1) % THEME_COUNT);
    } else if (key >= '1' && key <= '0' + THEME_COUNT) {
      focus = (ThemeName)(key - '1');
    } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
      chosen = focus;
      break;
    } else if (key == NCKEY_ESC || key == 'q' || key == 'Q') {
      chosen = initial;
      break;
    }
  }

  if (preview_ready) {
    tui_preview_game_destroy(&preview);
  }
  // The preview's pixel-grid child plane lives in game_render.c's
  // module-level registry; flush it now so its image doesn't linger under
  // the in-game UI (visible as ghost lines through the player pills on
  // pixel-graphics terminals).
  tui_game_render_reset_grids();
  return chosen;
}

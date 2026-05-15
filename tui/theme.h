#ifndef TUI_THEME_H
#define TUI_THEME_H

#include <notcurses/notcurses.h>
#include <stdint.h>

typedef enum {
  THEME_DARK,
  THEME_LIGHT,
  THEME_DIM,
  THEME_HIGH_CONTRAST,
  THEME_COUNT,
} ThemeName;

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} ThemeRgb;

typedef struct {
  ThemeName name;
  const char *id;
  const char *label;

  // Window chrome
  ThemeRgb bg;
  ThemeRgb fg;
  ThemeRgb dim_fg;
  ThemeRgb accent_fg;
  ThemeRgb header_fg;
  ThemeRgb header_bg;
  ThemeRgb status_fg;
  ThemeRgb error_fg;
  // Subtle background tint (a couple shades off `bg`) used for things
  // like the analysis-panel column-header strip. Pairs with dim_fg
  // for a low-contrast "this is a label row" feel without shouting.
  ThemeRgb subtle_bg;

  // Board (used by phase 2 renderer; defined now so the palette shape is
  // stable as we port string_builder_add_game).
  ThemeRgb board_bg;
  // Player tiles come in two palettes — P1 in green, P2 in amber —
  // so a finished board reads as two distinct sets of plays. Both
  // pairs are populated unconditionally; the renderer picks one per
  // tile based on who placed it (engine-tracked square owner; not
  // yet wired up — all callers currently use tile1).
  ThemeRgb tile1_fg;
  ThemeRgb tile1_bg;
  ThemeRgb tile2_fg;
  ThemeRgb tile2_bg;
  ThemeRgb blank_tile_fg; // fg for played-blank tiles, distinct from tileN_fg
  ThemeRgb rack_tile1_fg;
  ThemeRgb rack_tile1_bg;
  ThemeRgb rack_tile2_fg;
  ThemeRgb rack_tile2_bg;
  ThemeRgb on_turn_fg;    // P1 turn-marker arrow / chyron
  ThemeRgb on_turn_fg_p2; // P2 turn-marker arrow / chyron
  // Premium squares carry both a tinted background that fills the cell
  // and a foreground color used for the punctuation marker that lives in
  // it (＝ －  ＂ ＇ ＊). Themes that want flat premium squares can set
  // *_bg equal to board_bg.
  ThemeRgb premium_tws_bg;
  ThemeRgb premium_tws_fg;
  ThemeRgb premium_dws_bg;
  ThemeRgb premium_dws_fg;
  ThemeRgb premium_tls_bg;
  ThemeRgb premium_tls_fg;
  ThemeRgb premium_dls_bg;
  ThemeRgb premium_dls_fg;
  ThemeRgb premium_center_bg;
  ThemeRgb premium_center_fg;
} Theme;

const Theme *theme_get(ThemeName name);
const Theme *theme_get_by_id(const char *id);

ThemeName theme_auto_detect(const struct notcurses *nc);

void theme_apply_fg(struct ncplane *plane, ThemeRgb color);
void theme_apply_bg(struct ncplane *plane, ThemeRgb color);

// Sets the plane's base cell to the theme's body fg/bg so that
// ncplane_erase fills with the theme background instead of the terminal
// default. Call before drawing each frame.
void theme_apply_base(struct ncplane *plane, const Theme *theme);

#endif

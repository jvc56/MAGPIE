#ifndef TUI_THEME_H
#define TUI_THEME_H

#include <stdint.h>
#include <notcurses/notcurses.h>

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

  // Board (used by phase 2 renderer; defined now so the palette shape is
  // stable as we port string_builder_add_game).
  ThemeRgb board_bg;
  ThemeRgb tile_fg;
  ThemeRgb tile_bg;
  ThemeRgb blank_tile_fg;  // fg for played-blank tiles, distinct from tile_fg
  ThemeRgb rack_tile_fg;
  ThemeRgb rack_tile_bg;
  ThemeRgb on_turn_fg;
  ThemeRgb premium_tws_bg;
  ThemeRgb premium_dws_bg;
  ThemeRgb premium_tls_bg;
  ThemeRgb premium_dls_bg;
  ThemeRgb premium_center_bg;
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

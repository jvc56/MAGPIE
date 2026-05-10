#include "theme.h"

#include <stdint.h>
#include <string.h>
#include <notcurses/notcurses.h>

static const Theme themes[THEME_COUNT] = {
    [THEME_DARK] =
        {
            .name = THEME_DARK,
            .id = "dark",
            .label = "Dark",
            .bg = {8, 8, 12},
            .fg = {220, 220, 220},
            .dim_fg = {130, 130, 130},
            .accent_fg = {96, 220, 220},
            .header_fg = {255, 220, 80},
            .header_bg = {0, 24, 64},
            .status_fg = {120, 200, 120},
            .error_fg = {255, 80, 80},
            .board_bg = {18, 24, 18},
            .tile_fg = {24, 24, 24},
            .tile_bg = {220, 200, 160},
            .blank_tile_fg = {160, 70, 70},
            .rack_tile_fg = {24, 24, 24},
            .rack_tile_bg = {220, 200, 160},
            .on_turn_fg = {120, 220, 120},
            .premium_tws_bg = {180, 60, 60},
            .premium_dws_bg = {220, 130, 130},
            .premium_tls_bg = {60, 100, 200},
            .premium_dls_bg = {130, 180, 220},
            .premium_center_bg = {220, 130, 130},
        },
    [THEME_LIGHT] =
        {
            .name = THEME_LIGHT,
            .id = "light",
            .label = "Light",
            .bg = {245, 240, 230},
            .fg = {40, 40, 40},
            .dim_fg = {130, 130, 130},
            .accent_fg = {0, 130, 130},
            .header_fg = {24, 32, 96},
            .header_bg = {250, 240, 180},
            .status_fg = {40, 130, 40},
            .error_fg = {180, 40, 40},
            .board_bg = {240, 230, 200},
            .tile_fg = {24, 24, 24},
            .tile_bg = {240, 220, 180},
            .blank_tile_fg = {180, 60, 60},
            .rack_tile_fg = {24, 24, 24},
            .rack_tile_bg = {240, 220, 180},
            .on_turn_fg = {40, 130, 40},
            .premium_tws_bg = {220, 100, 100},
            .premium_dws_bg = {240, 180, 180},
            .premium_tls_bg = {100, 140, 220},
            .premium_dls_bg = {170, 210, 240},
            .premium_center_bg = {240, 180, 180},
        },
    [THEME_DIM] =
        {
            .name = THEME_DIM,
            .id = "dim",
            .label = "Dim",
            .bg = {0, 0, 0},
            .fg = {160, 160, 160},
            .dim_fg = {90, 90, 90},
            .accent_fg = {100, 160, 160},
            .header_fg = {180, 160, 80},
            .header_bg = {0, 12, 32},
            .status_fg = {100, 150, 100},
            .error_fg = {180, 80, 80},
            .board_bg = {16, 16, 18},
            .tile_fg = {180, 180, 180},
            .tile_bg = {60, 50, 40},
            .blank_tile_fg = {220, 130, 130},
            .rack_tile_fg = {180, 180, 180},
            .rack_tile_bg = {60, 50, 40},
            .on_turn_fg = {100, 150, 100},
            .premium_tws_bg = {110, 50, 50},
            .premium_dws_bg = {130, 80, 80},
            .premium_tls_bg = {50, 70, 130},
            .premium_dls_bg = {80, 100, 130},
            .premium_center_bg = {130, 80, 80},
        },
    [THEME_HIGH_CONTRAST] =
        {
            .name = THEME_HIGH_CONTRAST,
            .id = "high_contrast",
            .label = "High contrast",
            .bg = {0, 0, 0},
            .fg = {255, 255, 255},
            .dim_fg = {255, 255, 255},
            .accent_fg = {255, 255, 0},
            .header_fg = {0, 0, 0},
            .header_bg = {255, 255, 255},
            .status_fg = {0, 255, 0},
            .error_fg = {255, 0, 0},
            .board_bg = {0, 0, 0},
            .tile_fg = {0, 0, 0},
            .tile_bg = {255, 255, 255},
            .blank_tile_fg = {200, 0, 0},
            .rack_tile_fg = {0, 0, 0},
            .rack_tile_bg = {255, 255, 255},
            .on_turn_fg = {255, 255, 0},
            .premium_tws_bg = {255, 0, 0},
            .premium_dws_bg = {255, 100, 200},
            .premium_tls_bg = {0, 100, 255},
            .premium_dls_bg = {0, 200, 255},
            .premium_center_bg = {255, 100, 200},
        },
};

const Theme *theme_get(ThemeName name) {
  if (name < 0 || name >= THEME_COUNT) {
    return &themes[THEME_DARK];
  }
  return &themes[name];
}

const Theme *theme_get_by_id(const char *id) {
  if (id == NULL) {
    return NULL;
  }
  for (int theme_idx = 0; theme_idx < THEME_COUNT; theme_idx++) {
    if (strcmp(themes[theme_idx].id, id) == 0) {
      return &themes[theme_idx];
    }
  }
  return NULL;
}

ThemeName theme_auto_detect(const struct notcurses *nc) {
  uint32_t bg_color = 0;
  if (notcurses_default_background(nc, &bg_color) != 0) {
    return THEME_DARK;
  }
  const uint8_t red = (bg_color >> 16) & 0xff;
  const uint8_t green = (bg_color >> 8) & 0xff;
  const uint8_t blue = bg_color & 0xff;
  // Rec. 709 luma; threshold at ~50% relative luminance (128/255).
  const int luma = (2126 * red + 7152 * green + 722 * blue) / 10000;
  if (luma < 128) {
    return THEME_DARK;
  }
  return THEME_LIGHT;
}

void theme_apply_fg(struct ncplane *plane, ThemeRgb color) {
  ncplane_set_fg_rgb8(plane, color.r, color.g, color.b);
}

void theme_apply_bg(struct ncplane *plane, ThemeRgb color) {
  ncplane_set_bg_rgb8(plane, color.r, color.g, color.b);
}

void theme_apply_base(struct ncplane *plane, const Theme *theme) {
  uint64_t channels = 0;
  ncchannels_set_fg_rgb8(&channels, theme->fg.r, theme->fg.g, theme->fg.b);
  ncchannels_set_bg_rgb8(&channels, theme->bg.r, theme->bg.g, theme->bg.b);
  ncplane_set_base(plane, " ", 0, channels);
}

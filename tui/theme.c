#include "theme.h"

#include <notcurses/notcurses.h>
#include <stdint.h>
#include <string.h>

// macondo-inspired dark palette: green-on-black chrome, played tiles in
// green-on-dark-green, premium squares carrying both a tinted background
// and a saturated punctuation foreground (＝ TW, － DW, ＂ TL, ＇ DL, ＊
// center). The RGBs below are eyeballed from the macondo screenshot and
// are the values the user is iterating on — adjust here, no other code
// references them by literal.
static const Theme themes[THEME_COUNT] =
    {
        [THEME_DARK] =
            {
                .name = THEME_DARK,
                .id = "dark",
                .label = "Dark",
                .bg = {0, 0, 0},
                .fg = {220, 220, 220},
                .dim_fg = {110, 130, 120},
                .accent_fg = {120, 220, 140},
                .header_fg = {120, 220, 140},
                .header_bg = {0, 0, 0},
                .status_fg = {120, 220, 140},
                .error_fg = {220, 80, 80},
                .board_bg = {10, 14, 18},
                .tile_fg = {130, 230, 160},
                .tile_bg = {30, 50, 35},
                .blank_tile_fg = {170, 170, 170},
                .rack_tile_fg = {130, 230, 160},
                .rack_tile_bg = {30, 50, 35},
                .on_turn_fg = {120, 220, 140},
                .premium_tws_bg = {91, 35, 47},
                .premium_tws_fg = {204, 117, 133},
                .premium_dws_bg = {72, 36, 44},
                .premium_dws_fg = {180, 109, 112},
                .premium_tls_bg = {31, 68, 88},
                .premium_tls_fg = {88, 139, 168},
                .premium_dls_bg = {26, 51, 70},
                .premium_dls_fg = {79, 124, 160},
                .premium_center_bg = {72, 36, 44},
                .premium_center_fg = {180, 109, 112},
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
                .premium_tws_bg = {250, 215, 215},
                .premium_tws_fg = {180, 60, 60},
                .premium_dws_bg = {248, 225, 225},
                .premium_dws_fg = {200, 110, 110},
                .premium_tls_bg = {220, 230, 250},
                .premium_tls_fg = {60, 100, 200},
                .premium_dls_bg = {230, 240, 250},
                .premium_dls_fg = {100, 150, 220},
                .premium_center_bg = {248, 225, 225},
                .premium_center_fg = {200, 110, 110},
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
                .premium_tws_bg = {60, 28, 28},
                .premium_tws_fg = {180, 80, 80},
                .premium_dws_bg = {52, 32, 32},
                .premium_dws_fg = {170, 100, 100},
                .premium_tls_bg = {28, 36, 60},
                .premium_tls_fg = {100, 130, 200},
                .premium_dls_bg = {32, 42, 56},
                .premium_dls_fg = {120, 150, 200},
                .premium_center_bg = {52, 32, 32},
                .premium_center_fg = {170, 100, 100},
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
                .premium_tws_bg = {120, 0, 0},
                .premium_tws_fg = {255, 0, 0},
                .premium_dws_bg = {120, 40, 80},
                .premium_dws_fg = {255, 100, 200},
                .premium_tls_bg = {0, 40, 120},
                .premium_tls_fg = {0, 120, 255},
                .premium_dls_bg = {0, 80, 120},
                .premium_dls_fg = {0, 200, 255},
                .premium_center_bg = {120, 40, 80},
                .premium_center_fg = {255, 100, 200},
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

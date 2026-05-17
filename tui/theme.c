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
                .subtle_bg = {18, 22, 20},
                .board_bg = {10, 14, 18},
                .tile1_fg = {130, 230, 160},
                .tile1_bg = {30, 50, 35},
                .tile2_fg = {230, 194, 130},
                .tile2_bg = {50, 42, 30},
                .blank_tile_fg = {170, 170, 170},
                .rack_tile1_fg = {130, 230, 160},
                .rack_tile1_bg = {30, 50, 35},
                .rack_tile2_fg = {230, 194, 130},
                .rack_tile2_bg = {50, 42, 30},
                .on_turn_fg = {120, 220, 140},
                .on_turn_fg_p2 = {230, 194, 130},
                .history_p1_fg = {140, 220, 160},
                .history_p1_dim_fg = {80, 130, 95},
                .history_p2_fg = {220, 190, 130},
                .history_p2_dim_fg = {140, 115, 70},
                .panel_focus_border_bg = {30, 30, 30},
                .modal_bg = {38, 38, 38},
                .modal_fg = {232, 232, 232},
                .modal_shortcut_fg = {136, 136, 136},
                .modal_border_bg = {52, 52, 52},
                .modal_border_fg = {96, 96, 96},
                .modal_focus_bg = {68, 68, 68},
                .modal_focus_fg = {255, 255, 255},
                // Dark theme bg is pure black, so a literal "darker
                // shadow" is unrenderable. Use a barely-visible grey
                // that reads as a subtle depth indicator on the empty
                // bg; over colored game content the half-block still
                // composes as a half-cell shadow tint.
                .modal_shadow_fg = {24, 24, 24},
                .premium_tws_bg = {91, 35, 47},
                .premium_tws_fg = {204, 117, 133},
                .premium_dws_bg = {124, 63, 77},
                .premium_dws_fg = {216, 154, 160},
                .premium_tls_bg = {31, 68, 88},
                .premium_tls_fg = {88, 139, 168},
                .premium_dls_bg = {51, 94, 122},
                .premium_dls_fg = {125, 168, 200},
                .premium_center_bg = {124, 63, 77},
                .premium_center_fg = {216, 154, 160},
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
                .subtle_bg = {235, 230, 220},
                .board_bg = {240, 230, 200},
                .tile1_fg = {24, 24, 24},
                .tile1_bg = {240, 220, 180},
                .tile2_fg = {64, 36, 12},
                .tile2_bg = {238, 206, 150},
                .blank_tile_fg = {180, 60, 60},
                .rack_tile1_fg = {24, 24, 24},
                .rack_tile1_bg = {240, 220, 180},
                .rack_tile2_fg = {64, 36, 12},
                .rack_tile2_bg = {238, 206, 150},
                .on_turn_fg = {40, 130, 40},
                .on_turn_fg_p2 = {170, 110, 40},
                .history_p1_fg = {40, 110, 40},
                .history_p1_dim_fg = {110, 150, 110},
                .history_p2_fg = {150, 90, 30},
                .history_p2_dim_fg = {180, 150, 110},
                .panel_focus_border_bg = {230, 224, 210},
                .modal_bg = {252, 252, 252},
                .modal_fg = {28, 28, 28},
                .modal_shortcut_fg = {130, 130, 130},
                .modal_border_bg = {236, 236, 236},
                .modal_border_fg = {180, 180, 180},
                .modal_focus_bg = {220, 220, 220},
                .modal_focus_fg = {0, 0, 0},
                .modal_shadow_fg = {120, 120, 120},
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
                .subtle_bg = {22, 22, 24},
                .board_bg = {16, 16, 18},
                .tile1_fg = {180, 180, 180},
                .tile1_bg = {60, 50, 40},
                .tile2_fg = {200, 170, 110},
                .tile2_bg = {55, 45, 30},
                .blank_tile_fg = {220, 130, 130},
                .rack_tile1_fg = {180, 180, 180},
                .rack_tile1_bg = {60, 50, 40},
                .rack_tile2_fg = {200, 170, 110},
                .rack_tile2_bg = {55, 45, 30},
                .on_turn_fg = {100, 150, 100},
                .on_turn_fg_p2 = {190, 160, 100},
                .history_p1_fg = {140, 180, 140},
                .history_p1_dim_fg = {80, 105, 80},
                .history_p2_fg = {195, 165, 110},
                .history_p2_dim_fg = {130, 110, 75},
                .panel_focus_border_bg = {26, 26, 30},
                .modal_bg = {32, 32, 36},
                .modal_fg = {220, 220, 220},
                .modal_shortcut_fg = {130, 130, 130},
                .modal_border_bg = {46, 46, 50},
                .modal_border_fg = {92, 92, 96},
                .modal_focus_bg = {62, 62, 66},
                .modal_focus_fg = {255, 255, 255},
                .modal_shadow_fg = {24, 24, 26},
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
                .subtle_bg = {40, 40, 40},
                .board_bg = {0, 0, 0},
                .tile1_fg = {0, 0, 0},
                .tile1_bg = {255, 255, 255},
                .tile2_fg = {0, 0, 0},
                .tile2_bg = {255, 220, 80},
                .blank_tile_fg = {200, 0, 0},
                .rack_tile1_fg = {0, 0, 0},
                .rack_tile1_bg = {255, 255, 255},
                .rack_tile2_fg = {0, 0, 0},
                .rack_tile2_bg = {255, 220, 80},
                .on_turn_fg = {255, 255, 0},
                .on_turn_fg_p2 = {255, 220, 80},
                .history_p1_fg = {255, 255, 255},
                .history_p1_dim_fg = {180, 180, 180},
                .history_p2_fg = {255, 220, 80},
                .history_p2_dim_fg = {200, 180, 60},
                .panel_focus_border_bg = {40, 40, 40},
                .modal_bg = {0, 0, 0},
                .modal_fg = {255, 255, 255},
                .modal_shortcut_fg = {200, 200, 200},
                .modal_border_bg = {0, 0, 0},
                .modal_border_fg = {255, 255, 255},
                .modal_focus_bg = {255, 255, 255},
                .modal_focus_fg = {0, 0, 0},
                .modal_shadow_fg = {0, 0, 0},
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

#ifndef TUI_CONFIG_H
#define TUI_CONFIG_H

#include "theme.h"
#include <stdbool.h>
#include <stddef.h>

enum {
  TUI_CONFIG_PATH_MAX = 4096,
  TUI_LEXICON_NAME_MAX = 32,
};

// How the premium squares (TW/DW/TL/DL/center) are labeled on the board.
// `NONE` paints only the tinted background — the user evaluates premiums
// by color alone. Lowercase is identical to UPPERCASE in cell width and
// in color tokens; only the glyphs differ.
typedef enum {
  TUI_PREMIUM_LABELS_UPPERCASE = 0,
  TUI_PREMIUM_LABELS_LOWERCASE = 1,
  TUI_PREMIUM_LABELS_PUNCT = 2,
  TUI_PREMIUM_LABELS_NONE = 3,
  TUI_PREMIUM_LABELS_COUNT,
} TuiPremiumLabels;

// Tri-state for score subscripts on placed 2x board tiles. NONZERO
// skips the subscript when the tile's point value is 0 — the default
// because in standard letter distributions only blanks score 0 and a
// "0" subscript on a blank is noise. ALL renders the subscript even
// for zero-scored tiles (useful for variant LDs that give blanks a
// nonzero value, or for users who want every tile labeled).
typedef enum {
  TUI_SCORE_SUBSCRIPTS_OFF = 0,
  TUI_SCORE_SUBSCRIPTS_NONZERO = 1,
  TUI_SCORE_SUBSCRIPTS_ALL = 2,
  TUI_SCORE_SUBSCRIPTS_COUNT,
} TuiScoreSubscripts;

typedef struct {
  ThemeName theme;
  bool theme_set;
  char lexicon[TUI_LEXICON_NAME_MAX];
  bool lexicon_set;
  int time_per_side_seconds;
  bool time_per_side_set;
  // Pixel-grid border thickness. 0 = off; 1..6 supported.
  int border_thickness;
  bool border_thickness_set;
  // Render played blanks as uppercase letters in blank_tile_fg, instead
  // of the engine's lowercase rendering in tile_fg.
  bool blank_uppercase;
  bool blank_uppercase_set;
  TuiPremiumLabels premium_labels;
  bool premium_labels_set;
  // Board scale: 1 keeps the classic 2-col × 1-row tiles; 2 switches to
  // FreeType-rasterized 4-col × 2-row tiles via Kitty pixel graphics.
  // Only honored when notcurses_canpixel is true and a TTF was loaded.
  int board_scale;
  bool board_scale_set;
  // Antialiasing toggle for the 2x render path. No effect at 1x.
  bool antialias;
  bool antialias_set;
  // Score subscripts on placed 2x board tiles. No effect at 1x (no
  // room in a 2-col × 1-row cell). When non-OFF, the letter glyph
  // shrinks to 80% and pins to the top-left, freeing the bottom-right
  // for the tile's point value.
  TuiScoreSubscripts score_subscripts;
  bool score_subscripts_set;
  // Whether to load the per-lexicon RackInfoTable at startup. Costs
  // tens of MB; only worthwhile when inference / advanced rack-aware
  // analysis is in use. Defaults off; honored on the next New Game
  // start.
  bool load_rit;
  bool load_rit_set;
} TuiConfig;

// Resolves config path: $XDG_CONFIG_HOME/magpie/tui.toml or
// $HOME/.config/magpie/tui.toml. Returns false if neither is usable.
bool tui_config_resolve_path(char *buf, size_t buf_size);

// Loads config from disk into *config (zeroed first). Returns true if the
// file existed and was readable; sets config->theme_set if a recognized
// theme key was found. Returns false if the file is missing.
bool tui_config_load(TuiConfig *config);

// Persists config, creating parent directories as needed. Returns true on
// success.
bool tui_config_save(const TuiConfig *config);

#endif

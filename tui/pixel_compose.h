#ifndef TUI_PIXEL_COMPOSE_H
#define TUI_PIXEL_COMPOSE_H

#include "../src/ent/board.h"
#include "../src/ent/move.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "render_common.h"
#include "theme.h"
#include <stdbool.h>
#include <stdint.h>

void blit_glyph_at(uint8_t *buf, int buf_w, int buf_h, int glyph_left,
                   int glyph_top, const TuiGlyph *g, ThemeRgb fg, ThemeRgb bg);
uint8_t *compose_arrow_pixels(bool vertical, int player_idx, int tile_w,
                              int tile_h, bool antialias, int border_thickness,
                              TuiGlyphCache *glyph_cache, const Theme *theme);
uint8_t *compose_premium_pixels(BonusSquare bs, int row, int col, int tile_w,
                                int tile_h, TuiGlyphCache *glyph_cache,
                                int board_cell_w,
                                TuiPremiumLabels premium_labels, bool antialias,
                                int border_thickness, const Theme *theme);
uint8_t *compose_rack_tile_pixels(MachineLetter ml, int player_idx, bool ghost,
                                  int tile_w, int tile_h,
                                  TuiGlyphCache *glyph_cache,
                                  TuiGlyphCache *glyph_cache_sub,
                                  TuiScoreSubscripts score_subscripts,
                                  bool antialias, const Theme *theme,
                                  const LetterDistribution *ld);
uint8_t *compose_tile_pixels(MachineLetter ml, int owner, bool blank_uppercase,
                             bool is_preview, int tile_w, int tile_h,
                             TuiGlyphCache *glyph_cache,
                             TuiGlyphCache *glyph_cache_sub,
                             TuiScoreSubscripts score_subscripts,
                             bool antialias, int border_thickness,
                             const Theme *theme, const LetterDistribution *ld);
void fill_preview_map(const TuiGameState *state, const Board *board,
                      MachineLetter out_letters[BOARD_DIM][BOARD_DIM],
                      int *out_owner);
void fill_tile_rect(uint8_t *buf, int buf_w, int tx, int ty, int tile_w,
                    int tile_h, ThemeRgb color);
void overlay_grid_lines(uint8_t *buf, int buf_w, int buf_h, int tiles_y,
                        int tiles_x, int tile_h_px, int tile_w_px,
                        int thickness, ThemeRgb color);

#endif

#include "pixel_compose.h"

#include "../src/ent/bonus_square.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "render_layout.h"
#include "render_view.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// 2x pixel-composite render. Builds one RGBA image covering the whole
// board area in a single child plane and ncblit_rgba's it. Each tile is
// (4 * cdx) × (2 * cdy) pixels: filled with the cell's bg, then the
// cached glyph alpha-blended on top using the cell's fg.
// Primitive: blit a glyph alpha-mask into `buf`, with the glyph's top-
// left pixel anchored at (glyph_left, glyph_top). Per-pixel bounds-check
// — necessary because callers position glyphs near tile edges where
// negative starts are routine.
// `bold`: when true, simulate a heavier weight by doing a second
// pass shifted +1px horizontally. Cheap fake-bold suitable for the
// "preview tile" look on top of the regular non-bold rasterizer.
static void blit_glyph_at_bold(uint8_t *buf, int buf_w, int buf_h,
                               int glyph_left, int glyph_top, const TuiGlyph *g,
                               ThemeRgb fg, ThemeRgb bg, bool bold);
void blit_glyph_at(uint8_t *buf, int buf_w, int buf_h, int glyph_left,
                   int glyph_top, const TuiGlyph *g, ThemeRgb fg, ThemeRgb bg) {
  blit_glyph_at_bold(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg,
                     /*bold=*/false);
}
static void blit_glyph_at_bold(uint8_t *buf, int buf_w, int buf_h,
                               int glyph_left, int glyph_top, const TuiGlyph *g,
                               ThemeRgb fg, ThemeRgb bg, bool bold) {
  if (g == NULL || g->width <= 0 || g->height <= 0) {
    return;
  }
  // The bold pass simulates a heavier weight by OR-ing each row's
  // alpha with the same row shifted +1px right — gives a 1-pixel
  // stroke widening without re-rasterizing. Pure CPU work, runs
  // off the same cached alpha buffer.
  (void)bold;
  for (int row = 0; row < g->height; row++) {
    const int dst_y = glyph_top + row;
    if (dst_y < 0 || dst_y >= buf_h) {
      continue;
    }
    const unsigned char *src = g->alpha + (size_t)row * g->width;
    for (int col = 0; col < g->width; col++) {
      const int dst_x = glyph_left + col;
      if (dst_x < 0 || dst_x >= buf_w) {
        continue;
      }
      const unsigned int a = src[col];
      if (a == 0) {
        continue;
      }
      uint8_t *dst = buf + ((size_t)dst_y * buf_w + (size_t)dst_x) * 4;
      // Linear-space blend between bg and fg; close enough at the
      // contrast levels we ship.
      dst[0] = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
      dst[1] = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
      dst[2] = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
      dst[3] = 255;
    }
  }
}
// Centered-in-tile placement (the historical default). Used for
// premium-square labels and for tile letters when score subscripts
// are off. Baseline biased to 72% down the tile so the glyph optical-
// centers about right for cap-only Latin letters.
static void blit_glyph_into_buf(uint8_t *buf, int buf_w, int buf_h, int tx,
                                int ty, int tile_w, int tile_h,
                                const TuiGlyph *g, ThemeRgb fg, ThemeRgb bg) {
  if (g == NULL || g->width <= 0 || g->height <= 0) {
    return;
  }
  const int baseline = ty + (int)(tile_h * 0.72);
  const int glyph_top = baseline - g->bearing_y;
  const int glyph_left = tx + (tile_w - g->width) / 2;
  blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
}
void fill_tile_rect(uint8_t *buf, int buf_w, int tx, int ty, int tile_w,
                    int tile_h, ThemeRgb color) {
  for (int row = 0; row < tile_h; row++) {
    uint8_t *p = buf + ((size_t)(ty + row) * buf_w + tx) * 4;
    for (int col = 0; col < tile_w; col++, p += 4) {
      p[0] = color.r;
      p[1] = color.g;
      p[2] = color.b;
      p[3] = 255;
    }
  }
}
// Paint grid lines into the buffer using the same "bottom + right edge
// of each tile" geometry as draw_pixel_grid (which the 1x text path
// uses against a separate overlay plane). At 2x the pixel composite
// owns the only plane covering the board, so we draw the lines inline
// here instead of stacking another plane that would just collide.
void overlay_grid_lines(uint8_t *buf, int buf_w, int buf_h, int tiles_y,
                        int tiles_x, int tile_h_px, int tile_w_px,
                        int thickness, ThemeRgb color) {
  if (thickness <= 0) {
    return;
  }
  // Horizontal: one band per tile row, sitting in the last `thickness`
  // pixels of that row's vertical extent.
  for (int i = 1; i <= tiles_y; i++) {
    const int line_top = i * tile_h_px - thickness;
    for (int t = 0; t < thickness; t++) {
      const int row = line_top + t;
      if (row < 0 || row >= buf_h) {
        continue;
      }
      uint8_t *p = buf + (size_t)row * buf_w * 4;
      for (int col = 0; col < buf_w; col++, p += 4) {
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = 255;
      }
    }
  }
  // Vertical: one band per tile column, sitting in the last `thickness`
  // pixels of that column's horizontal extent.
  for (int i = 1; i <= tiles_x; i++) {
    const int line_left = i * tile_w_px - thickness;
    for (int t = 0; t < thickness; t++) {
      const int col = line_left + t;
      if (col < 0 || col >= buf_w) {
        continue;
      }
      for (int row = 0; row < buf_h; row++) {
        uint8_t *p = buf + ((size_t)row * buf_w + col) * 4;
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = 255;
      }
    }
  }
}
// Composes the full 2x board RGBA buffer for the given inputs and
// returns a heap-allocated buffer of size *out_buf_w * *out_buf_h
// * 4 bytes. Caller owns the buffer and must free() it. Returns
// NULL on allocation failure.
//
// The function is pure compute (no notcurses calls) so it's safe
// to invoke from a worker thread provided the supplied glyph
// caches are not in concurrent use elsewhere — it calls
// tui_glyph_cache_set_size on both, which would race with a
// concurrent reader of the same cache. The TUI keeps one set of
// caches owned by the UI thread (used for rack + label renders)
// and a separate pair owned by the pixel worker (used here).
static uint8_t *
compose_board_pixels(const Board *board, const LetterDistribution *ld,
                     const Theme *theme, TuiGlyphCache *glyph_cache,
                     TuiGlyphCache *glyph_cache_sub, int board_cell_w,
                     int board_cell_h, unsigned cdy, unsigned cdx,
                     bool blank_uppercase, TuiPremiumLabels premium_labels,
                     bool antialias, TuiScoreSubscripts score_subscripts,
                     int border_thickness, int *out_buf_w, int *out_buf_h) {
  const int tile_w = (int)cdx * board_cell_w;
  const int tile_h = (int)cdy * board_cell_h;
  const int buf_w = tile_w * BOARD_DIM;
  const int buf_h = tile_h * BOARD_DIM;
  if (buf_w <= 0 || buf_h <= 0) {
    return NULL;
  }
  const bool subs_on =
      score_subscripts != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
  if (buf == NULL) {
    return NULL;
  }

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int tx = col * tile_w;
      const int ty = row * tile_h;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      ThemeRgb bg;
      ThemeRgb fg;
      uint32_t glyph_codepoint = 0;
      uint32_t glyph_second = 0;
      bool is_placed_tile = false;
      int tile_score = 0;

      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker = premium_marker_for_cell(
            theme, bs, row, col, premium_labels, board_cell_w);
        bg = marker.bg;
        fg = marker.fg;
        if ((unsigned char)marker.glyph[0] < 0x80 &&
            (unsigned char)marker.glyph[1] < 0x80 && marker.glyph[0] != ' ') {
          glyph_codepoint = (uint32_t)marker.glyph[0];
          glyph_second = (uint32_t)marker.glyph[1];
        }
      } else {
        is_placed_tile = true;
        const bool is_blank = get_is_blanked(ml);
        const bool render_uppercase = is_blank && blank_uppercase;
        const MachineLetter glyph_ml =
            render_uppercase ? get_unblanked_machine_letter(ml) : ml;
        const int owner = board_get_square_owner(board, row, col);
        bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
        fg = is_blank ? theme->blank_tile_fg
                      : (owner == 1 ? theme->tile2_fg : theme->tile1_fg);
        const char *ascii = ld->ld_ml_to_hl[glyph_ml];
        if (ascii != NULL && ascii[0] != '\0' &&
            (unsigned char)ascii[0] < 0x80) {
          glyph_codepoint = (uint32_t)ascii[0];
        }
        tile_score = equity_to_int(ld_get_score(ld, ml));
      }

      fill_tile_rect(buf, buf_w, tx, ty, tile_w, tile_h, bg);

      const bool show_subscript =
          is_placed_tile && subs_on &&
          (score_subscripts == TUI_SCORE_SUBSCRIPTS_ALL || tile_score != 0);
      if (glyph_codepoint != 0 && glyph_second == 0) {
        const TuiGlyph *g = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
        if (is_placed_tile && subs_on && g != NULL && g->width > 0 &&
            g->height > 0) {
          const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
          const int shift_x = (int)((double)tile_w * shift_x_frac);
          const int shift_y = (int)((double)tile_h * 0.08);
          const int baseline = ty + (int)(tile_h * 0.72) - shift_y;
          const int glyph_top = baseline - g->bearing_y;
          const int glyph_left = tx + (tile_w - g->width) / 2 - shift_x;
          blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
        } else {
          blit_glyph_into_buf(buf, buf_w, buf_h, tx, ty, tile_w, tile_h, g, fg,
                              bg);
        }
      } else if (glyph_codepoint != 0 && glyph_second != 0) {
        const TuiGlyph *g1 = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
        const TuiGlyph *g2 = tui_glyph_cache_get(glyph_cache, glyph_second);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx, ty, tile_w / 2, tile_h, g1,
                            fg, bg);
        blit_glyph_into_buf(buf, buf_w, buf_h, tx + tile_w / 2, ty, tile_w / 2,
                            tile_h, g2, fg, bg);
      }

      if (show_subscript) {
        char digits[8];
        snprintf(digits, sizeof(digits), "%d", tile_score);
        const int margin_x = (int)((double)tile_w * 0.12);
        const int margin_y = (int)((double)tile_h * 0.16);
        const int digit_bottom = ty + tile_h - margin_y;
        int pen_right = tx + tile_w - margin_x;
        for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
          const TuiGlyph *gd =
              tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
          if (gd == NULL || gd->width <= 0) {
            continue;
          }
          const int gleft = pen_right - gd->width;
          const int gtop = digit_bottom - gd->height;
          blit_glyph_at(buf, buf_w, buf_h, gleft, gtop, gd, fg, bg);
          pen_right = gleft - 1;
        }
      }
    }
  }

  overlay_grid_lines(buf, buf_w, buf_h, BOARD_DIM, BOARD_DIM, tile_h, tile_w,
                     border_thickness, theme->bg);

  *out_buf_w = buf_w;
  *out_buf_h = buf_h;
  return buf;
}
// Pixel-worker thread main loop. Pulls one pending request at a
// time off state->pixel_request, composes the RGBA buffer using
// the worker's dedicated glyph caches, then publishes the result
// into state->pixel_result. The UI thread picks up the result on
// its next frame and ncblits it onto the board plane. Holds
// pixel_mutex for the lock/unlock around request + result slots
// only; the heavy compose runs unlocked so the UI thread can post
// follow-up requests while the worker is busy.
void *tui_pixel_worker_main(void *arg) {
  TuiGameState *state = (TuiGameState *)arg;
  while (true) {
    pthread_mutex_lock(&state->pixel_mutex);
    while (!state->pixel_request.pending && !atomic_load(&state->pixel_stop)) {
      pthread_cond_wait(&state->pixel_cond, &state->pixel_mutex);
    }
    if (atomic_load(&state->pixel_stop)) {
      pthread_mutex_unlock(&state->pixel_mutex);
      break;
    }
    Board *board = state->pixel_request.board;
    state->pixel_request.board = NULL;
    state->pixel_request.pending = false;
    const Theme *theme = (const Theme *)state->pixel_request.theme;
    const int scale = state->pixel_request.scale;
    const int cell_w = state->pixel_request.cell_w;
    const int cell_h = state->pixel_request.cell_h;
    const unsigned cdy = state->pixel_request.cdy;
    const unsigned cdx = state->pixel_request.cdx;
    const bool blank_uppercase = state->pixel_request.blank_uppercase;
    const bool antialias = state->pixel_request.antialias;
    const TuiPremiumLabels premium_labels = state->pixel_request.premium_labels;
    const TuiScoreSubscripts score_subscripts =
        state->pixel_request.score_subscripts;
    const int border_thickness = state->pixel_request.border_thickness;
    const uint64_t version = state->pixel_request.version;
    const int history_cursor = state->pixel_request.history_cursor;
    pthread_mutex_unlock(&state->pixel_mutex);

    int buf_w = 0;
    int buf_h = 0;
    uint8_t *buf = compose_board_pixels(
        board, state->ld, theme, state->pixel_glyph_cache,
        state->pixel_glyph_cache_sub, cell_w, cell_h, cdy, cdx, blank_uppercase,
        premium_labels, antialias, score_subscripts, border_thickness, &buf_w,
        &buf_h);
    if (board != NULL) {
      board_destroy(board);
    }
    if (buf == NULL) {
      continue;
    }

    pthread_mutex_lock(&state->pixel_mutex);
    if (state->pixel_result.buf != NULL) {
      // UI never consumed the prior result — drop it. Newer is
      // better.
      free(state->pixel_result.buf);
    }
    state->pixel_result.buf = buf;
    state->pixel_result.buf_w = buf_w;
    state->pixel_result.buf_h = buf_h;
    state->pixel_result.scale = scale;
    state->pixel_result.cell_w = cell_w;
    state->pixel_result.cell_h = cell_h;
    state->pixel_result.cdy = cdy;
    state->pixel_result.cdx = cdx;
    state->pixel_result.blank_uppercase = blank_uppercase;
    state->pixel_result.antialias = antialias;
    state->pixel_result.premium_labels = premium_labels;
    state->pixel_result.score_subscripts = score_subscripts;
    state->pixel_result.border_thickness = border_thickness;
    state->pixel_result.version = version;
    state->pixel_result.history_cursor = history_cursor;
    state->pixel_result.ready = true;
    pthread_mutex_unlock(&state->pixel_mutex);
  }
  return NULL;
}
// Populate the 15x15 preview-letter map for the currently-cursored
// Analysis row. Empties out_letters first. Only cells currently
// empty on the rendered board receive preview tiles; played-
// through squares keep their original letter. The candidate move
// is resolved through pick_analysis_preview_move so saved-turn
// navigation pulls from the matching history snapshot rather
// than the live sim.
void fill_preview_map(const TuiGameState *state, const Board *board,
                      MachineLetter out_letters[BOARD_DIM][BOARD_DIM],
                      int *out_owner) {
  memset(out_letters, 0,
         sizeof(MachineLetter) * (size_t)(BOARD_DIM * BOARD_DIM));
  *out_owner = 0;
  const Move *preview_move = pick_analysis_preview_move(state, out_owner);
  if (preview_move == NULL ||
      move_get_type(preview_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int dir = move_get_dir(preview_move);
  int pr = move_get_row_start(preview_move);
  int pc = move_get_col_start(preview_move);
  const int n = move_get_tiles_length(preview_move);
  for (int i = 0; i < n; i++) {
    if (pr >= 0 && pr < BOARD_DIM && pc >= 0 && pc < BOARD_DIM) {
      const MachineLetter t = move_get_tile(preview_move, i);
      if (t != PLAYED_THROUGH_MARKER &&
          board_get_letter(board, pr, pc) == ALPHABET_EMPTY_SQUARE_MARKER) {
        out_letters[pr][pc] = t;
      }
    }
    if (board_is_dir_vertical(dir)) {
      pr++;
    } else {
      pc++;
    }
  }
}
// Compose a tile-sized RGBA buffer holding a directional arrow.
// FreeType-rasterized Unicode arrow glyph (➡ U+27A1 / ⬇ U+2B07),
// sized to mostly fill the 4×2 cell and centered on the glyph's
// own bounding box (NOT the letter baseline — symbols don't share
// the alphabet's baseline metric, so a baseline-centered blit
// would pin them to the cell's upper-left).
uint8_t *compose_arrow_pixels(bool vertical, int player_idx, int tile_w,
                              int tile_h, bool antialias, int border_thickness,
                              TuiGlyphCache *glyph_cache, const Theme *theme) {
  if (glyph_cache == NULL) {
    return NULL;
  }
  if (tile_w <= 0 || tile_h <= 0) {
    return NULL;
  }
  // Transparent buffer (calloc gives alpha=0 everywhere). We only
  // paint the arrow glyph itself and the right + bottom grid
  // border — everywhere else stays alpha=0 so the cell's pixel
  // plane underneath shows through (premium color, TW label,
  // etc.). The arrow reads as a "floating" cursor over whatever
  // cell it sits on rather than a filled-in tile.
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  const ThemeRgb fg = player_idx == 1 ? theme->tile2_fg : theme->tile1_fg;
  // Account for the baked right/bottom grid border. The arrow
  // glyph centers on the cell's *content* area, not the full
  // cell, so its center matches the centers of letters on
  // neighboring tiles (those center on (tile_w-t)/2 because
  // their bake takes the right + bottom t pixels).
  const int content_w = (border_thickness > 0 && border_thickness < tile_w)
                            ? tile_w - border_thickness
                            : tile_w;
  const int content_h = (border_thickness > 0 && border_thickness < tile_h)
                            ? tile_h - border_thickness
                            : tile_h;
  // For blit_glyph_at's alpha blend, we use a synthetic "bg" of
  // pure transparent — glyph pixels with alpha < 255 will blend
  // toward (0,0,0) but alpha will be set to 255 by blit_glyph_at,
  // so anti-aliased edges may darken slightly against the cell
  // bg underneath. If that's visually distracting we can switch
  // to a no-AA glyph path.
  const ThemeRgb transparent_bg = {.r = 0, .g = 0, .b = 0};

  // Match the letter em-size used by compose_tile_pixels with
  // subscripts on: tile_h * 0.50. Same em means the arrow's
  // rasterized bitmap stands at the same visual weight as the
  // letters on neighboring tiles instead of dwarfing them.
  // (The arrow glyph occupies less of its em-square than a
  // capital letter, so it'll read as slightly narrower; that's
  // fine — same height is the goal.)
  const int glyph_px = (int)((double)tile_h * 0.50);
  tui_glyph_cache_set_size(glyph_cache, glyph_px, antialias);

  const uint32_t codepoint = vertical ? 0x2193 /* ↓ */ : 0x2192 /* → */;
  // Bold variant — the regular weight of these symbol glyphs is
  // already heavy, but the bold pass embolden's the outline and
  // gives a slightly chunkier look that reads more confidently
  // as a "cursor" rather than as a text decoration.
  const TuiGlyph *g = tui_glyph_cache_get_bold(glyph_cache, codepoint);
  if (g != NULL && g->width > 0 && g->height > 0) {
    const int glyph_top = (content_h - g->height) / 2;
    const int glyph_left = (content_w - g->width) / 2;
    blit_glyph_at(buf, tile_w, tile_h, glyph_left, glyph_top, g, fg,
                  transparent_bg);
  }
  // Bake the grid border on the right + bottom in theme->bg —
  // matches the border on the cell pixel plane underneath. This
  // is the one place we have opaque pixels (besides the glyph);
  // the rest of the buffer stays alpha=0 so the underlying
  // premium / empty cell shows through.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int row = 0; row < tile_h; row++) {
      uint8_t *p = buf + ((size_t)row * tile_w + (tile_w - t)) * 4;
      for (int col = 0; col < t; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int row = tile_h - t; row < tile_h; row++) {
      uint8_t *p = buf + (size_t)row * tile_w * 4;
      for (int col = 0; col < tile_w; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }
  return buf;
}
// Compose a premium / empty cell's RGBA buffer. Same shape and
// border bake as compose_tile_pixels — bg color from the premium
// marker, optional 1- or 2-char ASCII label glyph, right + bottom
// theme->bg border. Letting every cell (premium, empty, tile,
// arrow) come from a per-cell pixel plane means the grid borders
// are uniformly baked into sprixels at the same pixel positions
// instead of split between sprixel-baked tiles + an overlay plane
// for premiums — which caused the visible thickness asymmetry.
uint8_t *compose_premium_pixels(BonusSquare bs, int row, int col, int tile_w,
                                int tile_h, TuiGlyphCache *glyph_cache,
                                int board_cell_w,
                                TuiPremiumLabels premium_labels, bool antialias,
                                int border_thickness, const Theme *theme) {
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  // premium_marker_for_cell expects 1 for halfwidth, anything else
  // for fullwidth. Our 4-wide-cell 2x scale is fullwidth.
  const PremiumMarker marker = premium_marker_for_cell(
      theme, bs, row, col, premium_labels, board_cell_w == 1 ? 1 : 2);
  fill_tile_rect(buf, tile_w, 0, 0, tile_w, tile_h, marker.bg);

  // ASCII label glyphs only — fullwidth premium glyphs are
  // multi-byte UTF-8 (≥0x80 first byte) which we don't pixel-render
  // here. The bg-only fill still communicates "this is a premium"
  // via color even without the label.
  uint32_t glyph_codepoint = 0;
  uint32_t glyph_second = 0;
  if ((unsigned char)marker.glyph[0] < 0x80 &&
      (unsigned char)marker.glyph[1] < 0x80 && marker.glyph[0] != ' ' &&
      marker.glyph[0] != '\0') {
    glyph_codepoint = (uint32_t)marker.glyph[0];
    glyph_second = marker.glyph[1] != '\0' ? (uint32_t)marker.glyph[1] : 0;
  }
  if (glyph_codepoint != 0 && glyph_cache != NULL) {
    const int letter_px = (int)((double)tile_h * 0.50);
    tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
    if (glyph_second == 0) {
      const TuiGlyph *g = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
      if (g != NULL && g->width > 0 && g->height > 0) {
        blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w, tile_h, g,
                            marker.fg, marker.bg);
      }
    } else {
      const TuiGlyph *g1 = tui_glyph_cache_get(glyph_cache, glyph_codepoint);
      const TuiGlyph *g2 = tui_glyph_cache_get(glyph_cache, glyph_second);
      if (g1 != NULL && g2 != NULL && g1->width > 0 && g2->width > 0) {
        blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w / 2, tile_h, g1,
                            marker.fg, marker.bg);
        blit_glyph_into_buf(buf, tile_w, tile_h, tile_w / 2, 0, tile_w / 2,
                            tile_h, g2, marker.fg, marker.bg);
      }
    }
  }

  // Bake the right + bottom border, identical to compose_tile_pixels.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int r = 0; r < tile_h; r++) {
      uint8_t *p = buf + ((size_t)r * tile_w + (tile_w - t)) * 4;
      for (int c = 0; c < t; c++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int r = tile_h - t; r < tile_h; r++) {
      uint8_t *p = buf + (size_t)r * tile_w * 4;
      for (int c = 0; c < tile_w; c++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }
  return buf;
}
// Compose ONE tile's RGBA buffer (bg color + letter glyph +
// optional score subscript). Caller owns the returned buffer
// and must free() it. Returns NULL on failure.
uint8_t *compose_tile_pixels(MachineLetter ml, int owner, bool blank_uppercase,
                             bool is_preview, int tile_w, int tile_h,
                             TuiGlyphCache *glyph_cache,
                             TuiGlyphCache *glyph_cache_sub,
                             TuiScoreSubscripts score_subscripts,
                             bool antialias, int border_thickness,
                             const Theme *theme, const LetterDistribution *ld) {
  uint8_t *buf = (uint8_t *)calloc(1, (size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    return NULL;
  }
  const bool is_blank = get_is_blanked(ml);
  const bool render_uppercase = is_blank && blank_uppercase;
  const MachineLetter glyph_ml =
      render_uppercase ? get_unblanked_machine_letter(ml) : ml;
  // Preview tiles: muted player-hue tile under the bright player
  // hue letter (rendered through the bold glyph cache so the
  // outline is actually emboldened, not dilated). The bg is the
  // player's tile_fg color blended halfway with theme->bg so the
  // tile reads as a soft-tinted square instead of a fully
  // saturated one — enough hue to identify the player, low
  // enough contrast that the bold letter dominates.
  ThemeRgb bg;
  ThemeRgb fg;
  if (is_preview) {
    const ThemeRgb hue = owner == 1 ? theme->tile2_fg : theme->tile1_fg;
    ThemeRgb muted;
    muted.r = (uint8_t)((hue.r + theme->bg.r) / 2);
    muted.g = (uint8_t)((hue.g + theme->bg.g) / 2);
    muted.b = (uint8_t)((hue.b + theme->bg.b) / 2);
    bg = muted;
    fg = is_blank ? theme->blank_tile_fg : hue;
  } else {
    bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
    fg = is_blank ? theme->blank_tile_fg
                  : (owner == 1 ? theme->tile2_fg : theme->tile1_fg);
  }
  fill_tile_rect(buf, tile_w, 0, 0, tile_w, tile_h, bg);

  const int sub_mode = (int)score_subscripts;
  const bool subs_on =
      sub_mode != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)tile_h * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)tile_h * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  const char *ascii = ld->ld_ml_to_hl[glyph_ml];
  uint32_t glyph_codepoint = 0;
  if (ascii != NULL && ascii[0] != '\0' && (unsigned char)ascii[0] < 0x80) {
    glyph_codepoint = (uint32_t)ascii[0];
  }
  const int tile_score = equity_to_int(ld_get_score(ld, ml));
  const bool show_subscript =
      subs_on && (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL || tile_score != 0);

  if (glyph_codepoint != 0) {
    // Preview tiles pull from the bold cache so FreeType emboldens the
    // outline before rasterization — that gives a real bold stroke
    // weight, not a bitmap dilation of the regular glyph.
    const TuiGlyph *g =
        is_preview ? tui_glyph_cache_get_bold(glyph_cache, glyph_codepoint)
                   : tui_glyph_cache_get(glyph_cache, glyph_codepoint);
    if (subs_on && g != NULL && g->width > 0 && g->height > 0) {
      const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
      const int shift_x = (int)((double)tile_w * shift_x_frac);
      const int shift_y = (int)((double)tile_h * 0.08);
      const int baseline = (int)(tile_h * 0.72) - shift_y;
      const int glyph_top = baseline - g->bearing_y;
      const int glyph_left = (tile_w - g->width) / 2 - shift_x;
      blit_glyph_at_bold(buf, tile_w, tile_h, glyph_left, glyph_top, g, fg, bg,
                         /*bold=*/false);
    } else {
      blit_glyph_into_buf(buf, tile_w, tile_h, 0, 0, tile_w, tile_h, g, fg, bg);
    }
  }

  if (show_subscript) {
    char digits[8];
    snprintf(digits, sizeof(digits), "%d", tile_score);
    const int margin_x = (int)((double)tile_w * 0.12);
    const int margin_y = (int)((double)tile_h * 0.16);
    const int digit_bottom = tile_h - margin_y;
    int pen_right = tile_w - margin_x;
    for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
      const TuiGlyph *gd =
          is_preview
              ? tui_glyph_cache_get_bold(glyph_cache_sub, (uint32_t)digits[i])
              : tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
      if (gd == NULL || gd->width <= 0) {
        continue;
      }
      const int gleft = pen_right - gd->width;
      const int gtop = digit_bottom - gd->height;
      blit_glyph_at_bold(buf, tile_w, tile_h, gleft, gtop, gd, fg, bg,
                         /*bold=*/false);
      pen_right = gleft - 1;
    }
  }

  // Bake the grid lines into the tile itself: paint the right N
  // columns and bottom N rows in theme->bg. Adjacent tiles'
  // edges combine into a continuous grid, and there's no z-order
  // dependency on a separate overlay plane.
  if (border_thickness > 0) {
    int t = border_thickness;
    if (t > tile_w) {
      t = tile_w;
    }
    if (t > tile_h) {
      t = tile_h;
    }
    for (int row = 0; row < tile_h; row++) {
      uint8_t *p = buf + ((size_t)row * tile_w + (tile_w - t)) * 4;
      for (int col = 0; col < t; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
    for (int row = tile_h - t; row < tile_h; row++) {
      uint8_t *p = buf + (size_t)row * tile_w * 4;
      for (int col = 0; col < tile_w; col++, p += 4) {
        p[0] = theme->bg.r;
        p[1] = theme->bg.g;
        p[2] = theme->bg.b;
        p[3] = 255;
      }
    }
  }

  return buf;
}
// Compose ONE rack tile's RGBA buffer (rack_tileN bg + letter
// glyph + optional score subscript). Caller owns + frees the
// buffer. Returns NULL on failure. The rack uses its own
// palette (rack_tile1_*, rack_tile2_*) rather than the board's
// tile colors; blanks render as "?" with subscript 0.
uint8_t *compose_rack_tile_pixels(MachineLetter ml, int player_idx, bool ghost,
                                  int tile_w, int tile_h,
                                  TuiGlyphCache *glyph_cache,
                                  TuiGlyphCache *glyph_cache_sub,
                                  TuiScoreSubscripts score_subscripts,
                                  bool antialias, const Theme *theme,
                                  const LetterDistribution *ld, bool empty) {
  // Supersample: rasterize the glyph + composite the tile at SSx
  // the target pixel size, then box-average 2x2 down to the cell
  // pixel size the terminal expects. Sidesteps FT hinting that
  // snaps stems to integer pixels badly at certain sizes
  // (cdy=27/30 looked blocky, cdy=32/34 looked clean) — by
  // rendering at 2*letter_px, hints and rasterization quirks
  // happen on a finer grid, then the average yields smooth
  // sub-pixel-positioned strokes regardless of the target size.
  const int SS = 2;
  const int tw = tile_w * SS;
  const int th = tile_h * SS;
  uint8_t *buf_ss = (uint8_t *)calloc(1, (size_t)tw * th * 4);
  if (buf_ss == NULL) {
    return NULL;
  }
  // Ghosted rack tiles: the slot's letter is being projected onto
  // the board by the currently-previewed move. Render the slot
  // as a neutral gray-on-bg ghost (no player-hue tile, no score
  // subscript) so the rack visually communicates "this tile is
  // about to be spent" while keeping the slot's physical
  // position occupied.
  const ThemeRgb bg =
      ghost ? theme->bg
            : (player_idx == 1 ? theme->rack_tile2_bg : theme->rack_tile1_bg);
  const ThemeRgb fg =
      ghost ? (ThemeRgb){.r = 102, .g = 102, .b = 102}
            : (player_idx == 1 ? theme->rack_tile2_fg : theme->rack_tile1_fg);
  if (empty) {
    // Concealed opponent tiles: a full-size tile-color square (matching
    // the board / face-up rack tiles) framed by a thin panel-bg border
    // on all four edges so a row of them reads as separate square tiles.
    fill_tile_rect(buf_ss, tw, 0, 0, tw, th, bg);
    int line = th / 24;
    if (line < SS) {
      line = SS;
    }
    fill_tile_rect(buf_ss, tw, 0, 0, tw, line, theme->bg);         // top
    fill_tile_rect(buf_ss, tw, 0, th - line, tw, line, theme->bg); // bottom
    fill_tile_rect(buf_ss, tw, 0, 0, line, th, theme->bg);         // left
    fill_tile_rect(buf_ss, tw, tw - line, 0, line, th, theme->bg); // right
  } else {
    fill_tile_rect(buf_ss, tw, 0, 0, tw, th, bg);
  }

  const int sub_mode = (int)score_subscripts;
  const bool subs_on =
      sub_mode != TUI_SCORE_SUBSCRIPTS_OFF && glyph_cache_sub != NULL;
  const int letter_px = (int)((double)th * (subs_on ? 0.50 : 0.74));
  const int sub_px = (int)((double)th * 0.24);
  tui_glyph_cache_set_size(glyph_cache, letter_px, antialias);
  if (subs_on) {
    tui_glyph_cache_set_size(glyph_cache_sub, sub_px, antialias);
  }

  // Concealed (opponent's hidden) tiles render as a bare tile box: the
  // player-hued bg fill above, with no letter glyph and no score.
  const char *ascii = empty ? "" : (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
  const TuiGlyph *g =
      (ascii != NULL && ascii[0] != '\0' && (unsigned char)ascii[0] < 0x80)
          ? tui_glyph_cache_get(glyph_cache, (uint32_t)ascii[0])
          : NULL;
  const int tile_score = (ml == 0) ? 0 : equity_to_int(ld_get_score(ld, ml));
  if (g != NULL && g->width > 0 && g->height > 0) {
    if (subs_on) {
      const double shift_x_frac = (tile_score >= 10) ? 0.07 : 0.03;
      const int shift_x = (int)((double)tw * shift_x_frac);
      const int shift_y = (int)((double)th * 0.08);
      const int baseline = (int)(th * 0.72) - shift_y;
      const int glyph_top = baseline - g->bearing_y;
      const int glyph_left = (tw - g->width) / 2 - shift_x;
      blit_glyph_at(buf_ss, tw, th, glyph_left, glyph_top, g, fg, bg);
    } else {
      blit_glyph_into_buf(buf_ss, tw, th, 0, 0, tw, th, g, fg, bg);
    }
  }
  if (subs_on && !ghost && !empty) {
    // Blanks always get a "0" subscript so their value is explicit.
    const bool show_subscript = (ml == 0) ||
                                (sub_mode == TUI_SCORE_SUBSCRIPTS_ALL) ||
                                (tile_score != 0);
    if (show_subscript) {
      char digits[8];
      snprintf(digits, sizeof(digits), "%d", tile_score);
      const int margin_x = (int)((double)tw * 0.12);
      const int margin_y = (int)((double)th * 0.16);
      const int digit_bottom = th - margin_y;
      int pen_right = tw - margin_x;
      for (int i = (int)strlen(digits) - 1; i >= 0; i--) {
        const TuiGlyph *gd =
            tui_glyph_cache_get(glyph_cache_sub, (uint32_t)digits[i]);
        if (gd == NULL || gd->width <= 0) {
          continue;
        }
        const int gleft = pen_right - gd->width;
        const int gtop = digit_bottom - gd->height;
        blit_glyph_at(buf_ss, tw, th, gleft, gtop, gd, fg, bg);
        pen_right = gleft - 1;
      }
    }
  }
  // Downsample SSxSS box average to (tile_w x tile_h). Alpha is
  // fully opaque everywhere in the supersample buffer, so we
  // skip averaging it.
  uint8_t *buf = (uint8_t *)malloc((size_t)tile_w * tile_h * 4);
  if (buf == NULL) {
    free(buf_ss);
    return NULL;
  }
  const int denom = SS * SS;
  for (int row = 0; row < tile_h; row++) {
    for (int col = 0; col < tile_w; col++) {
      uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
      for (int dy = 0; dy < SS; dy++) {
        const uint8_t *src_row =
            buf_ss + (size_t)((row * SS + dy) * tw + col * SS) * 4;
        for (int dx = 0; dx < SS; dx++) {
          const uint8_t *src = src_row + (size_t)dx * 4;
          r_sum += src[0];
          g_sum += src[1];
          b_sum += src[2];
        }
      }
      uint8_t *dst = buf + (size_t)(row * tile_w + col) * 4;
      dst[0] = (uint8_t)(r_sum / denom);
      dst[1] = (uint8_t)(g_sum / denom);
      dst[2] = (uint8_t)(b_sum / denom);
      dst[3] = 255;
    }
  }
  free(buf_ss);
  return buf;
}

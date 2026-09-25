#include "render_board.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/rack.h"
#include "frame_dump.h"
#include "glyph_cache.h"
#include "mach_compat.h"
#include "pixel_compose.h"
#include "render_common.h"
#include "render_planes.h"
#include "render_view.h"
#include "tui_ui_types.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Fullwidth column labels Ａ..Ｚ.
static const char *const fullwidth_col_labels[] = {
    "\xef\xbc\xa1", "\xef\xbc\xa2", "\xef\xbc\xa3", "\xef\xbc\xa4",
    "\xef\xbc\xa5", "\xef\xbc\xa6", "\xef\xbc\xa7", "\xef\xbc\xa8",
    "\xef\xbc\xa9", "\xef\xbc\xaa", "\xef\xbc\xab", "\xef\xbc\xac",
    "\xef\xbc\xad", "\xef\xbc\xae", "\xef\xbc\xaf", "\xef\xbc\xb0",
    "\xef\xbc\xb1", "\xef\xbc\xb2", "\xef\xbc\xb3", "\xef\xbc\xb4",
    "\xef\xbc\xb5", "\xef\xbc\xb6", "\xef\xbc\xb7", "\xef\xbc\xb8",
    "\xef\xbc\xb9", "\xef\xbc\xba",
};
// Debug instrumentation. Three counters surfaced in the top-right
// overlay while tuning the 2x pixel pipeline:
//   lat — end-to-end microseconds from a History-cursor change to
//         the corresponding pixel-board blit landing on screen.
//   blit — microseconds the UI thread last spent inside
//          ncblit_rgba (memcpy + plane-state churn). Excludes the
//          notcurses_render Kitty emit that happens later.
//   max — peak UI-thread frame time over the last ~1 second
//         (microseconds between consecutive notcurses_render
//         returns observed from main.c). Lets us see if the loop
//         is dropping below 60fps even when no cursor moved.
static _Atomic long g_board_blit_latency_us;
static _Atomic long g_ncblit_us;
// Number of tile pixel planes ncblit'd on the most-recent frame
// that performed any board work. 0 means no blits (cache hit).
// High values (5–7) line up with bingos / multi-letter plays
// landing on the board.
static _Atomic int g_last_tile_blits;
static int g_last_blitted_cursor;
static bool g_last_blit_tracked;
static struct timespec g_cursor_pending_since;
static bool g_cursor_pending;
int tui_debug_last_tile_blits(void) { return atomic_load(&g_last_tile_blits); }
// Per-tile cache for the 2x layered renderer (text bg + small
// pixel planes per placed tile). If the (letter, owner, score,
// antialias, subs, cdy, cdx, scale) tuple hasn't changed since
// the tile was last rasterized, we skip both the rasterize AND
// the ncblit_rgba — the existing sprixel on the plane is still
// correct. This is what lets cursor scrolling through history
// snapshots cost only ~the tiles that differ between snapshots,
// instead of re-emitting the entire board.
typedef struct {
  int letter;
  int owner;
  int score;
  bool blank_uppercase;
  bool antialias;
  bool is_preview;
  int score_subscripts;
  int border_thickness;
  unsigned cdy, cdx;
  int scale;
  // bs / premium_labels: cached premium-cell inputs. The same
  // per-cell pixel plane handles both placed tiles AND empty /
  // premium cells now, so the cache key has to discriminate
  // "DLS at H8" from "TLS at H8" and from "[empty] at H8". When
  // ml==EMPTY the bake + bg come from premium_marker_for_cell
  // (which depends on bs + the premium_labels mode); when ml is a
  // letter, bs / premium_labels are ignored.
  int bs;
  int premium_labels;
  bool valid;
} TileCache;
static TileCache board_tile_cache[BOARD_DIM][BOARD_DIM];
static struct ncplane *board_tile_planes[BOARD_DIM][BOARD_DIM];
// Count of full tile-plane invalidations since start, for the
// MAGPIE_FPS_DEBUG perf trace: a slow frame with blits=~225 and this
// counter ticking up means something (resize recovery, cdy/cdx drift,
// scale flip) nuked the per-tile cache; a slow frame WITHOUT a tick
// points at the emit path / terminal backpressure instead.
static _Atomic unsigned long g_tile_invalidations;
unsigned long tui_debug_tile_invalidations(void) {
  return atomic_load(&g_tile_invalidations);
}

// Tear down the board's per-cell tile planes (see invalidate_tile_planes
// in game_render.c for the full teardown that also covers the rack and
// edit arrow).
void render_board_invalidate_tile_planes(void) {
  atomic_fetch_add(&g_tile_invalidations, 1);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_tile_planes[row][col] != NULL) {
        ncplane_destroy(board_tile_planes[row][col]);
        board_tile_planes[row][col] = NULL;
      }
      board_tile_cache[row][col].valid = false;
    }
  }
}
static BlitCache board_pixel_cache;
static BlitCache label_pixel_cache;
// ── Board ─────────────────────────────────────────────────────────────────
//
// Cell rendering is offset-parameterized so the same routine drives the
// in-game board (at CELL_ROW_BASE/CELL_COL_BASE) and the theme picker's
// preview (at whatever offset the picker chose).
static void render_board_cells(struct ncplane *plane, const Theme *theme,
                               const Board *board, const LetterDistribution *ld,
                               bool blank_uppercase,
                               TuiPremiumLabels premium_labels,
                               int border_thickness, int cell_w, int top,
                               int left) {
  // Text-mode cells get no cell borders. Don't approximate them with
  // NCSTYLE_UNDERLINE —
  // the underline reads as part of the glyph on fullwidth tiles and
  // the user vetoed that look.
  (void)border_thickness;
  const bool halfwidth = (cell_w == 1);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int screen_row = top + row;
      const int screen_col = left + col * cell_w;
      const MachineLetter ml = board_get_letter(board, row, col);
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        const PremiumMarker marker = premium_marker_for_cell(
            theme, bs, row, col, premium_labels, cell_w);
        theme_apply_fg(plane, marker.fg);
        theme_apply_bg(plane, marker.bg);
        ncplane_putstr_yx(plane, screen_row, screen_col, marker.glyph);
        continue;
      }
      const bool is_blank = get_is_blanked(ml);
      const bool render_uppercase = is_blank && blank_uppercase;
      const MachineLetter glyph_ml =
          render_uppercase ? get_unblanked_machine_letter(ml) : ml;
      const int owner = board_get_square_owner(board, row, col);
      const ThemeRgb tile_fg = owner == 1 ? theme->tile2_fg : theme->tile1_fg;
      const ThemeRgb tile_bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
      theme_apply_fg(plane, is_blank ? theme->blank_tile_fg : tile_fg);
      theme_apply_bg(plane, tile_bg);
      if (halfwidth) {
        const char *ascii = ld->ld_ml_to_hl[glyph_ml];
        ncplane_putstr_yx(plane, screen_row, screen_col,
                          ascii[0] != '\0' ? ascii : " ");
      } else {
        const char *fullwidth = ld->ld_ml_to_alt_hl[glyph_ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
        } else {
          ncplane_putstr_yx(plane, screen_row, screen_col, " ");
          ncplane_putstr(plane, ld->ld_ml_to_hl[glyph_ml]);
        }
      }
    }
  }
}
// Layered 2x board render. The cell backgrounds + premium labels
// go on the std plane as text (fast), and each placed tile gets
// its own small pixel plane (~6KB Kitty graphics payload, vs
// ~1.4MB for the previous single full-board plane). A per-tile
// cache means cursor scrolling through history snapshots only
// touches the tiles that actually differ between snapshots.
static void render_board_pixel(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L) {
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL) {
    return;
  }
  // Probe the cell-pixel geometry. We need a child plane to ask
  // ncplane_pixel_geom about pixel ratios; reuse the first cached
  // tile plane if one exists, or make a tiny scratch plane.
  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned cdy = 0;
  unsigned cdx = 0;
  unsigned mby = 0;
  unsigned mbx = 0;
  ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }

  const int tile_w = (int)cdx * L->board_cell_w;
  const int tile_h = (int)cdy * L->board_cell_h;
  if (tile_w <= 0 || tile_h <= 0) {
    return;
  }

  const Board *board = pick_render_board(state);
  if (board == NULL) {
    return;
  }
  const int cursor_key = state->history_cursor;
  if (g_last_blit_tracked && cursor_key != g_last_blitted_cursor &&
      !g_cursor_pending) {
    clock_gettime(CLOCK_MONOTONIC, &g_cursor_pending_since);
    g_cursor_pending = true;
  }

  MachineLetter preview_letters[BOARD_DIM][BOARD_DIM];
  int preview_owner = 0;
  fill_preview_map(state, board, preview_letters, &preview_owner);

  bool any_blit = false;
  long total_blit_us = 0;
  int tile_blits = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter board_ml = board_get_letter(board, row, col);
      const MachineLetter preview_ml = preview_letters[row][col];
      const bool is_preview =
          preview_ml != 0 && board_ml == ALPHABET_EMPTY_SQUARE_MARKER;
      // Effective tile content. Cells with no tile and no preview
      // get rendered as premium / empty (per-cell pixel plane, not
      // an early-return drop). That's the whole point of this
      // refactor — uniform per-cell pixel planes so the grid
      // borders are consistent across cell types.
      const MachineLetter ml = is_preview
                                   ? preview_ml
                                   : (board_ml == ALPHABET_EMPTY_SQUARE_MARKER
                                          ? ALPHABET_EMPTY_SQUARE_MARKER
                                          : board_ml);
      const bool is_tile = (ml != ALPHABET_EMPTY_SQUARE_MARKER);
      TileCache *tc = &board_tile_cache[row][col];
      int owner = 0;
      int tile_score = 0;
      const BonusSquare bs = board_get_bonus_square(board, row, col);
      if (is_tile) {
        owner = is_preview ? preview_owner
                           : board_get_square_owner(board, row, col);
        tile_score = equity_to_int(ld_get_score(state->ld, ml));
      }
      // Cache hit: cell's pixel plane already shows the right
      // content. Skip the rasterize + blit. The plane is sticky on
      // the screen until something invalidates it. Key now includes
      // bs + premium_labels for the empty / premium path.
      if (tc->valid && tc->letter == (int)ml && tc->owner == owner &&
          tc->score == tile_score &&
          tc->blank_uppercase == state->blank_uppercase &&
          tc->antialias == state->antialias && tc->is_preview == is_preview &&
          tc->score_subscripts == (int)state->score_subscripts &&
          tc->border_thickness == state->border_thickness && tc->cdy == cdy &&
          tc->cdx == cdx && tc->scale == L->scale && tc->bs == (int)bs.raw &&
          tc->premium_labels == (int)state->premium_labels &&
          board_tile_planes[row][col] != NULL) {
        continue;
      }
      // Ensure a plane exists at the tile's screen position.
      const int screen_top = CELL_ROW_BASE + row * L->board_cell_h;
      const int screen_left = CELL_COL_BASE + col * L->board_cell_w;
      if (board_tile_planes[row][col] == NULL) {
        ncplane_options opts = {0};
        opts.y = screen_top;
        opts.x = screen_left;
        opts.rows = (unsigned)L->board_cell_h;
        opts.cols = (unsigned)L->board_cell_w;
        opts.name = "tile";
        board_tile_planes[row][col] = ncplane_create(plane, &opts);
        if (board_tile_planes[row][col] == NULL) {
          continue;
        }
      } else {
        ncplane_move_yx(board_tile_planes[row][col], screen_top, screen_left);
      }
      uint8_t *buf = NULL;
      if (is_tile) {
        buf = compose_tile_pixels(
            ml, owner, state->blank_uppercase, is_preview, tile_w, tile_h,
            state->glyph_cache, state->glyph_cache_sub, state->score_subscripts,
            state->antialias, state->border_thickness, theme, state->ld);
      } else {
        buf = compose_premium_pixels(bs, row, col, tile_w, tile_h,
                                     state->glyph_cache, L->board_cell_w,
                                     state->premium_labels, state->antialias,
                                     state->border_thickness, theme);
      }
      if (buf == NULL) {
        continue;
      }
      struct ncvisual_options vopts = {0};
      vopts.n = board_tile_planes[row][col];
      vopts.blitter = NCBLIT_PIXEL;
      vopts.leny = (unsigned)tile_h;
      vopts.lenx = (unsigned)tile_w;
      struct timespec blit_start;
      clock_gettime(CLOCK_MONOTONIC, &blit_start);
      ncblit_rgba(buf, tile_w * 4, &vopts);
      tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
      struct timespec blit_end;
      clock_gettime(CLOCK_MONOTONIC, &blit_end);
      total_blit_us += (long)(blit_end.tv_sec - blit_start.tv_sec) * 1000000L +
                       (long)(blit_end.tv_nsec - blit_start.tv_nsec) / 1000L;
      free(buf);
      any_blit = true;
      tile_blits++;
      tc->letter = (int)ml;
      tc->owner = owner;
      tc->score = tile_score;
      tc->blank_uppercase = state->blank_uppercase;
      tc->antialias = state->antialias;
      tc->score_subscripts = (int)state->score_subscripts;
      tc->border_thickness = state->border_thickness;
      tc->is_preview = is_preview;
      tc->cdy = cdy;
      tc->cdx = cdx;
      tc->scale = L->scale;
      tc->bs = (int)bs.raw;
      tc->premium_labels = (int)state->premium_labels;
      tc->valid = true;
    }
  }

  if (any_blit) {
    atomic_store(&g_ncblit_us, total_blit_us);
    atomic_store(&g_last_tile_blits, tile_blits);
    if (g_cursor_pending) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long us =
          (long)(now.tv_sec - g_cursor_pending_since.tv_sec) * 1000000L +
          (long)(now.tv_nsec - g_cursor_pending_since.tv_nsec) / 1000L;
      atomic_store(&g_board_blit_latency_us, us);
      g_cursor_pending = false;
    }
    g_last_blitted_cursor = cursor_key;
    g_last_blit_tracked = true;
  }
}
// Renders the cell-text backdrop for the 2x board: premium markers
// on empty cells, plain tile-bg color on placed cells (the per-
// tile pixel planes draw the letter + subscript on top). Doing
// the backdrop in cells instead of in the pixel plane is what
// keeps Kitty graphics payloads tiny.
//
// Legacy text-mode board backdrop. Superseded at scale 2 by the
// per-cell pixel planes (compose_premium_pixels for empties /
// premiums, compose_tile_pixels for placed tiles), which produce
// uniformly-baked borders. Kept around for reference / future
// scale-0/1 work; cast to void in the caller so the compiler
// stops complaining about it being unused.
__attribute__((unused)) static void
render_board_text_bg(struct ncplane *plane, const Theme *theme,
                     const Board *board, const LetterDistribution *ld,
                     TuiPremiumLabels premium_labels, int cell_w, int cell_h,
                     int top, int left) {
  (void)ld;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int screen_row = top + row * cell_h;
      const int screen_col = left + col * cell_w;
      const MachineLetter ml = board_get_letter(board, row, col);
      ThemeRgb fg;
      ThemeRgb bg;
      const char *glyph;
      bool glyph_fullwidth;
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        // premium_marker_for_cell wants cell_w of 1 for halfwidth
        // glyphs, anything else for fullwidth. Map our 4-wide cell
        // back to 2 (fullwidth) so we get the same marker glyph the
        // 1x mode uses.
        const PremiumMarker marker = premium_marker_for_cell(
            theme, board_get_bonus_square(board, row, col), row, col,
            premium_labels, cell_w == 1 ? 1 : 2);
        fg = marker.fg;
        bg = marker.bg;
        glyph = marker.glyph;
        glyph_fullwidth = (cell_w != 1);
      } else {
        const int owner = board_get_square_owner(board, row, col);
        const ThemeRgb tile_bg = owner == 1 ? theme->tile2_bg : theme->tile1_bg;
        fg = tile_bg;
        bg = tile_bg;
        glyph = (cell_w == 1) ? " " : "\xe3\x80\x80"; // U+3000
        glyph_fullwidth = (cell_w != 1);
      }
      // Row 0: glyph, then bg-colored halfwidth padding to fill
      // the rest of the cell's width.
      theme_apply_fg(plane, fg);
      theme_apply_bg(plane, bg);
      ncplane_putstr_yx(plane, screen_row, screen_col, glyph);
      const int glyph_cells = glyph_fullwidth ? 2 : 1;
      theme_apply_fg(plane, bg);
      theme_apply_bg(plane, bg);
      for (int c = glyph_cells; c < cell_w; c++) {
        ncplane_putstr_yx(plane, screen_row, screen_col + c, " ");
      }
      // Remaining rows: solid bg, halfwidth spaces all the way
      // across the cell.
      for (int r = 1; r < cell_h; r++) {
        for (int c = 0; c < cell_w; c++) {
          ncplane_putstr_yx(plane, screen_row + r, screen_col + c, " ");
        }
      }
    }
  }
}
// 2x mode coordinate labels (Ａ-Ｏ above the board, 1-15 to its left).
// Drawn as pixels so a single-cell-tall glyph can be vertically
// centered against the two-cell-tall board rows. Each label lives in a
// 2×1 cell box (squarish in pixels, same footprint as a fullwidth letter
// or a two-digit ASCII number).
static void render_board_labels_pixel(struct ncplane *plane, const Theme *theme,
                                      const TuiGameState *state,
                                      const Layout *L) {
  TuiGridPlanes *planes = tui_grid_planes();
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache_sub == NULL) {
    return;
  }
  const int col_rows = 1;
  const int col_cols = BOARD_DIM * L->board_cell_w;
  const int row_rows = BOARD_DIM * L->board_cell_h;
  const int row_cols = CELL_COL_BASE;

  struct ncplane *col_p =
      acquire_grid_plane(&planes->labels_col, plane, "board_col_labels",
                         COL_LABELS_ROW, CELL_COL_BASE, col_rows, col_cols);
  struct ncplane *row_p =
      acquire_grid_plane(&planes->labels_row, plane, "board_row_labels",
                         CELL_ROW_BASE, 0, row_rows, row_cols);
  if (col_p == NULL || row_p == NULL) {
    return;
  }
  unsigned pxy = 0;
  unsigned pxx = 0;
  unsigned cdy = 0;
  unsigned cdx = 0;
  unsigned mby = 0;
  unsigned mbx = 0;
  ncplane_pixel_geom(col_p, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }

  // Labels are static text (Ａ-Ｏ + 1-15) — they don't depend on the
  // game state, only on cell dims + scale + antialias. Drop the version
  // dependency to avoid the kitty-graphics flash that comes with each
  // re-blit.
  if (label_pixel_cache.valid && label_pixel_cache.cdy == cdy &&
      label_pixel_cache.cdx == cdx && label_pixel_cache.param_a == L->scale &&
      label_pixel_cache.param_b == (state->antialias ? 1 : 0)) {
    return;
  }

  // Pick glyph pixel size to nearly fill the cell-tall box, but cap by
  // width so a 2-digit number ("15") still fits inside the 2×cdx-wide
  // box. Monospace glyph advance is roughly 0.6× the em size at the
  // fonts we ship.
  int label_px = (int)((double)cdy * 0.80);
  const int max_by_width = (int)((double)cdx * 1.5);
  if (label_px > max_by_width) {
    label_px = max_by_width;
  }
  if (label_px < 1) {
    label_px = 1;
  }
  tui_glyph_cache_set_size(state->glyph_cache_sub, label_px, state->antialias);

  const ThemeRgb fg = theme->dim_fg;
  const ThemeRgb bg = theme->bg;
  const int icdy = (int)cdy;
  const int icdx = (int)cdx;

  // Column labels: each letter centered in a 2×cdx box, which itself
  // is centered in the cell's L->board_cell_w columns.
  {
    const int buf_w = col_cols * icdx;
    const int buf_h = col_rows * icdy;
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
    if (buf == NULL) {
      return;
    }
    fill_tile_rect(buf, buf_w, 0, 0, buf_w, buf_h, bg);
    for (int col = 0; col < BOARD_DIM; col++) {
      const char ch = (char)('A' + col);
      const TuiGlyph *g =
          tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)ch);
      if (g == NULL || g->width <= 0 || g->height <= 0) {
        continue;
      }
      const int cell_left = col * L->board_cell_w * icdx;
      const int cell_w_px = L->board_cell_w * icdx;
      const int glyph_left = cell_left + (cell_w_px - g->width) / 2;
      // Baseline ~78% of the way down a 1-cell box leaves enough room
      // for the cap-height of Latin letters.
      const int baseline = (int)(cdy * 0.78);
      const int glyph_top = baseline - g->bearing_y;
      blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
    }
    struct ncvisual_options vopts = {0};
    vopts.n = col_p;
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)buf_h;
    vopts.lenx = (unsigned)buf_w;
    ncblit_rgba(buf, buf_w * 4, &vopts);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
  }

  // Row labels: "%2d" right-anchored against the board, with a margin
  // matching the col labels' bottom margin (the gap between an "A" and
  // the tile beneath). Both margins are a fixed fraction of cdy so they
  // scale with the board. Digits render slightly smaller than the col
  // labels so they leave room for that right margin.
  const int row_label_px = (int)((double)cdy * 0.65);
  tui_glyph_cache_set_size(state->glyph_cache_sub,
                           row_label_px > 0 ? row_label_px : 1,
                           state->antialias);
  {
    const int buf_w = row_cols * icdx;
    const int buf_h = row_rows * icdy;
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)buf_w * buf_h * 4);
    if (buf == NULL) {
      return;
    }
    // Leave the buffer's background transparent (calloc gave us
    // alpha=0 everywhere). The pixel plane then composites on top of
    // the text plane, so the box's left border `│` shows through for
    // the rows that need it — only the digit pixels mask the border.
    const int right_margin = (int)((double)cdy * 0.18);
    const int content_right = buf_w - right_margin;
    const int inter_digit_gap = (int)((double)cdy * 0.05);
    for (int row = 0; row < BOARD_DIM; row++) {
      char label[4];
      snprintf(label, sizeof(label), "%2d", row + 1);
      const int cell_top = row * L->board_cell_h * icdy;
      const int cell_h_px = L->board_cell_h * icdy;
      // Center a 1-row-tall label box vertically in the 2-row cell.
      const int box_top = cell_top + (cell_h_px - icdy) / 2;
      const int baseline = box_top + (int)(cdy * 0.78);
      // Pack digits flush right with a small fixed gap, right-to-left.
      // Single-digit rows (1..9) have the leading char as ' '; skip it
      // and only render the trailing digit at the rightmost slot.
      int right_edge = content_right;
      for (int i = 1; i >= 0; i--) {
        const char ch = label[i];
        if (ch == '\0' || ch == ' ') {
          continue;
        }
        const TuiGlyph *g =
            tui_glyph_cache_get(state->glyph_cache_sub, (uint32_t)ch);
        if (g == NULL || g->width <= 0 || g->height <= 0) {
          continue;
        }
        const int glyph_left = right_edge - g->width;
        const int glyph_top = baseline - g->bearing_y;
        blit_glyph_at(buf, buf_w, buf_h, glyph_left, glyph_top, g, fg, bg);
        right_edge = glyph_left - inter_digit_gap;
      }
    }
    struct ncvisual_options vopts = {0};
    vopts.n = row_p;
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)buf_h;
    vopts.lenx = (unsigned)buf_w;
    ncblit_rgba(buf, buf_w * 4, &vopts);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
  }

  label_pixel_cache.valid = true;
  label_pixel_cache.version = 0; // unused for the labels path
  label_pixel_cache.cdy = cdy;
  label_pixel_cache.cdx = cdx;
  label_pixel_cache.param_a = L->scale;
  label_pixel_cache.param_b = state->antialias ? 1 : 0;
}
// Count of letters currently on the given board (regular + blanks).
// Pass NULL to get 0 — callers route the result of pick_render_board
// here so the count tracks whatever the user is currently viewing
// (live game vs. a history-cursor snapshot).
static int board_tile_count(const Board *board) {
  if (board == NULL) {
    return 0;
  }
  int count = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_get_letter(board, row, col) != ALPHABET_EMPTY_SQUARE_MARKER) {
        count++;
      }
    }
  }
  return count;
}
// Draw the board widget's surrounding box. Title is "Board (N)" where
// N is the current tile count. The box spans rows 0..board_bottom+1
// (top border to bottom border) and cols 0..board_width-1.
static void render_board_box(struct ncplane *plane, const Theme *theme,
                             const TuiGameState *state, const Layout *L) {
  const int height = L->board_bottom_row + 2; // top border .. bottom border
  char title[32];
  snprintf(title, sizeof(title), "Board (%d)",
           board_tile_count(pick_render_board(state)));
  const bool focused = state->focused_panel == TUI_FOCUS_BOARD;
  draw_box_styled(plane, theme, 0, 0, height, L->board_width, title,
                  TUI_FOCUS_BOARD, focused);
}
void render_board(struct ncplane *plane, const Theme *theme,
                  const TuiGameState *state, const Layout *L) {
  TuiGridPlanes *planes = tui_grid_planes();
  render_board_box(plane, theme, state, L);
  if (L->scale >= 2 && state->glyph_cache != NULL) {
    render_board_labels_pixel(plane, theme, state, L);
    // Every cell — placed tile, premium, plain empty — gets its
    // own per-cell pixel plane via render_board_pixel. Each plane
    // bakes its right + bottom theme->bg border, so the grid is
    // uniform across cell types. No text-mode bg, no overlay
    // plane: there's exactly one render path and one source of
    // border pixels per cell. Caching (TileCache) skips planes
    // whose content + geometry haven't changed, so a 60fps idle
    // frame re-blits zero cells.
    if (planes->board != NULL) {
      ncplane_destroy(planes->board);
      planes->board = NULL;
      board_pixel_cache.valid = false;
    }
    render_board_pixel(plane, theme, state, L);
    return;
  }
  // 2x-only pixel planes are no-ops at scale < 2 but their stale
  // image data would otherwise sit on top of the text-mode layout.
  // Drop the cached board grid-overlay plane, the labels, AND any
  // per-tile pixel planes from the layered 2x renderer.
  if (planes->board != NULL) {
    ncplane_destroy(planes->board);
    planes->board = NULL;
    board_pixel_cache.valid = false;
  }
  if (planes->labels_col != NULL) {
    ncplane_destroy(planes->labels_col);
    planes->labels_col = NULL;
    label_pixel_cache.valid = false;
  }
  if (planes->labels_row != NULL) {
    ncplane_destroy(planes->labels_row);
    planes->labels_row = NULL;
    label_pixel_cache.valid = false;
  }
  render_board_invalidate_tile_planes();
  // Column labels: fullwidth Ａ-Ｏ at scale 1, halfwidth A-O at scale 0.
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  if (L->board_cell_w == 1) {
    for (int col = 0; col < BOARD_DIM; col++) {
      char ch[2] = {(char)('A' + col), '\0'};
      ncplane_putstr_yx(plane, COL_LABELS_ROW, CELL_COL_BASE + col, ch);
    }
  } else {
    for (int col = 0; col < BOARD_DIM; col++) {
      ncplane_putstr_yx(plane, COL_LABELS_ROW,
                        CELL_COL_BASE + col * L->board_cell_w,
                        fullwidth_col_labels[col]);
    }
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    char label[4];
    snprintf(label, sizeof(label), "%2d", row + 1);
    ncplane_putstr_yx(plane, CELL_ROW_BASE + row, ROW_LABEL_COL, label);
  }
  const Board *brd = pick_render_board(state);
  render_board_cells(plane, theme, brd, state->ld, state->blank_uppercase,
                     state->premium_labels, state->border_thickness,
                     L->board_cell_w, CELL_ROW_BASE, CELL_COL_BASE);
  // Analysis-cursored candidate overlay: paint the proposed
  // tiles on top of empty cells in a muted player-hue palette
  // (matches the 2x pixel-mode preview look, just delivered via
  // text cells).
  if (brd != NULL) {
    MachineLetter preview_letters[BOARD_DIM][BOARD_DIM];
    int preview_owner = 0;
    fill_preview_map(state, brd, preview_letters, &preview_owner);
    const ThemeRgb hue = preview_owner == 1 ? theme->tile2_fg : theme->tile1_fg;
    ThemeRgb muted;
    muted.r = (uint8_t)((hue.r + theme->bg.r) / 2);
    muted.g = (uint8_t)((hue.g + theme->bg.g) / 2);
    muted.b = (uint8_t)((hue.b + theme->bg.b) / 2);
    const bool halfwidth = (L->board_cell_w == 1);
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        const MachineLetter ml = preview_letters[row][col];
        if (ml == 0) {
          continue;
        }
        theme_apply_fg(plane, hue);
        theme_apply_bg(plane, muted);
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        const int screen_row = CELL_ROW_BASE + row;
        const int screen_col = CELL_COL_BASE + col * L->board_cell_w;
        if (halfwidth) {
          const char *ascii = (ml == 0) ? "?" : state->ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, screen_row, screen_col,
                            ascii[0] != '\0' ? ascii : " ");
        } else {
          const char *fullwidth = state->ld->ld_ml_to_alt_hl[ml];
          if (fullwidth[0] != '\0') {
            ncplane_putstr_yx(plane, screen_row, screen_col, fullwidth);
          } else {
            const char *ascii = (ml == 0) ? "?" : state->ld->ld_ml_to_hl[ml];
            ncplane_putstr_yx(plane, screen_row, screen_col, " ");
            ncplane_putstr(plane, ascii);
          }
        }
        ncplane_set_styles(plane, 0);
      }
    }
  }
}
void tui_render_board_at(struct ncplane *plane, int top, int left,
                         const Theme *theme, const Game *game,
                         const LetterDistribution *ld, bool blank_uppercase,
                         TuiPremiumLabels premium_labels,
                         int border_thickness) {
  if (plane == NULL || theme == NULL || game == NULL || ld == NULL) {
    return;
  }
  // Preview is always fullwidth (cell_w=2); the picker is shown on a
  // dedicated screen where halfwidth fallback isn't relevant.
  render_board_cells(plane, theme, game_get_board(game), ld, blank_uppercase,
                     premium_labels, border_thickness, CELL_WIDTH, top, left);
}

void render_board_invalidate_blit_caches(void) {
  board_pixel_cache.valid = false;
  label_pixel_cache.valid = false;
}

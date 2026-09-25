#include "render_rack.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "frame_dump.h"
#include "glyph_cache.h"
#include "pixel_compose.h"
#include "render_common.h"
#include "render_planes.h"
#include "render_view.h"
#include "tui_ui_types.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Rack panel ────────────────────────────────────────────────────────────
//
// Rack tiles scale alongside the board: at scale=2 we composite an RGBA
// strip via FreeType (same path as the board); at scale=1 we use
// fullwidth Unicode glyphs in cells; at scale=0 we collapse to single
// ASCII chars. The panel box gains one row at scale=2 so the 2-row
// tiles fit.

// Per-tile state for the rack panel. Same idea as the board tile
// cache: one small pixel plane per rack slot, re-blit only when
// the letter at that slot changes. RACK_SIZE = 7 in standard
// games. When the bot finalizes a move and draws fresh tiles,
// only the slots that actually changed letters re-emit instead
// of the whole rack strip thrashing.
typedef struct {
  int letter;
  int player_idx;
  int score;
  bool antialias;
  bool ghost;
  bool empty; // concealed opponent tile (no letter)
  int score_subscripts;
  unsigned cdy, cdx;
  int scale;
  // Last screen position the plane was created at. Pixel-mode
  // terminals don't move sprixel pixels when ncplane_move_yx is
  // called; if our slot needs to shift (rack count changed →
  // start_col shifted), we have to destroy + recreate the plane
  // so the terminal drops the old sprixel and rebuilds at the
  // new position. Otherwise tiles can end up at mismatched y
  // coords (the "ghost Q is misplaced" bug).
  int screen_top;
  int screen_left;
  bool valid;
} RackTileCache;
static RackTileCache rack_tile_cache[RACK_SIZE];
static struct ncplane *rack_tile_planes[RACK_SIZE];
void invalidate_rack_tile_planes(void) {
  for (int i = 0; i < RACK_SIZE; i++) {
    if (rack_tile_planes[i] != NULL) {
      tui_plane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
    }
    rack_tile_cache[i].valid = false;
  }
}
// Rack-panel tile blits since start (NOT included in the per-frame
// board blits count) — surfaces a rack cache that re-composes every
// frame.
static _Atomic unsigned long g_rack_blits;
unsigned long tui_debug_rack_blits(void) { return atomic_load(&g_rack_blits); }
static BlitCache rack_pixel_cache;
static void render_rack_panel_pixel(struct ncplane *plane, const Theme *theme,
                                    const TuiGameState *state, const Layout *L,
                                    int start_col, int tile_count,
                                    bool conceal) {
  TuiGridPlanes *planes = tui_grid_planes();
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc == NULL || !notcurses_canpixel(nc) || state->glyph_cache == NULL) {
    return;
  }
  // When the rack drops to 0 tiles (e.g. P2 just went out), fall
  // through to the cleanup loop below so the previously-rendered
  // pixel planes get destroyed instead of sitting on screen
  // showing stale DI/etc tiles.
  if (tile_count <= 0) {
    for (int i = 0; i < RACK_SIZE; i++) {
      if (rack_tile_planes[i] != NULL) {
        tui_plane_destroy(rack_tile_planes[i]);
        rack_tile_planes[i] = NULL;
        rack_tile_cache[i].valid = false;
      }
    }
    return;
  }
  const int cell_w = L->board_cell_w; // 4
  const int cell_h = L->board_cell_h; // 2
  // Probe cell-pixel geometry off the parent plane.
  unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
  ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
  if (cdy == 0 || cdx == 0) {
    return;
  }
  const int tile_w = (int)cdx * cell_w;
  const int tile_h = (int)cdy * cell_h;
  if (tile_w <= 0 || tile_h <= 0) {
    return;
  }

  // Expand the rack into a per-slot letter array so we can cache
  // each slot independently. tile_count comes from the caller and
  // is the visible rack length. Cursor-aware: the on-turn player
  // is resolved through pick_render_on_turn so navigating to a
  // committed turn rewinds the rack panel to that turn's player
  // + rack.
  const int player_idx = pick_render_on_turn(state);
  MachineLetter slot_letters[RACK_SIZE];
  for (int i = 0; i < RACK_SIZE; i++) {
    slot_letters[i] = ALPHABET_EMPTY_SQUARE_MARKER;
  }
  bool slot_ghost[RACK_SIZE];
  for (int i = 0; i < RACK_SIZE; i++) {
    slot_ghost[i] = false;
  }
  // Concealed mode draws `tile_count` bare tile boxes (no letters) for
  // the on-turn opponent; skip all the real-rack expansion.
  if (!conceal) {
    const Rack *rack = pick_render_rack(state, player_idx);
    const int slot_max = tile_count < RACK_SIZE ? tile_count : RACK_SIZE;
    const int slot_idx = sort_rack_for_display(
        rack, state->ld, state->rack_sort, slot_letters, slot_max);
    compute_rack_ghost_mask(state, slot_letters, slot_idx, slot_ghost);
    for (int i = slot_idx; i < RACK_SIZE; i++) {
      slot_ghost[i] = false;
    }
  }

  // Drop any cached tile plane beyond the visible rack length so
  // shrinking rack (end of game) frees its planes.
  for (int i = tile_count; i < RACK_SIZE; i++) {
    if (rack_tile_planes[i] != NULL) {
      tui_plane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
      rack_tile_cache[i].valid = false;
    }
  }

  for (int i = 0; i < tile_count && i < RACK_SIZE; i++) {
    const MachineLetter ml = conceal ? 0 : slot_letters[i];
    const bool ghost = conceal ? false : slot_ghost[i];
    const bool empty = conceal;
    RackTileCache *rc = &rack_tile_cache[i];
    const int tile_score =
        (ml == 0) ? 0 : equity_to_int(ld_get_score(state->ld, ml));
    const int screen_top = L->rack_top + 1;
    const int screen_left = start_col + i * cell_w;
    // Pixel-mode terminals don't actually move sprixel pixels when
    // ncplane_move_yx is called — they only update notcurses'
    // internal plane state. If our slot needs to shift (because
    // the rack count changed and the rack is re-centered on the
    // board), the old sprixel content sits at the previous y/x
    // until we destroy + recreate the plane. So: any time the
    // cached screen position doesn't match the current one,
    // tear the plane down so we start fresh. The cache only
    // counts as "hit" when content AND position match.
    const bool same_pos = rc->valid && rack_tile_planes[i] != NULL &&
                          rc->screen_top == screen_top &&
                          rc->screen_left == screen_left;
    if (!same_pos && rack_tile_planes[i] != NULL) {
      tui_plane_destroy(rack_tile_planes[i]);
      rack_tile_planes[i] = NULL;
      rc->valid = false;
    }
    if (rc->valid && rc->letter == (int)ml && rc->player_idx == player_idx &&
        rc->score == tile_score && rc->antialias == state->antialias &&
        rc->ghost == ghost && rc->empty == empty &&
        rc->score_subscripts == (int)state->score_subscripts &&
        rc->cdy == cdy && rc->cdx == cdx && rc->scale == L->scale &&
        rack_tile_planes[i] != NULL) {
      continue;
    }
    if (rack_tile_planes[i] == NULL) {
      ncplane_options opts = {0};
      opts.y = screen_top;
      opts.x = screen_left;
      opts.rows = (unsigned)cell_h;
      opts.cols = (unsigned)cell_w;
      opts.name = "rack_tile";
      rack_tile_planes[i] = ncplane_create(plane, &opts);
      if (rack_tile_planes[i] == NULL) {
        continue;
      }
    }
    uint8_t *buf = compose_rack_tile_pixels(
        ml, player_idx, ghost, tile_w, tile_h, state->glyph_cache,
        state->glyph_cache_sub, state->score_subscripts, state->antialias,
        theme, state->ld, empty);
    if (buf == NULL) {
      continue;
    }
    struct ncvisual_options vopts = {0};
    vopts.n = rack_tile_planes[i];
    vopts.blitter = NCBLIT_PIXEL;
    vopts.leny = (unsigned)tile_h;
    vopts.lenx = (unsigned)tile_w;
    ncblit_rgba(buf, tile_w * 4, &vopts);
    atomic_fetch_add(&g_rack_blits, 1);
    tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx, (int)vopts.leny);
    free(buf);
    rc->letter = (int)ml;
    rc->player_idx = player_idx;
    rc->score = tile_score;
    rc->antialias = state->antialias;
    rc->ghost = ghost;
    rc->empty = empty;
    rc->score_subscripts = (int)state->score_subscripts;
    rc->cdy = cdy;
    rc->cdx = cdx;
    rc->scale = L->scale;
    rc->screen_top = screen_top;
    rc->screen_left = screen_left;
    rc->valid = true;
  }
  // The legacy single-plane rack_pixel_cache is unused now; clear
  // it so a future regression that re-enables it starts cold
  // rather than reusing stale state.
  rack_pixel_cache.valid = false;
  if (planes->rack != NULL) {
    tui_plane_destroy(planes->rack);
    planes->rack = NULL;
  }
}
void render_rack_panel(struct ncplane *plane, const Theme *theme,
                       const TuiGameState *state, const Layout *L) {
  TuiGridPlanes *planes = tui_grid_planes();
  const int box_height = L->rack_bottom - L->rack_top + 1;
  // Follow the History cursor: when parked on a committed turn we
  // show that turn's player + rack, not the live game's on-turn
  // player. pick_render_rack falls through to live state when the
  // cursor is on the label or a pending entry.
  const int player_idx = pick_render_on_turn(state);
  const Rack *rack = pick_render_rack(state, player_idx);
  const bool rack_focused = state->focused_panel == TUI_FOCUS_RACK;
  // Play-vs-computer: while it's the computer's turn its rack is concealed
  // (pick_render_rack returned NULL). Rather than blank the panel, show a
  // row of empty tile slots so the human sees "opponent's tiles hidden."
  const bool concealed = state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                         player_idx != state->human_player_idx &&
                         state->game != NULL &&
                         !tui_game_state_play_over(state);
  if (concealed) {
    draw_box_styled(plane, theme, L->rack_top, 0, box_height, L->board_width,
                    "Rack", TUI_FOCUS_RACK, rack_focused);
    const int cell_w = L->board_cell_w;
    const int board_center = CELL_COL_BASE + (BOARD_DIM * cell_w) / 2;
    int slot_col = board_center - (RACK_SIZE * cell_w) / 2;
    if (slot_col < 1) {
      slot_col = 1;
    }
    if (L->scale >= 2 && state->glyph_cache != NULL) {
      // Real tile-shaped boxes in the opponent's color, no letters.
      render_rack_panel_pixel(plane, theme, state, L, slot_col, RACK_SIZE,
                              true);
    } else {
      // 1x: tile-colored blanks (bg fill, no glyph). Clear any stale 2x
      // planes first in case the scale just changed.
      render_rack_panel_pixel(plane, theme, state, L, slot_col, 0, false);
      theme_apply_fg(plane, player_idx == 1 ? theme->rack_tile2_fg
                                            : theme->rack_tile1_fg);
      theme_apply_bg(plane, player_idx == 1 ? theme->rack_tile2_bg
                                            : theme->rack_tile1_bg);
      for (int i = 0; i < RACK_SIZE; i++) {
        ncplane_putstr_yx(plane, L->rack_top + 1, slot_col + i * cell_w,
                          cell_w == 1 ? " " : "  ");
      }
    }
    return;
  }
  if (rack == NULL) {
    return;
  }
  char title[32];
  snprintf(title, sizeof(title), "Rack (%d)", rack_get_total_letters(rack));
  draw_box_styled(plane, theme, L->rack_top, 0, box_height, L->board_width,
                  title, TUI_FOCUS_RACK, rack_focused);
  const LetterDistribution *ld = state->ld;

  const int cell_w = L->board_cell_w;
  const int total_letters = rack_get_total_letters(rack);
  const int rack_width = total_letters * cell_w;
  // Align the rack's center column with the board's middle column
  // (H8 in standard 15×15), not the rack panel's interior center.
  // Board cells span [CELL_COL_BASE, CELL_COL_BASE + BOARD_DIM*cell_w),
  // so the midpoint is CELL_COL_BASE + (BOARD_DIM*cell_w)/2. Centering
  // on the panel made the rack drift relative to the board because the
  // panel box starts at col 0 (not CELL_COL_BASE) — visibly off by 2
  // cols at fullwidth.
  const int board_center = CELL_COL_BASE + (BOARD_DIM * cell_w) / 2;
  int start_col = board_center - rack_width / 2;
  if (start_col < 1) {
    start_col = 1;
  }

  if (total_letters == 0) {
    // Idle / pre-game state: rack hasn't been drawn yet. Print a
    // single dim placeholder centered in the panel interior. The
    // 2x pixel path still gets called below so any leftover rack
    // pixel planes from a prior game get destroyed.
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    const char *msg = "(no rack specified)";
    const int msg_col = 1 + (L->board_width - 2 - (int)strlen(msg)) / 2;
    ncplane_putstr_yx(plane, L->rack_top + 1, msg_col, msg);
  }

  if (L->scale >= 2 && state->glyph_cache != NULL) {
    render_rack_panel_pixel(plane, theme, state, L, start_col, total_letters,
                            false);
    return;
  }

  // Drop the 2x-only rack pixel plane if we're not using it — stale
  // pixel content otherwise sits on top of the text-mode rack.
  if (planes->rack != NULL) {
    tui_plane_destroy(planes->rack);
    planes->rack = NULL;
    rack_pixel_cache.valid = false;
  }

  // Cell-text rack: row sits one below the box top. Halfwidth uses
  // single-char glyphs, fullwidth uses ld_ml_to_alt_hl.
  const bool halfwidth = (cell_w == 1);
  // Build slot_letters in the user's chosen display order, then
  // compute the ghost mask against it so previewed-move tiles
  // render gray in the same positions the pixel renderer uses.
  MachineLetter slot_letters[RACK_SIZE];
  const int slot_idx = sort_rack_for_display(rack, ld, state->rack_sort,
                                             slot_letters, RACK_SIZE);
  bool slot_ghost[RACK_SIZE];
  compute_rack_ghost_mask(state, slot_letters, slot_idx, slot_ghost);
  const ThemeRgb ghost_fg = {.r = 102, .g = 102, .b = 102};
  int col_offset = 0;
  for (int i = 0; i < slot_idx; i++) {
    const MachineLetter ml = slot_letters[i];
    {
      const bool is_ghost = slot_ghost[i];
      if (is_ghost) {
        theme_apply_fg(plane, ghost_fg);
        theme_apply_bg(plane, theme->bg);
      } else {
        theme_apply_fg(plane, player_idx == 1 ? theme->rack_tile2_fg
                                              : theme->rack_tile1_fg);
        theme_apply_bg(plane, player_idx == 1 ? theme->rack_tile2_bg
                                              : theme->rack_tile1_bg);
      }
      if (halfwidth) {
        // Halfwidth blank: just the "?" glyph. We used to splice a
        // zero-width non-joiner in between adjacent blanks to defeat
        // "??" font ligatures, but it caused the row containing
        // blanks to drop out on some terminals; the ligature is the
        // lesser evil.
        const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
        ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                          ascii[0] != '\0' ? ascii : " ");
      } else {
        const char *fullwidth = ld->ld_ml_to_alt_hl[ml];
        if (fullwidth[0] != '\0') {
          ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                            fullwidth);
        } else {
          const char *ascii = (ml == 0) ? "?" : ld->ld_ml_to_hl[ml];
          ncplane_putstr_yx(plane, L->rack_top + 1, start_col + col_offset,
                            " ");
          ncplane_putstr(plane, ascii);
        }
      }
      col_offset += cell_w;
    }
  }
}

void render_rack_invalidate_blit_cache(void) { rack_pixel_cache.valid = false; }

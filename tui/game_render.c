#include "game_render.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "analysis_rows.h"
#include "frame_dump.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "pixel_compose.h"
#include "render_analysis.h"
#include "render_bag.h"
#include "render_bars.h"
#include "render_board.h"
#include "render_history.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "render_pills.h"
#include "render_planes.h"
#include "render_rack.h"
#include "theme.h"
#include "tui_resize.h"
#include "tui_ui_types.h"
#include <ctype.h>
#include <limits.h>
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Three tile scales:
//   0 = halfwidth (1 col × 1 row per cell, single ASCII glyph)
//   1 = fullwidth (2 cols × 1 row, fullwidth Unicode glyph)
//   2 = double    (4 cols × 2 rows, FreeType pixel composite)
// compute_effective_scale picks the largest scale ≤ user_pref that fits
// the current plane. Returns -1 when even halfwidth is too cramped.

// ── Pixel-graphics grid overlay ───────────────────────────────────────────
//
// On terminals that support pixel graphics (Kitty graphics protocol or
// Sixel — ghostty/iTerm2/kitty/foot/modern xterm), draw an RGBA bitmap
// of N-pixel borders in theme->bg over a child plane positioned at a
// region of cells. Pixels with alpha=0 composite through to the std plane,
// so the cells' glyph content stays readable underneath. On terminals
// without pixel support, the calls are no-ops via notcurses_canpixel.

// Annotation editor's directional cursor — a pixel-blitted "next
// tile" marker drawn at the cell past the last tile of the typed
// play. Cached the same way rack tiles are: only re-rasterized
// when geometry, direction, or player changes.
typedef struct {
  bool vertical;
  int player_idx;
  int screen_top;
  int screen_left;
  unsigned cdy, cdx;
  int scale;
  bool valid;
} EditArrowCache;
static EditArrowCache edit_arrow_cache;
static struct ncplane *edit_arrow_plane;

static void invalidate_edit_arrow_plane(void) {
  if (edit_arrow_plane != NULL) {
    tui_plane_destroy(edit_arrow_plane);
    edit_arrow_plane = NULL;
  }
  edit_arrow_cache.valid = false;
}

// Tear down every cached tile plane: the board's per-cell planes, the
// rack's per-slot planes and the edit-arrow plane. Used when scale
// flips back to 1x and at game-reset / state-destroy.
static void invalidate_tile_planes(void) {
  render_board_invalidate_tile_planes();
  invalidate_rack_tile_planes();
  invalidate_edit_arrow_plane();
}

static void invalidate_blit_caches(void) {
  render_board_invalidate_blit_caches();
  render_rack_invalidate_blit_cache();
}

static void invalidate_grid_planes(void) {
  tui_planes_destroy_all();
  invalidate_blit_caches();
  invalidate_tile_planes();
}

void tui_game_render_reset_grids(void) { invalidate_grid_planes(); }

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal) {
  TuiGridPlanes *planes = tui_grid_planes();
  TuiHitMaps *hit = tui_hit_maps();
  if (plane == NULL || theme == NULL || state == NULL || state->game == NULL) {
    return;
  }

  // Invalidate modal hit-test data at the start of each frame.
  // render_modal_ex will set it valid again if a modal renders.
  hit->modal_hit_map.valid = false;

  // Force a defensive full repaint whenever the plane's dimensions have
  // changed since the previous render — not only when *this* call did the
  // resize. main.c's NCKEY_RESIZE handler calls ncplane_resize_simple
  // before we get here, so tui_sync_plane_to_terminal sees the size
  // already matches and reports no resize; without this extra check the
  // diff cache leaves stale content (especially in the right history
  // column on terminals that uncover new cells, like Ghostty after a
  // font-size change).
  unsigned dim_y = 0;
  unsigned dim_x = 0;
  ncplane_dim_yx(plane, &dim_y, &dim_x);
  static unsigned prev_dim_y = 0;
  static unsigned prev_dim_x = 0;
  const bool plane_resized_externally =
      prev_dim_y != dim_y || prev_dim_x != dim_x;
  prev_dim_y = dim_y;
  prev_dim_x = dim_x;
  // Keep the plane synced to the terminal, but DON'T let this call's
  // return value drive the (expensive) full repaint below. On terminals
  // where the ioctl size and notcurses' clamped plane size persistently
  // disagree, tui_sync_plane_to_terminal returns true every frame; using
  // it to trigger the repaint re-rasterized all ~225 board sprixels each
  // frame (~250ms — the 4fps). A genuine resize still shows up as a
  // dimension change (plane_resized_externally) on the next frame.
  tui_sync_plane_to_terminal(plane);
  const bool resized = plane_resized_externally;
  if (resized) {
    // Pixel-graphics planes cache an image at a specific cell offset; a
    // font-size change keeps the cell count but moves the underlying
    // pixel boundaries, so the previous image sits at the wrong place.
    // Destroying the cached planes forces them rebuilt from scratch in
    // this frame, which clears the stale image from the terminal.
    invalidate_grid_planes();
    // The glyph cache is keyed by target pixel height, which derives
    // from cdy — a font-size change makes every cached bitmap the wrong
    // size. Flush so render_board_pixel rerasterizes at the new ratio.
    tui_glyph_cache_reset(state->glyph_cache);
    // After a resize, notcurses' diff cache and the terminal's actual
    // screen state can disagree, leaving stale content (missing borders,
    // labels, premium markers, history entries) on the visible terminal.
    // Force every cell dirty by painting it with a sentinel color and
    // rendering, so the next normal render writes out *every* cell that
    // differs from the sentinel — i.e., everything.
    ncplane_dim_yx(plane, &dim_y, &dim_x);
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->bg);
    for (unsigned r = 0; r < dim_y; r++) {
      for (unsigned c = 0; c < dim_x; c++) {
        ncplane_putstr_yx(plane, (int)r, (int)c, " ");
      }
    }
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_render(nc);
    }
  }

  // Cell pixel-dim drift detection. Font tweaks that keep the
  // same row/col count can still shift the underlying cdy/cdx
  // (e.g., aspect-ratio change with the same nominal cell size).
  // Our per-tile sprixels were sized for the old cdy/cdx; the
  // terminal would scale them with nearest-neighbor to fit the
  // new cell area, producing blocky output. Destroying the planes
  // forces them rebuilt at the new dims this frame. Glyph caches
  // are also reset so the next set_size call re-rasterizes at the
  // new pixel height instead of reusing bitmaps for the old aspect.
  {
    unsigned probe_cdy = 0, probe_cdx = 0;
    ncplane_pixel_geom(plane, NULL, NULL, &probe_cdy, &probe_cdx, NULL, NULL);
    static unsigned prev_cdy = 0, prev_cdx = 0;
    if (probe_cdy > 0 && probe_cdx > 0 &&
        (probe_cdy != prev_cdy || probe_cdx != prev_cdx)) {
      if (prev_cdy != 0 || prev_cdx != 0) {
        invalidate_tile_planes();
        // Reset only the UI-thread-owned glyph caches here. The
        // pixel_glyph_cache pair is owned by the worker thread and
        // self-heals via set_size on its next compose — touching
        // it from the UI thread would race with worker reads.
        if (state->glyph_cache != NULL) {
          tui_glyph_cache_reset(state->glyph_cache);
        }
        if (state->glyph_cache_sub != NULL) {
          tui_glyph_cache_reset(state->glyph_cache_sub);
        }
      }
      prev_cdy = probe_cdy;
      prev_cdx = probe_cdx;
    }
  }

  theme_apply_base(plane, theme);

  // 2x mode is only honored when the host terminal supports pixel
  // graphics AND the bundled TTF actually loaded; otherwise we cap the
  // user's preference at fullwidth (scale=1). compute_effective_scale
  // then degrades further to halfwidth (scale=0) when the plane is too
  // narrow for fullwidth.
  struct notcurses *render_nc = ncplane_notcurses(plane);
  const bool pixel_ok = render_nc != NULL && notcurses_canpixel(render_nc);
  const int user_pref =
      (state->board_scale >= 2 && pixel_ok && state->glyph_cache != NULL) ? 2
                                                                          : 1;
  const Layout L = compute_layout(plane, user_pref, state);
  if (L.scale < 0) {
    render_too_small(plane, theme);
    ncplane_erase(plane);
    return;
  }

  // Clear the plane before redrawing. At 2x the board widget (box,
  // labels, and the 15x15 grid) is painted with per-cell pixel
  // sprixels; a blanket ncplane_erase would mark the cells UNDER those
  // sprixels dirty every frame, forcing notcurses to re-emit all ~225
  // board sprixels (~250ms — the source of input lag in 2x mode).
  // Instead erase only the regions OUTSIDE the board widget so its
  // cells stay unchanged and the sprixels elide; the board renderers
  // overwrite the box / labels with identical content, leaving nothing
  // under the board dirty. At 1x there are no sprixels, so a plain
  // full erase is both correct and cheap. The first frame after
  // switching into 2x also takes the full erase: the text a smaller
  // scale left under the board and rack (row labels, panel titles, bag
  // letters) would otherwise show through the sprixels' transparent
  // pixels, and every sprixel is new that frame anyway.
  static int prev_scale = -1;
  const bool entered_pixel_scale = L.scale >= 2 && prev_scale != L.scale;
  prev_scale = L.scale;
  if (L.scale >= 2 && !entered_pixel_scale) {
    unsigned total_rows = 0;
    unsigned total_cols = 0;
    ncplane_dim_yx(plane, &total_rows, &total_cols);
    // The board widget AND the rack panel (directly below it) are the
    // two big pixel-sprixel clusters in the left column. Protect both
    // from the erase so their sprixels elide; the box / title / tiles
    // are overwritten in place by their renderers. The bag panel below
    // the rack is plain text (changes as tiles are drawn) so it stays
    // in the erased zone.
    const int widget_w = L.board_width;
    const int widget_last_row = L.rack_bottom;
    // Right of the board + rack column (rows 0..widget_last_row).
    if ((int)total_cols > widget_w) {
      ncplane_erase_region(plane, 0, widget_w, widget_last_row + 1,
                           (int)total_cols - widget_w);
    }
    // Everything below the rack (bag panel, status/command bars).
    if ((int)total_rows > widget_last_row + 1) {
      ncplane_erase_region(plane, widget_last_row + 1, 0,
                           (int)total_rows - (widget_last_row + 1),
                           (int)total_cols);
    }
  } else {
    ncplane_erase(plane);
  }

  // Prepare the analysis rows once per frame, BEFORE rendering
  // the board — the on-board candidate preview resolves the
  // Analysis cursor against this prepared row list (including
  // the MOVE-column anchor lookup), so it must be in sync with
  // what render_analysis_panel is about to paint.
  populate_frame_analysis_rows(state);

  render_board(plane, theme, state, &L);
  // The text-mode (1x) board layout also drops the rack's and the edit
  // arrow's 2x pixel planes, whose stale images would otherwise sit on top.
  if (!(L.scale >= 2 && state->glyph_cache != NULL)) {
    invalidate_rack_tile_planes();
    invalidate_edit_arrow_plane();
  }
  // Grid lines are baked into each per-tile pixel buffer now (see
  // compose_tile_pixels); no separate overlay plane needed.
  // An overlay plane (since removed) was tried on top to give
  // premium squares the same right/bottom inset, but the overlay
  // plane's transparent regions don't pass through to the
  // per-tile sprixels underneath (terminal sprixel stacking
  // replaces rather than composes), so it occludes placed tiles
  // entirely. Premium-square gridding would need a different
  // approach — e.g., per-premium pixel planes that bake in the
  // border the same way tiles do.

  // Annotation editor: when the user's typed play validates, draw
  // a Unicode directional cursor on the next square past the
  // last tile so they can see at a glance where the next typed
  // letter will land. Horizontal moves get "→", vertical "↓".
  // If the cursor would fall outside the 15×15 board (i.e., the
  // play ends at the right or bottom edge), it spills one cell
  // past column O or row 15 onto the panel border / row gutter,
  // which is the natural "row 16 / column P" position.
  // Show the directional cursor whenever we have a TILE_PLACEMENT
  // preview Move — that includes coord-only entries (tiles_length
  // == 0), partial placements that don't validate yet, and fully
  // legal plays. The arrow's job is to communicate WHERE the next
  // typed letter will land; it doesn't need the play to be legal
  // for that to be useful.
  bool arrow_drawn = false;
  if (state->edit_history_idx >= 0 && state->edit_preview_move_valid &&
      state->edit_preview_move != NULL &&
      move_get_type(state->edit_preview_move) ==
          GAME_EVENT_TILE_PLACEMENT_MOVE) {
    const Move *pm = state->edit_preview_move;
    const int dir = move_get_dir(pm);
    const bool vertical = board_is_dir_vertical(dir);
    const int row0 = move_get_row_start(pm);
    const int col0 = move_get_col_start(pm);
    const int n = move_get_tiles_length(pm);
    int next_row = vertical ? row0 + n : row0;
    int next_col = vertical ? col0 : col0 + n;
    // Skip past any board tiles sitting in the cursor's path — the
    // next *typed* letter would play through them, so the cursor
    // belongs on the first EMPTY square beyond the play's span.
    // The engine board is seeked to this turn's pre-move position
    // during editing, so it holds every other turn's tiles (e.g.
    // a perpendicular word crossing the cursor cell) but not this
    // play's preview.
    {
      const Board *brd = game_get_board(state->game);
      while (brd != NULL && next_row >= 0 && next_row < BOARD_DIM &&
             next_col >= 0 && next_col < BOARD_DIM &&
             board_get_letter(brd, next_row, next_col) !=
                 ALPHABET_EMPTY_SQUARE_MARKER) {
        if (vertical) {
          next_row++;
        } else {
          next_col++;
        }
      }
    }
    if (next_row >= 0 && next_col >= 0 && next_row <= BOARD_DIM &&
        next_col <= BOARD_DIM) {
      const int screen_top = CELL_ROW_BASE + next_row * L.board_cell_h;
      const int screen_left = CELL_COL_BASE + next_col * L.board_cell_w;
      const int player_idx = state->history[state->edit_history_idx].player_idx;
      // 2x scale: pixel-blit a fat filled-triangle arrow so it
      // really fills the tile-sized cell at the same visual
      // weight as a played tile. Below 2x, fall through to the
      // text-mode "filled cell + glyph" approach.
      // The pixel-blitted arrow plane leaves sprixel residue on
      // the terminal — destroying our plane doesn't reclaim the
      // cells the terminal painted with the pixel image, even
      // through notcurses_refresh. Until that's resolved, render
      // the arrow as a text-mode tile-bg fill + glyph at every
      // scale. The visual is less tile-weight at 2x but doesn't
      // bleed colored cells across row 8 when the arrow moves.
      // Now that the cdy/cdx invalidator at the top of
      // tui_game_render destroys per-tile planes on geometry
      // change AND we destroy + recreate the arrow plane on
      // position change, sprixel residue is contained and pixel
      // arrows are safe to use at 2x. Scales 0/1 keep the
      // text-mode fallback so 1- and 2-cell cells stay readable.
      struct notcurses *arrow_nc = ncplane_notcurses(plane);
      const bool arrow_pixel_ok =
          L.scale == 2 && arrow_nc != NULL && notcurses_canpixel(arrow_nc);
      if (arrow_pixel_ok) {
        unsigned pxy = 0, pxx = 0, cdy = 0, cdx = 0, mby = 0, mbx = 0;
        ncplane_pixel_geom(plane, &pxy, &pxx, &cdy, &cdx, &mby, &mbx);
        if (cdy > 0 && cdx > 0) {
          const int tile_w = (int)cdx * L.board_cell_w;
          const int tile_h = (int)cdy * L.board_cell_h;
          // Pixel-mode terminals leave the sprixel pixels lit at
          // a plane's old position when we ncplane_move_yx it. The
          // arrow walks one cell per keystroke during a typed
          // move, which left a trail of green ghost-cells at every
          // prior arrow position. Destroy + recreate the plane
          // whenever the position changes so the terminal is
          // forced to drop the old sprixel and only the current
          // cell stays lit.
          const bool same_position =
              edit_arrow_cache.valid && edit_arrow_plane != NULL &&
              edit_arrow_cache.screen_top == screen_top &&
              edit_arrow_cache.screen_left == screen_left &&
              edit_arrow_cache.cdy == cdy && edit_arrow_cache.cdx == cdx &&
              edit_arrow_cache.scale == L.scale;
          if (!same_position) {
            invalidate_edit_arrow_plane();
          }
          const bool cache_hit =
              edit_arrow_cache.valid && edit_arrow_plane != NULL &&
              edit_arrow_cache.vertical == vertical &&
              edit_arrow_cache.player_idx == player_idx &&
              edit_arrow_cache.cdy == cdy && edit_arrow_cache.cdx == cdx &&
              edit_arrow_cache.scale == L.scale;
          if (edit_arrow_plane == NULL) {
            ncplane_options opts = {0};
            opts.y = screen_top;
            opts.x = screen_left;
            opts.rows = (unsigned)L.board_cell_h;
            opts.cols = (unsigned)L.board_cell_w;
            opts.name = "edit_arrow";
            edit_arrow_plane = ncplane_create(plane, &opts);
          }
          if (edit_arrow_plane != NULL) {
            if (!cache_hit) {
              uint8_t *buf = compose_arrow_pixels(
                  vertical, player_idx, tile_w, tile_h, state->antialias,
                  state->border_thickness,
                  state->pixel_glyph_cache != NULL ? state->pixel_glyph_cache
                                                   : state->glyph_cache,
                  theme);
              if (buf != NULL) {
                struct ncvisual_options vopts = {0};
                vopts.n = edit_arrow_plane;
                vopts.blitter = NCBLIT_PIXEL;
                vopts.leny = (unsigned)tile_h;
                vopts.lenx = (unsigned)tile_w;
                ncblit_rgba(buf, tile_w * 4, &vopts);
                tui_frame_dump_capture(vopts.n, buf, (int)vopts.lenx,
                                       (int)vopts.leny);
                free(buf);
                edit_arrow_cache.vertical = vertical;
                edit_arrow_cache.player_idx = player_idx;
                edit_arrow_cache.screen_top = screen_top;
                edit_arrow_cache.screen_left = screen_left;
                edit_arrow_cache.cdy = cdy;
                edit_arrow_cache.cdx = cdx;
                edit_arrow_cache.scale = L.scale;
                edit_arrow_cache.valid = true;
              }
            }
            arrow_drawn = true;
          }
        }
      }
      if (!arrow_drawn) {
        // Text-mode fallback. Paint the player tile-bg across the
        // cell and stamp the same heavy block-arrow glyph used at
        // 2x: ➡ U+27A1 (rightwards) / ⬇ U+2B07 (downwards). At
        // 2-cell-wide scales the glyph's East Asian Wide width
        // naturally spans the cell; at 1-cell-wide scale 0 it
        // sits in a single cell.
        const ThemeRgb tile_bg =
            player_idx == 1 ? theme->tile2_bg : theme->tile1_bg;
        const ThemeRgb tile_fg =
            player_idx == 1 ? theme->tile2_fg : theme->tile1_fg;
        theme_apply_bg(plane, tile_bg);
        theme_apply_fg(plane, tile_fg);
        for (int dr = 0; dr < L.board_cell_h; dr++) {
          for (int dc = 0; dc < L.board_cell_w; dc++) {
            ncplane_putstr_yx(plane, screen_top + dr, screen_left + dc, " ");
          }
        }
        // Plain text-presentation arrows (U+2192 → / U+2193 ↓).
        // Text-range codepoints — the terminal won't up-render
        // them as color emoji images, which is what caused every
        // emoji-range glyph choice to leave sprixel residue when
        // the arrow moved.
        //   scale 0 (cell_w=1): single arrow fills the cell.
        //   scale 1 (cell_w=2): doubled arrows fill the cell.
        //   scale 2 (cell_w=4): single arrow centered in the cell.
        // The 2x case uses a single glyph rather than doubled —
        // the doubled pair only filled 2 of 4 columns and read
        // as awkwardly skewed; one centered arrow reads cleaner
        // even though it doesn't fill the whole cell.
        const bool double_glyph = L.board_cell_w == 2;
        const char *glyph_h_single = "\xe2\x86\x92";             // →
        const char *glyph_v_single = "\xe2\x86\x93";             // ↓
        const char *glyph_h_double = "\xe2\x86\x92\xe2\x86\x92"; // →→
        const char *glyph_v_double = "\xe2\x86\x93\xe2\x86\x93"; // ↓↓
        const char *glyph =
            vertical ? (double_glyph ? glyph_v_double : glyph_v_single)
                     : (double_glyph ? glyph_h_double : glyph_h_single);
        const int glyph_w = double_glyph ? 2 : 1;
        const int glyph_row = screen_top + (L.board_cell_h - 1) / 2;
        const int glyph_col = screen_left + (L.board_cell_w - glyph_w) / 2;
        ncplane_set_styles(plane, NCSTYLE_BOLD);
        ncplane_putstr_yx(plane, glyph_row, glyph_col, glyph);
        ncplane_set_styles(plane, 0);
        theme_apply_bg(plane, theme->bg);
        arrow_drawn = true;
      }
    }
  }
  // No arrow this frame — drop the cached plane so the underlying
  // board (premium/empty cell paint) shows through.
  if (!arrow_drawn && edit_arrow_plane != NULL) {
    invalidate_edit_arrow_plane();
  }

  render_rack_panel(plane, theme, state, &L);
  render_bag_panel(plane, theme, state, &L);

  (void)time_per_side_seconds; // now read from state->time_per_side_seconds
  if (L.combined_pills_history) {
    draw_combined_pills_history_frame(
        plane, theme, state, &L, state->focused_panel == TUI_FOCUS_HISTORY);
  }
  render_player_pill(plane, theme, state, 0, L.pill1_top, L.pill1_left,
                     L.pill1_right, L.pills_halfwidth,
                     !L.combined_pills_history);
  render_player_pill(plane, theme, state, 1, L.pill2_top, L.pill2_left,
                     L.pill2_right, L.pills_halfwidth,
                     !L.combined_pills_history);
  render_history_panel(plane, theme, state, &L);
  render_analysis_panel(plane, theme, state, &L);

  render_pending_bar(plane, theme, state, &L);
  render_command_bar(plane, theme, state, &L, modal);
  render_command_palette(plane, theme, state, &L);
  render_status_bar(plane, theme, state, &L, modal);

  // Debug perf overlay disabled. The pipeline-instrumentation
  // counters (g_board_blit_latency_us, g_ncblit_us, g_max_frame_us,
  // g_last_tile_blits, g_sprixel_emits_delta, g_sprixel_elides_delta)
  // are still updated by render_board_pixel / main.c so a one-line
  // re-enable here is enough if we need to look at them again.

  // When a modal closes, drop its plane so the next open recreates it
  // at the right size and z-position. Cheap (one destroy) and keeps
  // the modal-open path's plane setup simple.
  if (modal == TUI_MODAL_NONE && planes->modal != NULL) {
    tui_plane_destroy(planes->modal);
    planes->modal = NULL;
  }
}

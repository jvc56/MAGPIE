#include "game_render.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/endgame.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "analysis_rows.h"
#include "frame_dump.h"
#include "game_state.h"
#include "glyph_cache.h"
#include "mach_compat.h"
#include "pixel_compose.h"
#include "render_analysis.h"
#include "render_bag.h"
#include "render_board.h"
#include "render_common.h"
#include "render_history.h"
#include "render_hit_test.h"
#include "render_layout.h"
#include "render_pills.h"
#include "render_planes.h"
#include "render_rack.h"
#include "render_view.h"
#include "theme.h"
#include "time_picker.h"
#include "tui_resize.h"
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

static _Atomic long g_max_frame_us;
// notcurses sprixel emission counters. The renderer queries
// these once per frame to compare against the last snapshot; the
// delta is how many sprixels notcurses actually pushed to the
// terminal that frame, vs how many it elided (placed without
// re-uploading) because the content was unchanged. High emit
// counts on idle frames mean the elision logic isn't catching
// our planes.
static _Atomic uint64_t g_sprixel_emits_delta;
static _Atomic uint64_t g_sprixel_elides_delta;

// Snapshot the notcurses sprixel counters and stash the per-frame
// delta into the atomics that the debug overlay reads. Called
// from main.c right after each notcurses_render(). Lets us see
// whether unchanged sprixels are being elided (placed via short
// re-positioning commands) or fully re-emitted (full RGBA push).
void tui_debug_record_sprixel_stats(uint64_t emits, uint64_t elides) {
  static uint64_t last_emits;
  static uint64_t last_elides;
  static bool init;
  if (!init) {
    last_emits = emits;
    last_elides = elides;
    init = true;
    return;
  }
  const uint64_t de = (emits >= last_emits) ? emits - last_emits : 0;
  const uint64_t dl = (elides >= last_elides) ? elides - last_elides : 0;
  atomic_store(&g_sprixel_emits_delta, de);
  atomic_store(&g_sprixel_elides_delta, dl);
  last_emits = emits;
  last_elides = elides;
}

// Last measured keypress-to-pixels latency (microseconds). -1 = none yet.
static _Atomic long g_input_lag_us = -1;
void tui_debug_set_input_lag_us(long us) { atomic_store(&g_input_lag_us, us); }

void tui_debug_record_frame_us(long frame_us) {
  static long max_in_window;
  static struct timespec window_start;
  static bool window_init;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!window_init) {
    window_init = true;
    window_start = now;
    max_in_window = 0;
  }
  if (frame_us > max_in_window) {
    max_in_window = frame_us;
  }
  const long since_start_ms =
      (long)(now.tv_sec - window_start.tv_sec) * 1000L +
      (long)(now.tv_nsec - window_start.tv_nsec) / 1000000L;
  if (since_start_ms >= 1000) {
    atomic_store(&g_max_frame_us, max_in_window);
    max_in_window = 0;
    window_start = now;
  }
}

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
    ncplane_destroy(edit_arrow_plane);
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

// EMA-smoothed nodes-per-second based on the per-frame delta of a
// monotonically-increasing node counter. Lighter damping (α=0.3) than
// measure_fps so the readout responds quickly when a search ramps up
// or winds down. Resets gracefully when the counter resets (new
// search) or when there's no active counter (returns 0).
static double measure_nps(uint64_t nodes_now) {
  static double ema = 0.0;
  static struct timespec last;
  static uint64_t last_nodes = 0;
  static bool inited = false;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!inited) {
    inited = true;
    last = now;
    last_nodes = nodes_now;
    return 0.0;
  }
  const double dt = (double)(now.tv_sec - last.tv_sec) +
                    (double)(now.tv_nsec - last.tv_nsec) / 1e9;
  // Reset path: counter went backward (new search) or no progress.
  // Realign the baseline and decay the EMA toward 0 so the readout
  // fades when the bot stops.
  if (nodes_now < last_nodes || nodes_now == 0) {
    last = now;
    last_nodes = nodes_now;
    ema = ema * 0.7;
    return ema;
  }
  if (dt <= 0.0 || dt > 5.0) {
    last = now;
    last_nodes = nodes_now;
    return ema;
  }
  const uint64_t delta = nodes_now - last_nodes;
  last = now;
  last_nodes = nodes_now;
  const double instant = (double)delta / dt;
  if (ema <= 0.0) {
    ema = instant;
  } else {
    ema = ema * 0.7 + instant * 0.3;
  }
  return ema;
}

// Render-health "fps": derived from the peak notcurses_render duration
// over the recent window (g_max_frame_us), NOT the frame-to-frame
// interval. The main loop now renders conditionally — it deliberately
// idles when nothing on screen changed — so an interval-based rate would
// read a misleading 1-2 fps on a static screen even though every render
// is instant. Reporting 1 / worst-recent-render-time (capped at the 60fps
// target) answers the question that actually matters — "are renders fast
// enough to feel smooth" — and only dips when a frame genuinely takes a
// long time to emit (e.g. a heavy 2x pixel-board re-blit).
static double measure_fps(void) {
  const long peak_us = atomic_load(&g_max_frame_us);
  if (peak_us <= 0) {
    return 60.0; // no slow frame measured yet — renders are instant
  }
  double fps = 1e6 / (double)peak_us;
  if (fps > 60.0) {
    fps = 60.0;
  }
  return fps;
}

// ── Status bar ────────────────────────────────────────────────────────────
// Pending-change banner: when the user has changed Lexicon or RIT
// via Settings but the live game is still using the values from
// game-state init, render a one-line summary just above the status
// bar so they see exactly what needs a New Game to apply. Subtle
// color (theme->dim_fg on theme->bg) since it's informational, not
// alarming.
static void render_pending_bar(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L) {
  if (L->pending_row < 0 || state == NULL) {
    return;
  }
  const int row = L->pending_row;
  theme_apply_fg(plane, theme->dim_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_set_styles(plane, 0);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
  char buf[160];
  int written = 0;
  written += snprintf(
      buf + written,
      sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
      " Next game:");
  bool any = false;
  if (strcmp(state->pending_lexicon, state->active_lexicon) != 0) {
    written += snprintf(
        buf + written,
        sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
        " lexicon %s \xe2\x86\x92 %s", state->active_lexicon,
        state->pending_lexicon);
    any = true;
  }
  if (state->pending_load_rit != state->active_load_rit) {
    written += snprintf(
        buf + written,
        sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
        "%s RIT %s \xe2\x86\x92 %s", any ? "," : "",
        state->active_load_rit ? "on" : "off",
        state->pending_load_rit ? "on" : "off");
    any = true;
  }
  snprintf(buf + written,
           sizeof(buf) > (size_t)written ? sizeof(buf) - (size_t)written : 0,
           " \xc2\xb7 restart to apply");
  ncplane_putstr_yx(plane, row, 0, buf);
}

// Command bar: always-on row directly above the status bar. Hosts
// the [0] focus indicator on the left and a placeholder right-side
// hint ("/ for cmd") prompting the user to enter command-input mode.
// When [0] is the focused index, the row paints on
// theme->panel_focus_border_bg with bold [0] — matching the panel
// border treatment so focus reads consistently across the chrome.
// The actual /-input + autocomplete is wired in a follow-up.
// Command palette popup: rendered above the command bar while
// slash mode is active. Lists commands whose name starts with the
// typed prefix and a short description, mirroring the Claude Code
// CLI's `/`-prompt style — typed prefix in theme->fg, remaining
// command-name letters and descriptions in a dim grey. When the
// prefix matches nothing, shows a single "No commands match"
// message instead of an empty popup. Drawn directly on the std
// plane and sized to the visible matches; the next frame's content
// rendering naturally repaints over it when the popup goes away.
static void render_command_palette(struct ncplane *plane, const Theme *theme,
                                   const TuiGameState *state, const Layout *L) {
  if (state == NULL || !state->slash_active) {
    return;
  }
  struct Cmd {
    const char *name;
    const char *desc;
  };
  static const struct Cmd cmds[] = {
      {"copy", "Copy current position to clipboard as CGP"},
      {"exit", "Quit MAGPIE TUI (alias for /quit)"},
      {"new", "Start a new game"},
      {"quit", "Quit MAGPIE TUI"},
      {"settings", "Open settings"},
  };
  static const int n_cmds = (int)(sizeof(cmds) / sizeof(cmds[0]));

  // Filter to prefix matches against the lowercase slash buffer.
  int match_idx[16];
  int n_match = 0;
  for (int i = 0;
       i < n_cmds && n_match < (int)(sizeof(match_idx) / sizeof(match_idx[0]));
       i++) {
    if (state->slash_len == 0 ||
        ((int)strlen(cmds[i].name) >= state->slash_len &&
         strncmp(cmds[i].name, state->slash_buf, (size_t)state->slash_len) ==
             0)) {
      match_idx[n_match++] = i;
    }
  }

  const int popup_rows = n_match > 0 ? n_match : 1;
  const int popup_top = L->command_bar_row - popup_rows;
  if (popup_top < 0) {
    return;
  }

  if (n_match == 0) {
    // Single-line "No commands match" message.
    char buf[128];
    snprintf(buf, sizeof(buf), " No commands match \"/%s\"", state->slash_buf);
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, 0);
    // Clear the row then write.
    for (unsigned c = 0; c < L->plane_cols; c++) {
      ncplane_putstr_yx(plane, popup_top, (int)c, " ");
    }
    ncplane_putstr_yx(plane, popup_top, 1, buf);
    return;
  }

  // Compute description column: aligns the descriptions across all
  // matching rows. Leading "/" plus the command name, plus a fixed
  // gap of 3 cells.
  int max_name = 0;
  for (int i = 0; i < n_match; i++) {
    const int w = (int)strlen(cmds[match_idx[i]].name);
    if (w > max_name) {
      max_name = w;
    }
  }
  const int name_col = 1; // 1-cell left pad
  const int desc_col = name_col + 1 /* "/" */ + max_name + 3;

  for (int i = 0; i < n_match; i++) {
    const int row = popup_top + i;
    const struct Cmd *c = &cmds[match_idx[i]];
    // Clear row to theme->bg first so we don't inherit colored
    // content from the panel that was drawn below.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_set_styles(plane, 0);
    for (unsigned col = 0; col < L->plane_cols; col++) {
      ncplane_putstr_yx(plane, row, (int)col, " ");
    }
    // Leading "/" in dim (it's the same for every row, not part of
    // the matched-prefix highlight).
    int col = name_col;
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    theme_apply_bg(plane, theme->bg);
    ncplane_putstr_yx(plane, row, col++, "/");
    // Matched prefix portion in bright theme->fg.
    theme_apply_fg(plane, theme->fg);
    for (int k = 0; k < state->slash_len && c->name[k] != '\0'; k++) {
      char ch[2] = {c->name[k], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Remainder of the command name in dim grey.
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    for (int k = state->slash_len; c->name[k] != '\0'; k++) {
      char ch[2] = {c->name[k], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Description column, dim grey.
    if (desc_col < (int)L->plane_cols) {
      theme_apply_fg(plane, theme->modal_shortcut_fg);
      ncplane_putstr_yx(plane, row, desc_col, c->desc);
    }
  }
}

static void render_command_bar(struct ncplane *plane, const Theme *theme,
                               const TuiGameState *state, const Layout *L,
                               TuiModalState modal) {
  if (L->command_bar_row < 0) {
    return;
  }
  const int row = L->command_bar_row;
  const bool focused = state != NULL && state->focused_panel == 0;
  const ThemeRgb bar_bg = focused ? theme->panel_focus_border_bg : theme->bg;
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, bar_bg);
  ncplane_set_styles(plane, 0);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }
  // Left side: "[0] Command>" prompt. The ">" hangs off "Command"
  // with no space, so it reads as one prompt token like a shell.
  // Focused: grey-on-grey "[0>" chip matching every other panel's
  // focus badge. Unfocused: dim "[0]" hint.
  int col = 1;
  if (focused) {
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->fg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr_yx(plane, row, col, "[0>");
  } else {
    theme_apply_fg(plane, theme->modal_shortcut_fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_putstr_yx(plane, row, col, "[0]");
  }
  col += 3;
  ncplane_set_styles(plane, 0);
  theme_apply_fg(plane, theme->fg);
  theme_apply_bg(plane, bar_bg);
  ncplane_putstr_yx(plane, row, col++, " ");
  ncplane_putstr_yx(plane, row, col, "Command>");
  col += 8;
  ncplane_putstr_yx(plane, row, col++, " ");

  // After the prompt we render either:
  //  - the slash-mode input (typed bold, autocomplete suffix dim),
  //  - the placeholder hint "/ to type commands" (dim italic),
  //  - or nothing if neither applies (slash mode not entered, [0]
  //    not focused).
  if (state != NULL && state->slash_active) {
    // "/" prompt, non-bold.
    theme_apply_fg(plane, theme->fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_set_styles(plane, 0);
    ncplane_putstr_yx(plane, row, col++, "/");
    const int typed_start = col;
    for (int i = 0; i < state->slash_len; i++) {
      char ch[2] = {state->slash_buf[i], '\0'};
      ncplane_putstr_yx(plane, row, col++, ch);
    }
    // Live terminal cursor at slash_cursor's column. Matching
    // commands and their descriptions appear in a popup above the
    // bar (render_command_palette), so there's no inline ghost text
    // here anymore.
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_enable(nc, row, typed_start + state->slash_cursor);
    }
  } else if (modal == TUI_MODAL_NONE) {
    // Not in slash mode and no modal open — show the "/" hint so
    // the user knows the command bar is reachable. When a modal
    // is up, the "/" key won't refocus [0] anyway, so suppress
    // the hint instead of advertising an action that won't work.
    theme_apply_fg(plane, theme->dim_fg);
    theme_apply_bg(plane, bar_bg);
    ncplane_set_styles(plane, NCSTYLE_ITALIC);
    ncplane_putstr_yx(plane, row, col, "/ to type commands");
    ncplane_set_styles(plane, 0);
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_disable(nc);
    }
  } else {
    // Modal up: nothing in the input slot. Make sure the live
    // terminal cursor (which slash mode may have enabled) is
    // disabled so it doesn't blink in the command bar while the
    // user is interacting with the modal.
    struct notcurses *nc = ncplane_notcurses(plane);
    if (nc != NULL) {
      notcurses_cursor_disable(nc);
    }
  }

  // Right side: when [0] is focused (and we're not actively in
  // slash mode), surface the alphabetical commands as inline help.
  // Each entry is "<KEY> <name>", separated by " · ". The key
  // letter renders in modal_shortcut_fg, the name in theme->fg.
  if (focused && !(state != NULL && state->slash_active)) {
    struct {
      const char *key;
      const char *name;
    } cmds[] = {{"N", "new"}, {"S", "settings"}, {"Q", "quit"}};
    const int n = (int)(sizeof(cmds) / sizeof(cmds[0]));
    // Pre-compute width to right-align.
    int total = 0;
    for (int i = 0; i < n; i++) {
      if (i > 0) {
        total += 3; // " · "
      }
      total += (int)strlen(cmds[i].key) + 1 + (int)strlen(cmds[i].name);
    }
    int rcol = (int)L->plane_cols - total - 1;
    if (rcol > col + 2) {
      for (int i = 0; i < n; i++) {
        if (i > 0) {
          theme_apply_fg(plane, theme->dim_fg);
          theme_apply_bg(plane, bar_bg);
          ncplane_putstr_yx(plane, row, rcol, " \xc2\xb7 "); // " · "
          rcol += 3;
        }
        theme_apply_fg(plane, theme->modal_shortcut_fg);
        theme_apply_bg(plane, bar_bg);
        ncplane_putstr_yx(plane, row, rcol, cmds[i].key);
        rcol += (int)strlen(cmds[i].key);
        theme_apply_fg(plane, theme->fg);
        ncplane_putstr_yx(plane, row, rcol++, " ");
        ncplane_putstr_yx(plane, row, rcol, cmds[i].name);
        rcol += (int)strlen(cmds[i].name);
      }
    }
  }
}

static void render_status_bar(struct ncplane *plane, const Theme *theme,
                              const TuiGameState *state, const Layout *L,
                              TuiModalState modal) {
  const int row = L->status_row;
  if (row < 0) {
    return;
  }

  // Inverted-grey band: dark text on dim_fg-colored bar, matching the
  // analysis-panel column header strip so chrome reads consistently.
  theme_apply_fg(plane, theme->bg);
  theme_apply_bg(plane, theme->dim_fg);
  for (unsigned col = 0; col < L->plane_cols; col++) {
    ncplane_putstr_yx(plane, row, (int)col, " ");
  }

  // Left side: "Language · Lexicon · 60 fps · 234MB mem". FPS and the
  // process resident-set size are tacked onto the left so they sit
  // next to the other engine-state readouts; the right side is
  // reserved for transient control hints. Show fps as an integer so
  // the last digit doesn't twitch every frame.
  const double fps = measure_fps();
  char mem_str[24];
  mem_str[0] = '\0';
  uint64_t resident_bytes = 0;
#ifdef __APPLE__
  {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &count) == KERN_SUCCESS) {
      resident_bytes = (uint64_t)info.resident_size;
    }
  }
#endif
  if (resident_bytes > 0) {
    // Pick the largest unit that keeps the number compact and the
    // precision sane: GB always shows 1 decimal; MB shows 2/1/0
    // decimals depending on magnitude so we land at "9.23MB",
    // "12.3MB", "234MB", "2.4GB".
    double val;
    const char *unit;
    int decimals;
    if (resident_bytes >= (1ULL << 30)) {
      val = (double)resident_bytes / (1024.0 * 1024.0 * 1024.0);
      unit = "GB";
      decimals = 1;
    } else {
      val = (double)resident_bytes / (1024.0 * 1024.0);
      unit = "MB";
      if (val >= 100.0) {
        decimals = 0;
      } else if (val >= 10.0) {
        decimals = 1;
      } else {
        decimals = 2;
      }
    }
    snprintf(mem_str, sizeof(mem_str), " \xc2\xb7 %.*f%s mem", decimals, val,
             unit);
  }
  char dim_str[48];
  unsigned cdy_now = 0, cdx_now = 0;
  ncplane_pixel_geom(plane, NULL, NULL, &cdy_now, &cdx_now, NULL, NULL);
  if (cdy_now > 0 && cdx_now > 0) {
    snprintf(dim_str, sizeof(dim_str), " \xc2\xb7 %ux%u (%ux%u)", L->plane_cols,
             L->plane_rows, cdx_now, cdy_now);
  } else {
    snprintf(dim_str, sizeof(dim_str), " \xc2\xb7 %ux%u", L->plane_cols,
             L->plane_rows);
  }
  // FPS is normally hidden — only surfaces when we're off the 60Hz
  // target (under 55 or over 70). When shown it gets bold + error_fg
  // so it reads as a "something's wrong" callout. The chunk is
  // rendered separately from left_buf so the styling is local to the
  // " · NN fps" segment and doesn't leak into the rest of the bar.
  const int fps_int = (int)(fps + 0.5);
  const bool show_fps = fps > 0.0 && (fps_int < 55 || fps_int > 70);
  char left_buf[192];
  snprintf(left_buf, sizeof(left_buf), " %s \xc2\xb7 %s",
           language_for_lexicon(state->lexicon), state->lexicon);
  char right_buf[64];
  snprintf(right_buf, sizeof(right_buf), "%s%s", mem_str, dim_str);
  // NPS: only shown while the bot is computing. Sim mode reports
  // iters*(plies+1); endgame reports the per-worker atomic sum.
  // measure_nps EMA-smooths the per-frame delta so the readout is
  // visually stable, and decays toward 0 when the counter idles.
  uint64_t bot_nodes = 0;
  if (atomic_load(&((TuiGameState *)state)->endgame_results_active)) {
    if (state->endgame_ctx != NULL) {
      bot_nodes = endgame_ctx_get_nodes_searched(state->endgame_ctx);
    }
  } else if (atomic_load(&((TuiGameState *)state)->sim_results_active) &&
             state->sim_results != NULL) {
    const int plies = sim_results_get_num_plies(state->sim_results);
    const uint64_t iters = sim_results_get_iteration_count(state->sim_results);
    bot_nodes = iters * (uint64_t)(plies + 1);
  }
  const double nps = measure_nps(bot_nodes);
  // Hide nps while the human is on turn in play-vs-computer — there's no
  // bot search running, so a lingering/decaying nps readout is just noise.
  const bool human_on_turn =
      state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER && state->game != NULL &&
      !tui_game_state_play_over(state) &&
      game_get_player_on_turn_index(state->game) == state->human_player_idx;
  const bool show_nps = nps >= 1.0 && !human_on_turn;

  ncplane_putstr_yx(plane, row, 0, left_buf);
  if (show_fps) {
    char fps_buf[24];
    snprintf(fps_buf, sizeof(fps_buf), " \xc2\xb7 %d fps", fps_int);
    theme_apply_fg(plane, theme->error_fg);
    theme_apply_bg(plane, theme->dim_fg);
    ncplane_set_styles(plane, NCSTYLE_BOLD);
    ncplane_putstr(plane, fps_buf);
    ncplane_set_styles(plane, 0);
    // Restore the inverted-grey band colors for the rest of the bar.
    theme_apply_fg(plane, theme->bg);
    theme_apply_bg(plane, theme->dim_fg);
  }
  if (show_nps) {
    char nps_str[16];
    format_count_compact((uint64_t)(nps + 0.5), nps_str, sizeof(nps_str));
    char nps_buf[32];
    snprintf(nps_buf, sizeof(nps_buf), " \xc2\xb7 %s nps", nps_str);
    ncplane_putstr(plane, nps_buf);
  }
  // Keypress-to-pixels latency (the time from a keystroke dirtying a
  // frame to that frame rendering). Shown in ms once we've measured one.
  const long input_lag_us = atomic_load(&g_input_lag_us);
  if (input_lag_us >= 0) {
    char lag_buf[32];
    snprintf(lag_buf, sizeof(lag_buf), " \xc2\xb7 %ld ms lag",
             (input_lag_us + 500) / 1000);
    ncplane_putstr(plane, lag_buf);
  }
  // Transient notice (e.g. "Copied CGP"). Expires via notice_expires_at;
  // the once-a-second clock render tick repaints the bar without it
  // within a second of expiry.
  if (state->notice_buf[0] != '\0' && (state->notice_expires_at.tv_sec != 0 ||
                                       state->notice_expires_at.tv_nsec != 0)) {
    struct timespec notice_now;
    clock_gettime(CLOCK_MONOTONIC, &notice_now);
    const bool notice_live =
        notice_now.tv_sec < state->notice_expires_at.tv_sec ||
        (notice_now.tv_sec == state->notice_expires_at.tv_sec &&
         notice_now.tv_nsec < state->notice_expires_at.tv_nsec);
    if (notice_live) {
      char notice_seg[80];
      snprintf(notice_seg, sizeof(notice_seg), " \xc2\xb7 %s",
               state->notice_buf);
      ncplane_set_styles(plane, NCSTYLE_BOLD);
      ncplane_putstr(plane, notice_seg);
      ncplane_set_styles(plane, 0);
    }
  }
  ncplane_putstr(plane, right_buf);

  // Right side: dynamic shortcut hint depending on what modal is open.
  // Key names use leading caps for consistency with the rest of the
  // chrome (e.g. status bar's "Esc menu", command bar's "Q quit").
  const char *hint = " Esc menu ";
  switch (modal) {
  case TUI_MODAL_MAIN_MENU:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_SETTINGS:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 \xe2\x86\x90\xe2"
           "\x86\x92 adjust \xc2\xb7 Esc back ";
    break;
  case TUI_MODAL_TIME_PICKER:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_LEXICON_PICKER:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_QUIT_CONFIRM:
    hint = " \xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter confirm \xc2"
           "\xb7 Esc back ";
    break;
  case TUI_MODAL_NONE:
  default:
    break;
  }
  const int hint_len = (int)strlen(hint);
  // Strlen counts bytes; UTF-8 multibyte chars need to be counted as
  // single columns. Approximate by subtracting an estimated byte-overhead.
  // Each ↑/↓/←/→ is 3 bytes (E2 86 9X) but 1 col; · is 2 bytes (C2 B7)
  // but 1 col. Compute visual width by walking.
  int hint_cols = 0;
  for (const unsigned char *p = (const unsigned char *)hint; *p != '\0'; p++) {
    if ((*p & 0xC0) != 0x80) { // not a UTF-8 continuation byte
      hint_cols++;
    }
  }
  (void)hint_len;
  const int right_col = (int)L->plane_cols - hint_cols;
  if (right_col > 0) {
    ncplane_putstr_yx(plane, row, right_col, hint);
  }
}

// ── Top-level render ──────────────────────────────────────────────────────
static void render_too_small(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 0, 0, "Terminal too small. Resize.");
}

void tui_game_render(struct ncplane *plane, const Theme *theme,
                     const TuiGameState *state, int time_per_side_seconds,
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
  // full erase is both correct and cheap.
  if (L.scale >= 2) {
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
  // Tried layering render_board_grid_overlay on top to give
  // premium squares the same right/bottom inset, but the overlay
  // plane's transparent regions don't pass through to the
  // per-tile sprixels underneath (terminal sprixel stacking
  // replaces rather than composes), so it occludes placed tiles
  // entirely. Premium-square gridding would need a different
  // approach — e.g., per-premium pixel planes that bake in the
  // border the same way tiles do.
  (void)render_board_grid_overlay;

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
    ncplane_destroy(planes->modal);
    planes->modal = NULL;
  }
}

// ── Modal helpers ─────────────────────────────────────────────────────────
//
// A modal is a centered box on top of the game frame. We render the items
// vertically with the focused row using accent_fg as a highlight stripe.

// shortcuts[i] is an optional right-aligned hint shown in a mid-grey
// next to items[i] (e.g. "N", "Esc"). Pass NULL to omit shortcuts
// entirely; individual entries may also be NULL/"" for items with no
// hint. The modal uses its own clinical-grey palette (theme->modal_*)
// rather than the game-content green/amber, so menus read as system
// chrome distinct from the game surface.
// An "input-field zone" decoration for a modal row: a fixed-width,
// right-anchored-within-the-row rectangle painted in a darker bg
// so the user can see exactly where typing lands. zone_starts[i]
// is the modal-local items[]-string offset of the zone's left
// edge; zone_widths[i] is the zone width in cells. Pass NULL for
// either to disable zones for the whole modal.
static void render_modal_ex(struct ncplane *plane, const Theme *theme,
                            const char *title, const char *const *items,
                            const char *const *shortcuts, const bool *disabled,
                            const int *cursor_cols, const int *zone_starts,
                            const int *zone_widths, int item_count, int focus,
                            int width) {
  TuiGridPlanes *planes = tui_grid_planes();
  TuiHitMaps *hit = tui_hit_maps();
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  // Items now sit flush against the top/bottom borders — no blank
  // padding row under the title — so height is exactly 2 + items.
  const int height = 2 + item_count;
  // Plane is height+1 × width+1 so we can paint a 1-cell drop shadow
  // along the bottom row and right column. The shadow uses half-block
  // glyphs (▀ ▌) with transparent bg, so the row under and column
  // right of the modal show through except for the thin shadow strip
  // hugging the modal's edge.
  const int plane_h = height + 1;
  const int plane_w = width + 1;
  if ((unsigned)plane_w >= plane_cols || (unsigned)plane_h >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - plane_h) / 2;
  const int left = (int)(plane_cols - plane_w) / 2;

  // Publish hit-test data for mouse-click handling. Items live at
  // rows [top+1 .. top+item_count] within columns [left+1 .. left+width-2]
  // (the 1-cell border on each side is non-clickable chrome). The
  // shadow row/col are not part of the clickable surface.
  hit->modal_hit_map.valid = true;
  hit->modal_hit_map.outer_top = top;
  hit->modal_hit_map.outer_bottom = top + height - 1;
  hit->modal_hit_map.outer_left = left;
  hit->modal_hit_map.outer_right = left + width - 1;
  hit->modal_hit_map.top = top + 1; // first item row
  hit->modal_hit_map.left = left + 1;
  hit->modal_hit_map.right = left + width - 2;
  hit->modal_hit_map.item_count =
      item_count < MODAL_MAX_ITEMS ? item_count : MODAL_MAX_ITEMS;
  for (int i = 0; i < hit->modal_hit_map.item_count; i++) {
    hit->modal_hit_map.disabled[i] = disabled != NULL && disabled[i];
    hit->modal_hit_map.left_chev_col[i] = -1;
    hit->modal_hit_map.right_chev_col[i] = -1;
    // Scan the item text for ◀ (E2 97 80) and ▶ (E2 96 B6).
    // Each chevron occupies 1 display column. Item text renders
    // starting at modal-interior col 3, so the screen column is
    // (left + 3 + display_offset).
    if (items != NULL && items[i] != NULL) {
      const unsigned char *s = (const unsigned char *)items[i];
      int disp = 0;
      while (*s != '\0') {
        if (s[0] == 0xe2 && s[1] == 0x97 && s[2] == 0x80) {
          hit->modal_hit_map.left_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] == 0xe2 && s[1] == 0x96 && s[2] == 0xb6) {
          hit->modal_hit_map.right_chev_col[i] = left + 3 + disp;
          s += 3;
          disp++;
        } else if (s[0] >= 0x80) {
          // Other multi-byte UTF-8 glyph; advance bytes by the
          // UTF-8 length and column by 1 (assumes BMP narrow,
          // which holds for the strings the modals build).
          int len = 1;
          if ((s[0] & 0xe0) == 0xc0) {
            len = 2;
          } else if ((s[0] & 0xf0) == 0xe0) {
            len = 3;
          } else if ((s[0] & 0xf8) == 0xf0) {
            len = 4;
          }
          s += len;
          disp++;
        } else {
          s++;
          disp++;
        }
      }
    }
  }

  // Modal lives on its own child plane that always sits on top of the
  // z-stack. Otherwise the 2x pixel composite (also a child of std)
  // sits above the modal, occluding it. Box-local coords run (0,0) to
  // (plane_h-1, plane_w-1); the modal proper occupies (0..height-1,
  // 0..width-1) and the shadow occupies (height, 1..width) plus
  // (1..height, width).
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)plane_h;
    opts.cols = (unsigned)plane_w;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != plane_h || (int)cur_cols != plane_w) {
      ncplane_resize_simple(planes->modal, (unsigned)plane_h,
                            (unsigned)plane_w);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
  // Plane base is transparent: cells outside the modal proper and
  // outside the shadow strips let the underlying game frame show
  // through. The modal area fills explicitly below.
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  // Reset the plane's current channels to fully-opaque defaults. The
  // shadow-pass at the end of this function leaves bg alpha set to
  // TRANSPARENT, and notcurses' ncplane_set_{fg,bg}_rgb8 only touches
  // the RGB bits — without this reset, every frame after the first
  // would inherit the transparent bg from the previous shadow pass
  // and paint the modal interior as see-through.
  ncplane_set_channels(mp, 0);

  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }

  // Frame chrome: top/bottom rows + left/right columns paint with
  // modal_border_bg (a hair lighter than the interior modal_bg) so
  // the edge reads as a defined trim — the macOS-style hairline.
  // Box-drawing glyphs sit on this trim in modal_border_fg.
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  if (title != NULL && title[0] != '\0') {
    // Title sits on the top frame strip, so its bg is modal_border_bg
    // to match the surrounding chrome (otherwise the " Title " label
    // appears in a darker pocket cut out of the lighter strip).
    theme_apply_fg(mp, theme->modal_fg);
    theme_apply_bg(mp, theme->modal_border_bg);
    ncplane_putstr_yx(mp, 0, 2, " ");
    ncplane_putstr(mp, title);
    ncplane_putstr(mp, " ");
  }

  // Item rows. Layout per row:
  //   [ space ][ space ][ label ............... ][ shortcut ][ space ]
  // Focused row gets a full-width selection bar (modal_focus_bg) so
  // the highlight reads as a coherent strip, not just a colored label.
  for (int i = 0; i < item_count; i++) {
    const int item_row = 1 + i;
    const bool focused = (i == focus);
    const bool item_disabled = disabled != NULL && disabled[i];
    // Disabled items never use the focus highlight — they paint
    // dim text on the unfocused row background so they read as
    // "informational only, not selectable".
    const ThemeRgb row_fg = item_disabled ? theme->modal_shortcut_fg
                            : focused     ? theme->modal_focus_fg
                                          : theme->modal_fg;
    const ThemeRgb row_bg =
        (focused && !item_disabled) ? theme->modal_focus_bg : theme->modal_bg;
    const ThemeRgb shortcut_fg = focused && !item_disabled
                                     ? theme->modal_focus_fg
                                     : theme->modal_shortcut_fg;

    // Fill the row background (between the side borders).
    theme_apply_fg(mp, row_fg);
    theme_apply_bg(mp, row_bg);
    for (int c = 1; c <= right_col - 1; c++) {
      ncplane_putstr_yx(mp, item_row, c, " ");
    }

    // Per-row input-field zone (e.g. annotate-setup name field).
    // Paint a darker bg over the zone before the items text so
    // the editable region reads as a recessed input rectangle.
    const int z_start = (zone_starts != NULL) ? zone_starts[i] : -1;
    const int z_width = (zone_widths != NULL) ? zone_widths[i] : 0;
    const bool has_zone = z_start >= 0 && z_width > 0;
    if (has_zone) {
      theme_apply_fg(mp, row_fg);
      theme_apply_bg(mp, theme->bg);
      for (int z = 0; z < z_width; z++) {
        ncplane_putstr_yx(mp, item_row, 3 + z_start + z, " ");
      }
    }

    // Label / value text. When a zone is set we split the paint
    // so the zone keeps its darker bg: label region uses row_bg,
    // zone region uses theme->bg.
    if (items[i] != NULL) {
      if (has_zone) {
        const int items_len = (int)strlen(items[i]);
        // Region before the zone.
        if (z_start > 0 && z_start <= items_len) {
          char before[96];
          int n = z_start;
          if (n > (int)sizeof(before) - 1) {
            n = sizeof(before) - 1;
          }
          memcpy(before, items[i], (size_t)n);
          before[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3, before);
        }
        // Region inside the zone.
        if (z_start < items_len) {
          char inside[96];
          int end = z_start + z_width;
          if (end > items_len) {
            end = items_len;
          }
          int n = end - z_start;
          if (n > (int)sizeof(inside) - 1) {
            n = sizeof(inside) - 1;
          }
          memcpy(inside, items[i] + z_start, (size_t)n);
          inside[n] = '\0';
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, theme->bg);
          ncplane_putstr_yx(mp, item_row, 3 + z_start, inside);
        }
        // Region after the zone, if any.
        const int after_off = z_start + z_width;
        if (after_off < items_len) {
          theme_apply_fg(mp, row_fg);
          theme_apply_bg(mp, row_bg);
          ncplane_putstr_yx(mp, item_row, 3 + after_off, items[i] + after_off);
        }
      } else {
        ncplane_putstr_yx(mp, item_row, 3, items[i]);
      }
    }

    // Right-aligned shortcut hint, 2-space right padding.
    if (shortcuts != NULL && shortcuts[i] != NULL && shortcuts[i][0] != '\0') {
      const int sc_len = (int)strlen(shortcuts[i]);
      const int sc_col = right_col - 2 - sc_len + 1;
      if (sc_col >= 3) {
        theme_apply_fg(mp, shortcut_fg);
        theme_apply_bg(mp, row_bg);
        ncplane_putstr_yx(mp, item_row, sc_col, shortcuts[i]);
      }
    }

    // Optional block cursor for this row. cursor_cols[i] is the
    // BYTE OFFSET into items[i] where the caret sits (-1 = no
    // cursor on this row). The cell repaints with inverted
    // colors — when the caret sits inside a zone we invert the
    // zone's darker bg, otherwise we invert the row bg. When the
    // caret is past the end of the string we draw a space, same
    // color treatment.
    if (cursor_cols != NULL && items[i] != NULL && cursor_cols[i] >= 0) {
      const int items_len = (int)strlen(items[i]);
      const int co = cursor_cols[i];
      const int screen_col = 3 + co;
      if (screen_col >= 1 && screen_col <= right_col - 1) {
        char ch[2] = {' ', '\0'};
        if (co < items_len) {
          ch[0] = items[i][co];
        }
        const bool over_zone =
            has_zone && co >= z_start && co < z_start + z_width;
        const ThemeRgb cursor_fg = over_zone ? theme->bg : row_bg;
        // Invert: cursor cell bg = row_fg, cursor cell fg = the
        // bg we'd otherwise have at this cell.
        theme_apply_fg(mp, cursor_fg);
        theme_apply_bg(mp, row_fg);
        ncplane_set_styles(mp, NCSTYLE_BOLD);
        ncplane_putstr_yx(mp, item_row, screen_col, ch);
        ncplane_set_styles(mp, 0);
      }
    }
  }

  // Drop shadow: offset 1 cell right / 1 row down. Bottom strip uses
  // ▀ (upper half block) so only the half-row immediately touching
  // the modal renders shadow color; the lower half stays transparent
  // and lets whatever's underneath show through. Right strip uses ▌
  // (left half block) symmetrically. The shadow skips the top-left
  // corner cells so it visibly comes from a top-left light source.
  // Glyphs paint with bg-alpha transparent so the uncovered half-cell
  // composes with the game plane behind.
  {
    uint64_t shadow_ch = 0;
    ncchannels_set_fg_rgb8(&shadow_ch, theme->modal_shadow_fg.r,
                           theme->modal_shadow_fg.g, theme->modal_shadow_fg.b);
    ncchannels_set_bg_alpha(&shadow_ch, NCALPHA_TRANSPARENT);
    ncplane_set_channels(mp, shadow_ch);
    const int shadow_row = height; // first row past modal's bottom
    const int shadow_col = width;  // first col past modal's right
    // Half-cell offset shadow: the right strip's ▌ paints the LEFT
    // half of col=width, so its right edge sits at the middle of
    // that cell. For a sharp bottom-right corner the bottom strip
    // has to end at that same middle. Symmetrically on the left,
    // the bottom strip starts at the middle of col=0 (a half-cell
    // offset from the modal's left edge, implying light from
    // upper-left). Quadrant glyphs handle the two end caps:
    //   col=0:       ▝ (upper-right quadrant)  — right-half + top-half
    //   1..width-1:  ▀ (upper half, full width)
    //   col=width:   ▘ (upper-left quadrant)   — left-half + top-half
    ncplane_putstr_yx(mp, shadow_row, 0, "\xe2\x96\x9d"); // ▝
    for (int c = 1; c < shadow_col; c++) {
      ncplane_putstr_yx(mp, shadow_row, c, "\xe2\x96\x80"); // ▀
    }
    ncplane_putstr_yx(mp, shadow_row, shadow_col, "\xe2\x96\x98"); // ▘
    for (int r = 1; r < shadow_row; r++) {
      ncplane_putstr_yx(mp, r, shadow_col, "\xe2\x96\x8c"); // ▌
    }
  }
}

// Backwards-compatible wrapper: existing callers (menu, settings,
// pickers) never gray out items, so they pass NULL for the
// disabled mask and reach the same paint code path.
static void render_modal(struct ncplane *plane, const Theme *theme,
                         const char *title, const char *const *items,
                         const char *const *shortcuts, int item_count,
                         int focus, int width) {
  render_modal_ex(plane, theme, title, items, shortcuts, /*disabled=*/NULL,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, item_count, focus, width);
}

// Forward declaration so tui_game_render_watch_setup can format
// adjustable rows using the same arrow-marker convention as the
// Settings modal. Defined a few hundred lines below.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused);

void tui_game_render_menu(struct ncplane *plane, const Theme *theme,
                          int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[TUI_MENU_ITEM_COUNT];
  const char *shortcuts[TUI_MENU_ITEM_COUNT];
  items[TUI_MENU_NEW_GAME] = "New game";
  shortcuts[TUI_MENU_NEW_GAME] = "N";
  items[TUI_MENU_SETTINGS] = "Settings";
  shortcuts[TUI_MENU_SETTINGS] = "S";
  items[TUI_MENU_BACK] = "Back";
  shortcuts[TUI_MENU_BACK] = "Esc";
  items[TUI_MENU_QUIT] = "Quit";
  shortcuts[TUI_MENU_QUIT] = "Q";
  render_modal(plane, theme, "Menu", items, shortcuts, TUI_MENU_ITEM_COUNT,
               focus, 28);
}

void tui_game_render_startup_menu(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Per-row buffers so we can append "(coming soon)" to unbuilt
  // modes without separate string literals for each variant.
  enum { ROW_BUF = 48 };
  static char buf[TUI_STARTUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_STARTUP_ITEM_COUNT];
  const char *shortcuts[TUI_STARTUP_ITEM_COUNT];
  bool disabled[TUI_STARTUP_ITEM_COUNT];
  const char *labels[TUI_STARTUP_ITEM_COUNT] = {
      "Watch computer play",  "Load a position",           "Load a game",
      "Annotate a live game", "Play against the computer",
  };
  const char *shortcut_chars[TUI_STARTUP_ITEM_COUNT] = {"W", "P", "G", "A",
                                                        "C"};
  // All modes are wired up. Any future unbuilt mode would render
  // dimmed with a "(coming soon)" tag and the cursor would skip past
  // it — flip its entry to true to do so.
  const bool item_disabled[TUI_STARTUP_ITEM_COUNT] = {
      false, false, false, false, false,
  };
  for (int i = 0; i < TUI_STARTUP_ITEM_COUNT; i++) {
    if (item_disabled[i]) {
      snprintf(buf[i], ROW_BUF, "%s (coming soon)", labels[i]);
    } else {
      snprintf(buf[i], ROW_BUF, "%s", labels[i]);
    }
    items[i] = buf[i];
    shortcuts[i] = item_disabled[i] ? NULL : shortcut_chars[i];
    disabled[i] = item_disabled[i];
  }
  render_modal_ex(plane, theme, "MAGPIE", items, shortcuts, disabled,
                  /*cursor_cols=*/NULL, /*zone_starts=*/NULL,
                  /*zone_widths=*/NULL, TUI_STARTUP_ITEM_COUNT, focus, 44);
}

// Format a Watch-setup row as "Label" left-aligned + "value"
// right-aligned within `content_w` display columns. When `focused`
// is true, the value is wrapped in ◀ ▶ markers to signal that
// Left/Right arrows will adjust it. `content_w` is the cell width
// available between the modal's 2-space left padding and the
// right border padding — the caller picks a value that matches
// the modal's `width - 4`.
static void format_setup_row(char *out, size_t out_size, int content_w,
                             const char *label, const char *value,
                             bool focused) {
  // Display width of the value, including arrow decorations when
  // focused. Each arrow is one display column despite being 3
  // bytes of UTF-8 (◀ = U+25C0, ▶ = U+25B6).
  const int value_disp = (int)strlen(value);
  const int decorated_disp = focused ? value_disp + 4 : value_disp;
  const int label_disp = (int)strlen(label);
  int pad = content_w - label_disp - decorated_disp;
  if (pad < 1) {
    pad = 1;
  }
  if (focused) {
    snprintf(out, out_size, "%s%*s\xe2\x97\x80 %s \xe2\x96\xb6", label, pad, "",
             value);
  } else {
    snprintf(out, out_size, "%s%*s%s", label, pad, "", value);
  }
}

void tui_game_render_watch_setup(struct ncplane *plane, const Theme *theme,
                                 int focus, int time_seconds,
                                 const char *language, const char *lexicon,
                                 int sim_plies, int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Resolve the time-control display string from whichever preset
  // currently matches. Falls back to a "Ns" form so a custom value
  // (e.g., loaded from config) renders sensibly.
  const int preset_idx = tui_time_picker_closest_index(time_seconds);
  const char *time_label =
      tui_time_picker_preset_seconds(preset_idx) == time_seconds
          ? tui_time_picker_preset_label(preset_idx)
          : NULL;
  char time_value[24];
  if (time_label != NULL) {
    snprintf(time_value, sizeof(time_value), "%s", time_label);
  } else if (time_seconds <= 0) {
    snprintf(time_value, sizeof(time_value), "untimed");
  } else if (time_seconds % 60 == 0) {
    snprintf(time_value, sizeof(time_value), "%d min", time_seconds / 60);
  } else {
    snprintf(time_value, sizeof(time_value), "%ds", time_seconds);
  }

  // Modal width chosen to comfortably fit the widest row. "Sim
  // candidates" + 4 digits + ◀ ▶ markers needs ~30 cols of
  // content; 56 keeps the value column visually anchored to the
  // right edge for every row.
  enum { MODAL_WIDTH = 56, CONTENT_W = MODAL_WIDTH - 4, ROW_BUF = 96 };
  static char buf[TUI_WATCH_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_WATCH_SETUP_ITEM_COUNT];
  const bool focus_time = (focus == TUI_WATCH_SETUP_TIME);
  const bool focus_lang = (focus == TUI_WATCH_SETUP_LANGUAGE);
  const bool focus_lex = (focus == TUI_WATCH_SETUP_LEXICON);
  const bool focus_plies = (focus == TUI_WATCH_SETUP_SIM_PLIES);
  const bool focus_cands = (focus == TUI_WATCH_SETUP_SIM_CANDIDATES);
  format_setup_row(buf[TUI_WATCH_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus_time);
  format_setup_row(
      buf[TUI_WATCH_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
      language != NULL && language[0] != '\0' ? language : "(none)",
      focus_lang);
  format_setup_row(buf[TUI_WATCH_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus_lex);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus_plies);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_WATCH_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str, focus_cands);
  snprintf(buf[TUI_WATCH_SETUP_START], ROW_BUF, "Start game");
  for (int i = 0; i < TUI_WATCH_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Watch setup", items, NULL,
               TUI_WATCH_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}

// Format a row with a right-anchored fixed-width input zone.
// Layout:
//   [label][   space-padding   ][   input zone   ]
// The input zone is `zone_width` cells wide and ends at column
// content_w-1. The value (possibly empty) sits left-justified
// inside the zone, padded with spaces so the entire zone is
// covered by characters — the renderer paints a darker bg on
// the zone, and characters here keep that bg.
static void format_setup_text_row(char *out, size_t out_size, int content_w,
                                  int zone_width, const char *label,
                                  const char *value) {
  if (out_size == 0) {
    return;
  }
  for (size_t i = 0; i < out_size - 1; i++) {
    out[i] = ' ';
  }
  out[out_size - 1] = '\0';
  const int label_disp = label != NULL ? (int)strlen(label) : 0;
  int li = 0;
  while (label != NULL && label[li] != '\0' && li < content_w &&
         (size_t)li < out_size - 1) {
    out[li] = label[li];
    li++;
  }
  const int zone_start = content_w - zone_width;
  (void)label_disp;
  if (value != NULL && zone_width > 0 && zone_start >= 0) {
    const int max_chars = zone_width - 1; // leave a trailing cell for the
                                          // end-of-text caret
    for (int i = 0; value[i] != '\0' && i < max_chars &&
                    (size_t)(zone_start + i) < out_size - 1;
         i++) {
      out[zone_start + i] = value[i];
    }
  }
  if ((size_t)content_w < out_size) {
    out[content_w] = '\0';
  }
}

void tui_game_render_annotate_setup(struct ncplane *plane, const Theme *theme,
                                    int focus, const char *lexicon,
                                    const char *p1_name, const char *p2_name,
                                    int name_edit_pos) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    // Right-anchored input zone for the player-name rows. Width
    // is the visual size of the rectangle the renderer paints
    // in a darker bg; the value sits left-justified inside, and
    // the trailing cell is reserved for the block cursor when
    // the caret is at end-of-text. 24 cells fits "PlayerNameHere "
    // — plenty for tournament-style nicknames.
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_ANNOTATE_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_starts[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  int zone_widths[TUI_ANNOTATE_SETUP_ITEM_COUNT];
  const bool focus_lex = (focus == TUI_ANNOTATE_SETUP_LEXICON);
  const bool focus_p1 = (focus == TUI_ANNOTATE_SETUP_P1_NAME);
  const bool focus_p2 = (focus == TUI_ANNOTATE_SETUP_P2_NAME);

  format_setup_row(
      buf[TUI_ANNOTATE_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
      lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)", focus_lex);
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P1_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 1",
                        p1_name != NULL ? p1_name : "");
  format_setup_text_row(buf[TUI_ANNOTATE_SETUP_P2_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Player 2",
                        p2_name != NULL ? p2_name : "");
  snprintf(buf[TUI_ANNOTATE_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_ANNOTATE_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  // Both name rows always show the input zone (so the user can
  // see where typing will land before they focus the row). The
  // block cursor is only painted on the focused row.
  zone_starts[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_W;
  zone_starts[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START;
  zone_widths[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_W;
  if (focus_p1) {
    cursor_cols[TUI_ANNOTATE_SETUP_P1_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_p2) {
    cursor_cols[TUI_ANNOTATE_SETUP_P2_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Annotate setup", items, /*shortcuts=*/NULL,
                  /*disabled=*/NULL, cursor_cols, zone_starts, zone_widths,
                  TUI_ANNOTATE_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}

// Render a load-style modal (text input + Enter-to-load + error
// line). Both Load-position (CGP) and Load-game (GCG) use this
// with just title + prompt differing.
static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error);

void tui_game_render_load_position(struct ncplane *plane, const Theme *theme,
                                   const char *buf, int cursor,
                                   const char *error) {
  render_load_text_modal(plane, theme, " Load position ",
                         "Type or paste a CGP format position, or drag a .cgp "
                         "file to this window.",
                         buf, cursor, error);
}

void tui_game_render_load_game(struct ncplane *plane, const Theme *theme,
                               const char *buf, int cursor, const char *error) {
  render_load_text_modal(plane, theme, " Load game ",
                         "Drag a .gcg file to this window (or type its path).",
                         buf, cursor, error);
}

static void render_load_text_modal(struct ncplane *plane, const Theme *theme,
                                   const char *title, const char *prompt,
                                   const char *buf, int cursor,
                                   const char *error) {
  TuiGridPlanes *planes = tui_grid_planes();
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Layout: prompt (1) + spacer (1) + INPUT_ROWS + spacer (1) +
  // Load button (1) + error line (1) inside the box, plus the
  // top/bottom borders. INPUT_ROWS includes a one-row pad below
  // the lowest line of text so the cursor doesn't sit flush
  // against the next row.
  enum {
    MODAL_WIDTH = 80,
    INPUT_ROWS = 8,
    INTERIOR_LEFT = 3,
  };
  const int interior_w = MODAL_WIDTH - 2 - INTERIOR_LEFT - 2;
  // Total content height: prompt (1) + blank (1) + INPUT_ROWS +
  // hint (1) + error (1) = INPUT_ROWS + 4.
  const int height = INPUT_ROWS + 4 + 2; // +2 for top/bottom borders
  const int width = MODAL_WIDTH;

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  if ((unsigned)width >= plane_cols || (unsigned)height >= plane_rows) {
    return;
  }
  const int top = (int)(plane_rows - height) / 2;
  const int left = (int)(plane_cols - width) / 2;

  // Reuse the shared modal plane (created on first use by
  // render_modal_ex) — same z-order rules apply. Re-create if
  // not present and size to our dimensions.
  if (planes->modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)height;
    opts.cols = (unsigned)width;
    opts.name = "modal";
    planes->modal = ncplane_create(plane, &opts);
    if (planes->modal == NULL) {
      return;
    }
  } else {
    unsigned cur_rows = 0;
    unsigned cur_cols = 0;
    ncplane_dim_yx(planes->modal, &cur_rows, &cur_cols);
    if ((int)cur_rows != height || (int)cur_cols != width) {
      ncplane_resize_simple(planes->modal, (unsigned)height, (unsigned)width);
    }
    ncplane_move_yx(planes->modal, top, left);
  }
  struct ncplane *mp = planes->modal;
  uint64_t base_ch = 0;
  ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
  ncplane_set_base(mp, " ", 0, base_ch);
  ncplane_erase(mp);
  ncplane_move_top(mp);
  ncplane_set_channels(mp, 0);

  // Background fill + border chrome.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int r = 0; r < height; r++) {
    for (int c = 0; c < width; c++) {
      ncplane_putstr_yx(mp, r, c, " ");
    }
  }
  const int right_col = width - 1;
  const int bottom_row = height - 1;
  theme_apply_fg(mp, theme->modal_border_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 0, BOX_TL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, 0, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, 0, right_col, BOX_TR);
  for (int row = 1; row < bottom_row; row++) {
    ncplane_putstr_yx(mp, row, 0, BOX_VT);
    ncplane_putstr_yx(mp, row, right_col, BOX_VT);
  }
  ncplane_putstr_yx(mp, bottom_row, 0, BOX_BL);
  for (int col = 1; col < right_col; col++) {
    ncplane_putstr_yx(mp, bottom_row, col, BOX_HZ);
  }
  ncplane_putstr_yx(mp, bottom_row, right_col, BOX_BR);

  // Title inset.
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_border_bg);
  ncplane_putstr_yx(mp, 0, 2, title != NULL ? title : " Load ");

  // Prompt row.
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  if (prompt != NULL) {
    ncplane_putstr_yx(mp, 1, INTERIOR_LEFT, prompt);
  }

  // Walk the buffer into (row, col) display coordinates.
  // For each character paint it within the input area; record
  // where the cursor lands so we can paint it inverted last.
  const int input_top = 3;
  const int input_left = INTERIOR_LEFT;
  int row_in = 0;
  int col_in = 0;
  int cursor_row = 0;
  int cursor_col = 0;
  theme_apply_fg(mp, theme->modal_fg);
  theme_apply_bg(mp, theme->modal_bg);
  for (int i = 0; buf != NULL && buf[i] != '\0'; i++) {
    if (i == cursor) {
      cursor_row = row_in;
      cursor_col = col_in;
    }
    const char ch = buf[i];
    if (ch == '\n') {
      row_in++;
      col_in = 0;
      continue;
    }
    if (row_in < INPUT_ROWS && col_in < interior_w) {
      char one[2] = {ch, '\0'};
      ncplane_putstr_yx(mp, input_top + row_in, input_left + col_in, one);
    }
    col_in++;
    if (col_in >= interior_w) {
      // Soft wrap so a long line keeps flowing into the next row.
      row_in++;
      col_in = 0;
    }
  }
  // Cursor at end-of-buffer case.
  if (buf == NULL || cursor >= (int)(buf != NULL ? strlen(buf) : 0)) {
    cursor_row = row_in;
    cursor_col = col_in;
  }
  // Render the cursor as an inverted cell so the user always
  // sees where the next inserted/deleted character will land.
  if (cursor_row < INPUT_ROWS) {
    const int cy = input_top + cursor_row;
    const int cx =
        input_left + (cursor_col < interior_w ? cursor_col : interior_w - 1);
    theme_apply_fg(mp, theme->modal_bg);
    theme_apply_bg(mp, theme->modal_fg);
    ncplane_putstr_yx(mp, cy, cx, " ");
  }

  // Hint row below the input area.
  const int hint_row = input_top + INPUT_ROWS;
  theme_apply_fg(mp, theme->modal_shortcut_fg);
  theme_apply_bg(mp, theme->modal_bg);
  ncplane_putstr_yx(mp, hint_row, INTERIOR_LEFT, "Enter: load  Esc: cancel");

  // Error line just below the hint, dim red.
  if (error != NULL && error[0] != '\0') {
    const int err_row = hint_row + 1;
    theme_apply_fg(mp, theme->error_fg);
    theme_apply_bg(mp, theme->modal_bg);
    char trunc[96];
    snprintf(trunc, sizeof(trunc), "%.*s", interior_w, error);
    ncplane_putstr_yx(mp, err_row, INTERIOR_LEFT, trunc);
  }
}

void tui_game_render_time_picker(struct ncplane *plane, const Theme *theme,
                                 int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const int n = tui_time_picker_preset_count();
  // Format each row as "1 minute    ultra" — left-justified label
  // followed by a blurb. render_modal takes plain strings so we
  // pre-format into per-row buffers and pass pointers into items[].
  enum { ROW_BUF = 40 };
  static char buf[8][ROW_BUF];
  const char *items[8];
  const int rows = n < 8 ? n : 8;
  for (int i = 0; i < rows; i++) {
    snprintf(buf[i], ROW_BUF, "%-12s %s", tui_time_picker_preset_label(i),
             tui_time_picker_preset_blurb(i));
    items[i] = buf[i];
  }
  render_modal(plane, theme, "Time control", items, NULL, rows, focus, 28);
}

void tui_game_render_quit_confirm(struct ncplane *plane, const Theme *theme,
                                  int focus) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  const char *items[2] = {"No", "Yes"};
  const char *shortcuts[2] = {"N", "Y"};
  render_modal(plane, theme, "Quit?", items, shortcuts, 2, focus, 24);
}

void tui_play_setup_enabled_rows(UiOvertimeRule overtime_rule, int time_seconds,
                                 UiChallengeRule challenge_rule,
                                 bool out_enabled[TUI_PLAY_SETUP_ITEM_COUNT]) {
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    out_enabled[item_idx] = true;
  }
  if (challenge_rule != UI_CHALLENGE_PENALTY) {
    out_enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY] = false;
  }
  if (time_seconds <= 0) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME] = false;
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
    return;
  }
  if (overtime_rule != UI_OVERTIME_MAX) {
    out_enabled[TUI_PLAY_SETUP_OVERTIME_CAP] = false;
  }
  if (overtime_rule == UI_OVERTIME_FLAG) {
    out_enabled[TUI_PLAY_SETUP_TIME_PENALTY] = false;
  }
}

void tui_game_render_play_setup(
    struct ncplane *plane, const Theme *theme, int focus,
    const char *human_name, const char *computer_name, int first_move,
    int name_edit_pos, int time_seconds, UiOvertimeRule overtime_rule,
    int overtime_cap_minutes, UiTimePenaltyRate time_penalty_rate,
    UiChallengeRule challenge_rule, UiChallengePenalty challenge_penalty,
    const char *language, const char *lexicon, int sim_plies,
    int sim_candidates) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  enum {
    MODAL_WIDTH = 56,
    CONTENT_W = MODAL_WIDTH - 4,
    ROW_BUF = 96,
    NAME_ZONE_W = 24,
    NAME_ZONE_START = CONTENT_W - NAME_ZONE_W,
  };
  static char buf[TUI_PLAY_SETUP_ITEM_COUNT][ROW_BUF];
  const char *items[TUI_PLAY_SETUP_ITEM_COUNT];
  int cursor_cols[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_starts[TUI_PLAY_SETUP_ITEM_COUNT];
  int zone_widths[TUI_PLAY_SETUP_ITEM_COUNT];
  bool enabled[TUI_PLAY_SETUP_ITEM_COUNT];
  bool disabled[TUI_PLAY_SETUP_ITEM_COUNT];
  tui_play_setup_enabled_rows(overtime_rule, time_seconds, challenge_rule,
                              enabled);
  for (int item_idx = 0; item_idx < TUI_PLAY_SETUP_ITEM_COUNT; item_idx++) {
    disabled[item_idx] = !enabled[item_idx];
  }
  const bool focus_human = (focus == TUI_PLAY_SETUP_HUMAN_NAME);
  const bool focus_comp = (focus == TUI_PLAY_SETUP_COMPUTER_NAME);

  format_setup_text_row(buf[TUI_PLAY_SETUP_HUMAN_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Your name",
                        human_name != NULL ? human_name : "");
  format_setup_text_row(buf[TUI_PLAY_SETUP_COMPUTER_NAME], ROW_BUF, CONTENT_W,
                        NAME_ZONE_W, "Computer name",
                        computer_name != NULL ? computer_name : "");
  const char *first_value = first_move == TUI_PLAY_FIRST_HUMAN      ? "Human"
                            : first_move == TUI_PLAY_FIRST_COMPUTER ? "Computer"
                                                                    : "Random";
  format_setup_row(buf[TUI_PLAY_SETUP_FIRST_MOVE], ROW_BUF, CONTENT_W,
                   "First move", first_value,
                   focus == TUI_PLAY_SETUP_FIRST_MOVE);

  // Time control — same preset resolution as the Watch-setup modal.
  const int preset_idx = tui_time_picker_closest_index(time_seconds);
  const char *time_label =
      tui_time_picker_preset_seconds(preset_idx) == time_seconds
          ? tui_time_picker_preset_label(preset_idx)
          : NULL;
  char time_value[24];
  if (time_label != NULL) {
    snprintf(time_value, sizeof(time_value), "%s", time_label);
  } else if (time_seconds <= 0) {
    snprintf(time_value, sizeof(time_value), "untimed");
  } else if (time_seconds % 60 == 0) {
    snprintf(time_value, sizeof(time_value), "%d min", time_seconds / 60);
  } else {
    snprintf(time_value, sizeof(time_value), "%ds", time_seconds);
  }
  format_setup_row(buf[TUI_PLAY_SETUP_TIME], ROW_BUF, CONTENT_W, "Time",
                   time_value, focus == TUI_PLAY_SETUP_TIME);

  // Overtime rule + its dependents. Disabled rows render their value
  // dimmed without the ◀ ▶ adjusters (the cap only matters under
  // "max overtime"; penalties don't exist under "flag at 0:00").
  const char *overtime_value =
      overtime_rule == UI_OVERTIME_FLAG  ? "flag at 0:00"
      : overtime_rule == UI_OVERTIME_MAX ? "max overtime"
                                         : "unlimited";
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME], ROW_BUF, CONTENT_W, "Overtime",
                   overtime_value,
                   focus == TUI_PLAY_SETUP_OVERTIME &&
                       enabled[TUI_PLAY_SETUP_OVERTIME]);
  // Disabled rows show a plain-ASCII "n/a" — format_setup_row pads by
  // byte length, so a multi-byte glyph (em dash) would right-align two
  // columns short.
  char cap_value[24];
  if (enabled[TUI_PLAY_SETUP_OVERTIME_CAP]) {
    snprintf(cap_value, sizeof(cap_value), "%d min", overtime_cap_minutes);
  } else {
    snprintf(cap_value, sizeof(cap_value), "n/a");
  }
  format_setup_row(buf[TUI_PLAY_SETUP_OVERTIME_CAP], ROW_BUF, CONTENT_W,
                   "Overtime cap", cap_value,
                   focus == TUI_PLAY_SETUP_OVERTIME_CAP &&
                       enabled[TUI_PLAY_SETUP_OVERTIME_CAP]);
  const char *penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_TIME_PENALTY]) {
    penalty_value = time_penalty_rate == UI_TIME_PENALTY_1_PER_SEC
                        ? "1 pt/sec"
                        : "10 pts/min";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_TIME_PENALTY], ROW_BUF, CONTENT_W,
                   "Time penalty", penalty_value,
                   focus == TUI_PLAY_SETUP_TIME_PENALTY &&
                       enabled[TUI_PLAY_SETUP_TIME_PENALTY]);

  // Challenge rule + its penalty variant (the variant row only
  // applies under the "penalty" rule).
  const char *challenge_value =
      challenge_rule == UI_CHALLENGE_VOID     ? "void"
      : challenge_rule == UI_CHALLENGE_SINGLE ? "single"
      : challenge_rule == UI_CHALLENGE_DOUBLE ? "double"
                                              : "penalty";
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE], ROW_BUF, CONTENT_W,
                   "Challenge", challenge_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE);
  const char *challenge_penalty_value = "n/a";
  if (enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]) {
    challenge_penalty_value =
        challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_PLAY    ? "5 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_10_PER_PLAY ? "10 pts/play"
        : challenge_penalty == UI_CHALLENGE_PENALTY_5_PER_WORD  ? "5 pts/word"
                                                                : "10 pts/word";
  }
  format_setup_row(buf[TUI_PLAY_SETUP_CHALLENGE_PENALTY], ROW_BUF, CONTENT_W,
                   "Challenge penalty", challenge_penalty_value,
                   focus == TUI_PLAY_SETUP_CHALLENGE_PENALTY &&
                       enabled[TUI_PLAY_SETUP_CHALLENGE_PENALTY]);

  format_setup_row(buf[TUI_PLAY_SETUP_LANGUAGE], ROW_BUF, CONTENT_W, "Language",
                   language != NULL && language[0] != '\0' ? language
                                                           : "(none)",
                   focus == TUI_PLAY_SETUP_LANGUAGE);
  format_setup_row(buf[TUI_PLAY_SETUP_LEXICON], ROW_BUF, CONTENT_W, "Lexicon",
                   lexicon != NULL && lexicon[0] != '\0' ? lexicon : "(none)",
                   focus == TUI_PLAY_SETUP_LEXICON);
  char plies_str[8];
  snprintf(plies_str, sizeof(plies_str), "%d", sim_plies);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_PLIES], ROW_BUF, CONTENT_W,
                   "Sim plies", plies_str, focus == TUI_PLAY_SETUP_SIM_PLIES);
  char cands_str[8];
  snprintf(cands_str, sizeof(cands_str), "%d", sim_candidates);
  format_setup_row(buf[TUI_PLAY_SETUP_SIM_CANDIDATES], ROW_BUF, CONTENT_W,
                   "Sim candidates", cands_str,
                   focus == TUI_PLAY_SETUP_SIM_CANDIDATES);
  snprintf(buf[TUI_PLAY_SETUP_START], ROW_BUF, "Start");
  for (int i = 0; i < TUI_PLAY_SETUP_ITEM_COUNT; i++) {
    items[i] = buf[i];
    cursor_cols[i] = -1;
    zone_starts[i] = -1;
    zone_widths[i] = 0;
  }
  zone_starts[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_W;
  zone_starts[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START;
  zone_widths[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_W;
  if (focus_human) {
    cursor_cols[TUI_PLAY_SETUP_HUMAN_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  if (focus_comp) {
    cursor_cols[TUI_PLAY_SETUP_COMPUTER_NAME] = NAME_ZONE_START + name_edit_pos;
  }
  render_modal_ex(plane, theme, "Play vs computer", items, /*shortcuts=*/NULL,
                  disabled, cursor_cols, zone_starts, zone_widths,
                  TUI_PLAY_SETUP_ITEM_COUNT, focus, MODAL_WIDTH);
}

// Helper for an arrow-adjusted Settings row. Renders
//   "<label>   ◀ <value> ▶"   when focused
//   "<label>   <value>"       when not focused
// `value` may be a fixed string (e.g., "lowercase") or numeric.
static void format_setting_row(char *out, size_t out_size, const char *label,
                               const char *value, bool focused) {
  if (focused) {
    snprintf(out, out_size, "%-13s\xe2\x97\x80 %s \xe2\x96\xb6", label, value);
  } else {
    snprintf(out, out_size, "%-13s%s", label, value);
  }
}

static const char *premium_labels_value(TuiPremiumLabels labels) {
  switch (labels) {
  case TUI_PREMIUM_LABELS_LOWERCASE:
    return "lowercase";
  case TUI_PREMIUM_LABELS_PUNCT:
    return "punctuation";
  case TUI_PREMIUM_LABELS_NONE:
    return "none";
  case TUI_PREMIUM_LABELS_UPPERCASE:
  case TUI_PREMIUM_LABELS_COUNT:
  default:
    return "uppercase";
  }
}

static const char *score_subscripts_value(TuiScoreSubscripts mode) {
  switch (mode) {
  case TUI_SCORE_SUBSCRIPTS_NONZERO:
    return "nonzero";
  case TUI_SCORE_SUBSCRIPTS_ALL:
    return "all";
  case TUI_SCORE_SUBSCRIPTS_OFF:
  case TUI_SCORE_SUBSCRIPTS_COUNT:
  default:
    return "off";
  }
}

// Display label for a rack-sort enum value. Concise on purpose so it
// fits in the right-aligned value column of the Settings modal:
//   "?+alpha" / "alpha+?" / "?+vow+con" / "vow+con+?"
// Leading "?+" means blanks come first; the rest is the letter
// ordering ("alpha" = alphabetical, "vow+con" = vowels then
// consonants).
static const char *rack_sort_value(TuiRackSort sort) {
  switch (sort) {
  case TUI_RACK_SORT_BLANKS_ALPHA:
    return "?+alpha";
  case TUI_RACK_SORT_BLANKS_VOWELS:
    return "?+vow+con";
  case TUI_RACK_SORT_VOWELS:
    return "vow+con+?";
  case TUI_RACK_SORT_ALPHA:
  case TUI_RACK_SORT_COUNT:
  default:
    return "alpha+?";
  }
}

void tui_game_render_settings(struct ncplane *plane, const Theme *theme,
                              int focus, int board_scale, bool antialias,
                              TuiScoreSubscripts score_subscripts,
                              int border_thickness, bool pixel_supported,
                              bool font_available,
                              TuiPremiumLabels premium_labels,
                              bool blank_uppercase, TuiRackSort rack_sort,
                              const char *lexicon, bool load_rit) {
  if (plane == NULL || theme == NULL) {
    return;
  }
  // Lexicon row has been removed — lexicon is set only via the
  // New Game / Watch setup flow. Keep the param for signature
  // stability with existing callers.
  (void)lexicon;

  // Scale row. 2x needs both pixel graphics and a loaded font; if
  // either is missing, the row reports unavailable and arrow keys
  // become no-ops at this focus. Even when 2x is supported the
  // terminal may currently be too small to fit 2x cells — in that
  // case we still show the preference (the user may want to set 2x
  // and resize) but flag that it can't render right now.
  char scale_label[96];
  const bool scale_available = pixel_supported && font_available;
  if (!scale_available) {
    snprintf(scale_label, sizeof(scale_label), "Scale        unsupported here");
  } else {
    unsigned plane_rows = 0;
    unsigned plane_cols = 0;
    ncplane_dim_yx(plane, &plane_rows, &plane_cols);
    const bool layout_fits_2x =
        compute_effective_scale(2, plane_cols, plane_rows) >= 2;
    char value_buf[32];
    if (board_scale >= 2 && !layout_fits_2x) {
      // The setting stays editable so the user can step back to 1x
      // without resizing first, but the value spells out why the
      // board is still rendering as 1x.
      snprintf(value_buf, sizeof(value_buf), "2x \xc2\xb7 too small");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dx", board_scale);
    }
    format_setting_row(scale_label, sizeof(scale_label), "Scale", value_buf,
                       focus == TUI_SETTINGS_SCALE);
  }

  // Antialiasing row — only meaningful when 2x is engaged.
  char aa_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(aa_label, sizeof(aa_label), "Antialias    n/a at 1x");
  } else {
    format_setting_row(aa_label, sizeof(aa_label), "Antialias",
                       antialias ? "on" : "off", focus == TUI_SETTINGS_AA);
  }

  // Score subscripts row — also 2x-only.
  char sub_label[96];
  if (!scale_available || board_scale < 2) {
    snprintf(sub_label, sizeof(sub_label), "Subscript    n/a at 1x");
  } else {
    format_setting_row(sub_label, sizeof(sub_label), "Subscript",
                       score_subscripts_value(score_subscripts),
                       focus == TUI_SETTINGS_SUBSCRIPTS);
  }

  // Border row.
  char border_label[96];
  if (!pixel_supported) {
    snprintf(border_label, sizeof(border_label),
             "Border       unsupported here");
  } else {
    char value_buf[16];
    if (border_thickness <= 0) {
      snprintf(value_buf, sizeof(value_buf), "off");
    } else {
      snprintf(value_buf, sizeof(value_buf), "%dpx", border_thickness);
    }
    format_setting_row(border_label, sizeof(border_label), "Border", value_buf,
                       focus == TUI_SETTINGS_BORDER);
  }

  // Premium label row.
  char premium_label[96];
  format_setting_row(premium_label, sizeof(premium_label), "Premium",
                     premium_labels_value(premium_labels),
                     focus == TUI_SETTINGS_PREMIUM);

  // Blanks row.
  char blanks_label[96];
  format_setting_row(blanks_label, sizeof(blanks_label), "Blanks",
                     blank_uppercase ? "uppercase" : "lowercase",
                     focus == TUI_SETTINGS_BLANKS);

  // Rack-sort row.
  char rack_sort_label[96];
  format_setting_row(rack_sort_label, sizeof(rack_sort_label), "Rack sort",
                     rack_sort_value(rack_sort),
                     focus == TUI_SETTINGS_RACK_SORT);

  // RIT row. Plain on/off arrow toggle like Antialias.
  char rit_label[96];
  format_setting_row(rit_label, sizeof(rit_label), "RIT",
                     load_rit ? "on" : "off", focus == TUI_SETTINGS_RIT);

  // Antialias / Subscript / Border are only meaningful at 2x — hide
  // them entirely when the board isn't rendering at 2x rather than
  // showing greyed "n/a at 1x" placeholders. settings_visible() in
  // main.c mirrors this so arrow-key navigation skips them.
  const bool effective_2x = scale_available && board_scale >= 2;
  const char *items[TUI_SETTINGS_ITEM_COUNT];
  int n = 0;
  int display_focus = 0;
  // Walk enum order; append a row if visible, and translate the
  // caller's enum-valued focus into the corresponding display index.
  for (int idx = 0; idx < TUI_SETTINGS_ITEM_COUNT; idx++) {
    const bool is_2x_only =
        (idx == TUI_SETTINGS_AA || idx == TUI_SETTINGS_SUBSCRIPTS ||
         idx == TUI_SETTINGS_BORDER);
    if (is_2x_only && !effective_2x) {
      continue;
    }
    const char *label = NULL;
    switch (idx) {
    case TUI_SETTINGS_SCALE:
      label = scale_label;
      break;
    case TUI_SETTINGS_AA:
      label = aa_label;
      break;
    case TUI_SETTINGS_SUBSCRIPTS:
      label = sub_label;
      break;
    case TUI_SETTINGS_BORDER:
      label = border_label;
      break;
    case TUI_SETTINGS_PREMIUM:
      label = premium_label;
      break;
    case TUI_SETTINGS_BLANKS:
      label = blanks_label;
      break;
    case TUI_SETTINGS_RACK_SORT:
      label = rack_sort_label;
      break;
    case TUI_SETTINGS_RIT:
      label = rit_label;
      break;
    case TUI_SETTINGS_BACK:
      label = "Back";
      break;
    default:
      continue;
    }
    if (idx == focus) {
      display_focus = n;
    }
    items[n++] = label;
  }
  render_modal(plane, theme, "Settings", items, NULL, n, display_focus, 40);
}

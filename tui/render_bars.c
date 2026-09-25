#include "render_bars.h"

#include "../src/ent/game.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/endgame.h"
#include "mach_compat.h"
#include "render_common.h"
#include "render_view.h"
#include "time_picker.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
void render_pending_bar(struct ncplane *plane, const Theme *theme,
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
void render_command_palette(struct ncplane *plane, const Theme *theme,
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
void render_command_bar(struct ncplane *plane, const Theme *theme,
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
void render_status_bar(struct ncplane *plane, const Theme *theme,
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
  // Esc cancels a half-typed slash command before it opens the menu.
  const char *hint = state->slash_active ? " Esc cancel " : " Esc menu ";
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
void render_too_small(struct ncplane *plane, const Theme *theme) {
  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 0, 0, "Terminal too small. Resize.");
}

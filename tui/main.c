#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gcg.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "bot_worker.h"
#include "config.h"
#include "frame_dump.h"
#include "game_render.h"
#include "game_state.h"
#include "lexicon_picker.h"
#include "onboarding.h"
#include "theme.h"
#include "time_picker.h"
#include <execinfo.h>
#include <fcntl.h>
#include <locale.h>
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
  TARGET_FPS = 60,
};

static const long FRAME_NS = 1000000000L / TARGET_FPS;

// ── Crash diagnostics ─────────────────────────────────────────────────────
//
// When the engine hits log_fatal (which abort()s) or anything else
// trips SIGSEGV/SIGBUS, we want to (a) put the terminal back in a
// usable state and (b) leave behind enough information to debug.
//
// notcurses_stop is NOT async-signal-safe but it's our only practical
// way to restore the tty from a handler — without it the user is left
// staring at smashed terminal state. The backtrace goes to a fixed
// file path so it survives even when stderr is restored to the tty
// and immediately scrolled away.

#define MAGPIE_CRASH_LOG "/tmp/magpie_crash.log"

static struct notcurses *g_crash_nc; // set when nc is alive; NULL otherwise

static void crash_write(int fd, const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  (void)!write(fd, s, n);
}

static void crash_handler(int signo) {
  // Restore the tty first so any text written afterward is readable.
  // Yes, technically not async-signal-safe — but the alternative is
  // an unusable terminal, which is worse.
  if (g_crash_nc != NULL) {
    struct notcurses *nc = g_crash_nc;
    g_crash_nc = NULL;
    notcurses_stop(nc);
  }

  int fd = open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fd = STDERR_FILENO;
  }
  crash_write(fd, "magpie_tui crashed: signal ");
  // signo is at most 2 digits; itoa manually since async-signal-safe.
  char buf[8];
  int n = 0;
  int s = signo < 0 ? -signo : signo;
  if (s == 0) {
    buf[n++] = '0';
  }
  while (s > 0 && n < (int)sizeof(buf)) {
    buf[n++] = (char)('0' + (s % 10));
    s /= 10;
  }
  for (int i = 0; i < n / 2; i++) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  (void)!write(fd, buf, (size_t)n);
  crash_write(fd, " (");
  const char *name = "?";
  switch (signo) {
  case SIGABRT:
    name = "SIGABRT";
    break;
  case SIGSEGV:
    name = "SIGSEGV";
    break;
  case SIGBUS:
    name = "SIGBUS";
    break;
  case SIGFPE:
    name = "SIGFPE";
    break;
  case SIGILL:
    name = "SIGILL";
    break;
  }
  crash_write(fd, name);
  crash_write(fd, ")\n\nBacktrace:\n");

  void *frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, fd);
  crash_write(fd, "\n");
  if (fd != STDERR_FILENO) {
    close(fd);
  }

  // Also dump to stderr so a `2>` redirect sees it.
  crash_write(STDERR_FILENO, "magpie_tui crashed: ");
  crash_write(STDERR_FILENO, name);
  crash_write(STDERR_FILENO, " — backtrace written to " MAGPIE_CRASH_LOG "\n");

  // Re-raise with the default handler so the process exits with the
  // right status (and gets dumped to a corefile when configured).
  signal(signo, SIG_DFL);
  raise(signo);
}

// SIGUSR1 arms a one-shot off-terminal PNG screenshot. The main loop
// services the request just after notcurses_render(). The handler only
// stores to an atomic flag, so it is async-signal-safe. This is the
// trigger for automated UI testing / remote inspection: drive input via
// a PTY harness, then `kill -USR1 <pid>` to capture what the renderer
// produced (the board + rack pixel planes the terminal never hands back).
static void dump_handler(int signo) {
  (void)signo;
  tui_frame_dump_request();
}

static void install_crash_handlers(void) {
  struct sigaction sa = {0};
  sa.sa_handler = crash_handler;
  sa.sa_flags = SA_RESETHAND; // one-shot; subsequent fault uses default
  sigemptyset(&sa.sa_mask);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  // SIGUSR1: capture a PNG screenshot. SA_RESTART so it doesn't EINTR
  // the input poll, and not SA_RESETHAND so repeated dumps work.
  struct sigaction dump_sa = {0};
  dump_sa.sa_handler = dump_handler;
  dump_sa.sa_flags = SA_RESTART;
  sigemptyset(&dump_sa.sa_mask);
  sigaction(SIGUSR1, &dump_sa, NULL);

  // Touch the log file so we can verify the handler at least got
  // installed — if /tmp/magpie_crash.log doesn't exist post-crash,
  // we know the install never ran or got clobbered.
  int fd = open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    crash_write(fd, "magpie_tui started, crash handlers installed\n");
    close(fd);
  }
}

typedef struct {
  const char *theme_arg;
  const char *config_path;
  bool reconfigure;
  bool no_config;
  bool watch;
  bool show_help;
  bool error;
} CliArgs;

static void print_usage(void) {
  fputs(
      "Usage: magpie_tui [options]\n"
      "\n"
      "Options:\n"
      "  --theme <name>   one-shot theme override; one of:\n"
      "                     dark, light, dim, high_contrast\n"
      "  --reconfigure    re-run all setup pickers (theme, lexicon, time)\n"
      "  --no-config      skip reading and writing the saved settings\n"
      "  --config <path>  read and write settings at <path> instead of the\n"
      "                   default location (for tests / alternate profiles)\n"
      "  --watch          skip the startup menu and start watching the bots\n"
      "                   play immediately with saved (or default) settings\n"
      "  --help, -h       show this help and exit\n"
      "\n"
      "On first run interactive pickers ask for theme, lexicon, and time\n"
      "control. Settings are saved to $XDG_CONFIG_HOME/magpie/tui.toml\n"
      "(default ~/.config/magpie/tui.toml), or to --config <path> if given.\n"
      "Subsequent runs reuse those settings unless --reconfigure is passed.\n",
      stderr);
}

static CliArgs parse_args(int argc, char *argv[]) {
  CliArgs args = {
      .theme_arg = NULL,
      .reconfigure = false,
      .no_config = false,
      .show_help = false,
      .error = false,
  };
  for (int idx = 1; idx < argc; idx++) {
    const char *arg = argv[idx];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      args.show_help = true;
    } else if (strcmp(arg, "--reconfigure") == 0) {
      args.reconfigure = true;
    } else if (strcmp(arg, "--no-config") == 0) {
      args.no_config = true;
    } else if (strcmp(arg, "--watch") == 0) {
      args.watch = true;
    } else if (strcmp(arg, "--theme") == 0) {
      if (idx + 1 >= argc) {
        fputs("magpie_tui: --theme requires an argument\n", stderr);
        args.error = true;
        return args;
      }
      const char *theme_id = argv[++idx];
      if (theme_get_by_id(theme_id) == NULL) {
        fprintf(stderr,
                "magpie_tui: unknown theme '%s'. "
                "Valid: dark, light, dim, high_contrast\n",
                theme_id);
        args.error = true;
        return args;
      }
      args.theme_arg = theme_id;
    } else if (strcmp(arg, "--config") == 0) {
      if (idx + 1 >= argc) {
        fputs("magpie_tui: --config requires a path argument\n", stderr);
        args.error = true;
        return args;
      }
      args.config_path = argv[++idx];
    } else {
      fprintf(stderr, "magpie_tui: unknown argument '%s'\n", arg);
      args.error = true;
      return args;
    }
  }
  return args;
}

static void render_init_error(struct ncplane *plane, const Theme *theme,
                              const char *lexicon, const char *message) {
  theme_apply_base(plane, theme);
  ncplane_erase(plane);

  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);

  theme_apply_fg(plane, theme->header_fg);
  theme_apply_bg(plane, theme->header_bg);
  for (unsigned col = 0; col < plane_cols; col++) {
    ncplane_putstr_yx(plane, 0, (int)col, " ");
  }
  ncplane_putstr_yx(plane, 0, 2, " MAGPIE TUI — could not start game ");

  theme_apply_fg(plane, theme->error_fg);
  theme_apply_bg(plane, theme->bg);
  ncplane_putstr_yx(plane, 3, 4, "Failed to load ");
  ncplane_putstr(plane, lexicon != NULL ? lexicon : "(unknown)");
  ncplane_putstr(plane, ":");

  theme_apply_fg(plane, theme->fg);
  ncplane_putstr_yx(plane, 5, 4, message != NULL ? message : "(no detail)");

  theme_apply_fg(plane, theme->dim_fg);
  ncplane_putstr_yx(plane, (int)plane_rows - 2, 4, "Press any key to exit.");
}

// Copy `text` to the system clipboard. Primary mechanism is OSC 52,
// written straight to the tty the same way the focus-reporting
// toggles are: supported by Ghostty / Kitty / WezTerm / iTerm2 (with
// the pref enabled) and works over SSH. On macOS, also pipe through
// pbcopy as a fallback for terminals without OSC 52 (Terminal.app).
static void tui_copy_to_clipboard(const char *text) {
  if (text == NULL) {
    return;
  }
  const size_t text_len = strlen(text);
  // Base64-encode for OSC 52: ESC ] 52 ; c ; <base64> BEL.
  static const char b64_alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char osc_prefix[] = "\x1b]52;c;";
  const size_t prefix_len = sizeof(osc_prefix) - 1;
  const size_t b64_len = 4 * ((text_len + 2) / 3);
  char *seq = malloc(prefix_len + b64_len + 2);
  if (seq == NULL) {
    return;
  }
  memcpy(seq, osc_prefix, prefix_len);
  char *out = seq + prefix_len;
  size_t in_idx = 0;
  while (in_idx < text_len) {
    const uint32_t byte0 = (uint8_t)text[in_idx];
    const uint32_t byte1 =
        in_idx + 1 < text_len ? (uint8_t)text[in_idx + 1] : 0;
    const uint32_t byte2 =
        in_idx + 2 < text_len ? (uint8_t)text[in_idx + 2] : 0;
    const uint32_t chunk = (byte0 << 16) | (byte1 << 8) | byte2;
    *out++ = b64_alphabet[(chunk >> 18) & 0x3f];
    *out++ = b64_alphabet[(chunk >> 12) & 0x3f];
    *out++ = in_idx + 1 < text_len ? b64_alphabet[(chunk >> 6) & 0x3f] : '=';
    *out++ = in_idx + 2 < text_len ? b64_alphabet[chunk & 0x3f] : '=';
    in_idx += 3;
  }
  *out++ = '\x07';
  (void)!write(STDOUT_FILENO, seq, (size_t)(out - seq));
  free(seq);
#ifdef __APPLE__
  FILE *clip_pipe = popen("pbcopy", "w");
  if (clip_pipe != NULL) {
    fwrite(text, 1, text_len, clip_pipe);
    pclose(clip_pipe);
  }
#endif
}

// Copy the current live position to the clipboard as a CGP string
// and flash a status-bar notice. In a live play-vs-computer game the
// computer's rack is blanked out of the CGP — consistent with the
// rack-panel / history concealment, the clipboard must not leak the
// computer's tiles; once the game is over the full position is
// copied. Caller must hold gs->mutex.
static void tui_copy_position_cgp(TuiGameState *gs) {
  if (gs->game == NULL) {
    return;
  }
  char *cgp = game_get_cgp(gs->game, true);
  if (cgp == NULL) {
    return;
  }
  const bool conceal =
      gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER && !game_over(gs->game);
  if (conceal) {
    // CGP is "<board> <rack_on_turn>/<rack_other> <scores> <zeros>".
    // Blank the computer's side of the racks token by splicing the
    // string — the real Game is left untouched.
    const int on_turn_idx = game_get_player_on_turn_index(gs->game);
    const bool computer_on_turn = on_turn_idx != gs->human_player_idx;
    char *racks = strchr(cgp, ' ');
    if (racks != NULL) {
      racks++;
      char *rack_slash = strchr(racks, '/');
      const char *racks_end = strchr(racks, ' ');
      if (rack_slash != NULL && racks_end != NULL && rack_slash < racks_end) {
        if (computer_on_turn) {
          memmove(racks, rack_slash, strlen(rack_slash) + 1);
        } else {
          memmove(rack_slash + 1, racks_end, strlen(racks_end) + 1);
        }
      }
    }
  }
  tui_copy_to_clipboard(cgp);
  free(cgp);
  snprintf(gs->notice_buf, sizeof(gs->notice_buf), "Copied CGP%s",
           conceal ? " (computer rack hidden)" : "");
  clock_gettime(CLOCK_MONOTONIC, &gs->notice_expires_at);
  gs->notice_expires_at.tv_sec += 2;
}

// Apply readline / emacs-style cursor and kill bindings to a
// multi-line text buffer. Returns true if the key was consumed,
// in which case the caller should continue past the rest of its
// input handling. "Line" means visual newline-bounded segment —
// for a single-line buffer (CGP) Ctrl-A goes to buffer start and
// Ctrl-E to buffer end. *dirty flips to true on any edit so the
// caller's live-parse trigger fires.
static bool tui_text_readline_key(uint32_t key, char *buf, int *cursor,
                                  int *len, bool *dirty) {
  switch (key) {
  case 0x01: { // Ctrl-A: beginning of current line
    int c = *cursor;
    while (c > 0 && buf[c - 1] != '\n') {
      c--;
    }
    *cursor = c;
    return true;
  }
  case 0x05: { // Ctrl-E: end of current line
    int c = *cursor;
    while (c < *len && buf[c] != '\n') {
      c++;
    }
    *cursor = c;
    return true;
  }
  case 0x02: // Ctrl-B: back one char
    if (*cursor > 0) {
      (*cursor)--;
    }
    return true;
  case 0x06: // Ctrl-F: forward one char
    if (*cursor < *len) {
      (*cursor)++;
    }
    return true;
  case 0x04: // Ctrl-D: delete forward
    if (*cursor < *len) {
      memmove(&buf[*cursor], &buf[*cursor + 1], (size_t)(*len - *cursor));
      (*len)--;
      *dirty = true;
    }
    return true;
  case 0x0b: { // Ctrl-K: kill from cursor to end of line
    int end = *cursor;
    while (end < *len && buf[end] != '\n') {
      end++;
    }
    if (end > *cursor) {
      memmove(&buf[*cursor], &buf[end], (size_t)(*len - end + 1));
      *len -= (end - *cursor);
      *dirty = true;
    }
    return true;
  }
  case 0x15: { // Ctrl-U: kill from beginning of line to cursor
    int beg = *cursor;
    while (beg > 0 && buf[beg - 1] != '\n') {
      beg--;
    }
    if (beg < *cursor) {
      const int drop = *cursor - beg;
      memmove(&buf[beg], &buf[*cursor], (size_t)(*len - *cursor + 1));
      *len -= drop;
      *cursor = beg;
      *dirty = true;
    }
    return true;
  }
  case 0x17: { // Ctrl-W: delete word backward
    int end = *cursor;
    int beg = end;
    while (beg > 0 && (buf[beg - 1] == ' ' || buf[beg - 1] == '\t' ||
                       buf[beg - 1] == '\n')) {
      beg--;
    }
    while (beg > 0 && buf[beg - 1] != ' ' && buf[beg - 1] != '\t' &&
           buf[beg - 1] != '\n') {
      beg--;
    }
    if (beg < end) {
      memmove(&buf[beg], &buf[end], (size_t)(*len - end + 1));
      *len -= (end - beg);
      *cursor = beg;
      *dirty = true;
    }
    return true;
  }
  default:
    return false;
  }
}

// Walk a GCG tile-placement word starting at (row, col) in `dir`
// (0 = across, 1 = down) and stamp every newly-placed tile's
// square with `player_idx` so the board renderer can color it
// correctly. Played-through tiles (marked '.' in the word) are
// skipped — they retain whichever player_idx the earlier event
// already wrote. Out-of-bounds coordinates terminate the walk.
static void tui_apply_gcg_move_owner(Board *board, int row, int col, int dir,
                                     const char *word, int player_idx) {
  int r = row;
  int c = col;
  for (const char *p = word; *p != '\0'; p++) {
    if (r < 0 || r >= BOARD_DIM || c < 0 || c >= BOARD_DIM) {
      break;
    }
    if (*p != '.') {
      board_set_square_owner(board, r, c, player_idx);
    }
    if (dir == 0) {
      c++;
    } else {
      r++;
    }
  }
}

// Persist the current edit buffers (move / rack / leave) into the
// entry the editor is focused on. ALWAYS saves — valid or not —
// so navigating away never loses typed input; revalidation then
// surfaces any error inline. Caller holds game_state.mutex.
static void tui_commit_edit_to_entry(TuiGameState *gs) {
  if (gs->edit_history_idx < 0 || gs->edit_history_idx >= gs->history_count) {
    return;
  }
  TuiHistoryEntry *e = &gs->history[gs->edit_history_idx];
  tui_game_state_parse_edit_buf(gs);
  switch (gs->edit_move_kind) {
  case TUI_EDIT_MOVE_KIND_PLACEMENT:
  case TUI_EDIT_MOVE_KIND_EXCHANGE:
  case TUI_EDIT_MOVE_KIND_PASS:
    // Engine-recognized move: store the canonical form ("-ABC"
    // display normalization for exchanges) and its score.
    if (strncmp(gs->edit_move_canonical, "ex ", 3) == 0) {
      snprintf(e->move_str, sizeof(e->move_str), "-%s",
               gs->edit_move_canonical + 3);
    } else {
      snprintf(e->move_str, sizeof(e->move_str), "%s", gs->edit_move_canonical);
    }
    e->score = gs->edit_move_score >= 0 ? gs->edit_move_score : 0;
    e->pending = false;
    break;
  default:
    // Partial / invalid / bare-word / empty. Persist the raw typed
    // text (so it survives the blur and revalidation can flag it),
    // or clear + revert to pending when the move field is empty.
    if (gs->edit_move_len > 0) {
      snprintf(e->move_str, sizeof(e->move_str), "%.*s", gs->edit_move_len,
               gs->edit_move_buf);
      e->pending = false;
    } else {
      e->move_str[0] = '\0';
      e->pending = true;
    }
    e->score = 0;
    break;
  }
  if (gs->edit_rack_len > 0) {
    snprintf(e->rack_str, sizeof(e->rack_str), "%.*s", gs->edit_rack_len,
             gs->edit_rack_buf);
  } else {
    e->rack_str[0] = '\0';
  }
  if (gs->edit_leave_len > 0) {
    snprintf(e->leave_str, sizeof(e->leave_str), "%.*s", gs->edit_leave_len,
             gs->edit_leave_buf);
  } else if (gs->edit_move_leave[0] != '\0') {
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", gs->edit_move_leave);
  } else {
    e->leave_str[0] = '\0';
  }
}

// Commit-on-blur + full revalidation. Call before any navigation
// that moves focus off the currently-edited turn.
static void tui_commit_edit_and_revalidate(TuiGameState *gs) {
  if (gs->edit_history_idx < 0) {
    return;
  }
  // Play-vs-computer: blur (Esc / turn-nav / click-away) ABANDONS the
  // typed text instead of committing it — moves only land through the
  // explicit PvC commit (Enter), which plays them for real. The
  // annotation commit would stamp unplayed text onto the live pending
  // entry, and revalidate_history's full no-draw replay rebuilds racks
  // from entry text — wiping the real bag-drawn racks (the "(no rack)"
  // pill + empty-racks CGP bug).
  if (gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    return;
  }
  tui_commit_edit_to_entry(gs);
  tui_game_state_revalidate_history(gs);
}

// Type-through: after a letter is typed in the MOVE field, if the
// play's word now butts up against existing board tiles, append
// those tiles' letters to the move buffer automatically — the
// annotator shouldn't have to retype letters already on the board.
// e.g. typing "1H QUI" with PIRATED's P sitting at K1 auto-extends
// the move to "1H QUIP" (the P is played-through) and parks the
// cursor past it, so the next typed letter lands on the first
// empty square. Operates on the engine board, which is seeked to
// this turn's pre-move position (holds every other turn's tiles).
// Caller holds the mutex and has just run parse_edit_buf.
static void tui_autofill_playthrough(TuiGameState *gs) {
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->game == NULL || gs->ld == NULL) {
    return;
  }
  const Move *pm = gs->edit_preview_move;
  if (move_get_type(pm) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const Board *brd = game_get_board(gs->game);
  if (brd == NULL) {
    return;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(pm));
  const int span = move_get_tiles_length(pm);
  int next_r =
      vertical ? move_get_row_start(pm) + span : move_get_row_start(pm);
  int next_c =
      vertical ? move_get_col_start(pm) : move_get_col_start(pm) + span;
  bool changed = false;
  while (next_r >= 0 && next_r < BOARD_DIM && next_c >= 0 &&
         next_c < BOARD_DIM) {
    const MachineLetter ml = board_get_letter(brd, next_r, next_c);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    // ld_ml_to_hl is already lowercase for blanked tiles, uppercase
    // otherwise — exactly the move-notation convention.
    const char *hl = gs->ld->ld_ml_to_hl[ml];
    if (hl == NULL || hl[0] == '\0') {
      break;
    }
    // The very first absorbed letter may arrive while the buffer is
    // still just the coord token (typing "13A" with a tile sitting ON
    // A13). Without the coord/word space the letter glues onto the
    // coord — "13A" became "13AV", which then poisoned every
    // subsequent parse of the move.
    const bool need_space = strchr(gs->edit_move_buf, ' ') == NULL;
    const int hlen = (int)strlen(hl);
    if (gs->edit_move_len + hlen + (need_space ? 1 : 0) >=
        (int)sizeof(gs->edit_move_buf)) {
      break;
    }
    if (need_space) {
      gs->edit_move_buf[gs->edit_move_len++] = ' ';
    }
    memcpy(gs->edit_move_buf + gs->edit_move_len, hl, (size_t)hlen);
    gs->edit_move_len += hlen;
    gs->edit_move_buf[gs->edit_move_len] = '\0';
    gs->edit_move_cursor = gs->edit_move_len;
    changed = true;
    if (vertical) {
      next_r++;
    } else {
      next_c++;
    }
  }
  if (changed) {
    tui_game_state_parse_edit_buf(gs);
  }
}

// ── Board move-entry builder ────────────────────────────────────────
// Board-driven move entry (click a square, type, arrows to navigate,
// backspace to retract, Enter to submit) reuses the annotation edit
// pipeline. The builder owns only the anchor + direction; the play's
// letters live in edit_move_buf's word token exactly as typed annotation
// stores them. Regenerating edit_move_buf and re-parsing keeps the board
// ghost, the on-board cursor arrow, the inferred rack, and the History
// cell all in sync for free.

// Format the coordinate token for the current anchor + direction. Across
// is "<row+1><col-letter>" (e.g. "8H"); down is "<col-letter><row+1>"
// (e.g. "H8") — the engine infers direction from the token order.
static void tui_board_builder_coord_token(const TuiGameState *gs, char *out,
                                          size_t out_cap) {
  const int row1 = gs->board_anchor_row + 1;
  const char col_letter = (char)('A' + gs->board_anchor_col);
  if (board_is_dir_vertical(gs->board_dir)) {
    snprintf(out, out_cap, "%c%d", col_letter, row1);
  } else {
    snprintf(out, out_cap, "%d%c", row1, col_letter);
  }
}

// Copy the word token (everything after the first space) of edit_move_buf
// into `out`. Empty string when no word has been typed yet.
static void tui_board_builder_extract_word(const TuiGameState *gs, char *out,
                                           size_t out_cap) {
  out[0] = '\0';
  const char *space = strchr(gs->edit_move_buf, ' ');
  if (space != NULL && space[1] != '\0') {
    snprintf(out, out_cap, "%s", space + 1);
  }
}

// A fresh anchor always starts ACROSS — matching Woogles and every
// mainstream Scrabble UI, and predictability beats cleverness here: an
// earlier "whichever direction has the longer empty run" heuristic
// meant the same click could anchor differently depending on nearby
// tiles, which read as random. Down is one toggle away (click the
// cell again, or Space/Tab).
static int tui_board_builder_default_dir(const TuiGameState *gs, int row,
                                         int col) {
  (void)gs;
  (void)row;
  (void)col;
  return BOARD_HORIZONTAL_DIRECTION;
}

// Begin (or relocate) board move-entry at origin (row, col) with the
// given direction. Opens the editor on the current pending entry so the
// cell + board pipeline lights up. Drops any previously-placed tiles.
// Caller holds the mutex.
static void tui_board_builder_set_anchor(TuiGameState *gs, int row, int col,
                                         int dir) {
  // The clicked cell is the ORIGIN — remembered so direction toggles
  // and arrow moves can re-derive everything from the user's cell. The
  // walked-back anchor below is a derived value.
  gs->board_origin_row = row;
  gs->board_origin_col = col;
  // Absorb any contiguous on-board tiles immediately BEFORE the clicked
  // cell (in the play direction) so a word that plays through them is
  // anchored at the true word start. e.g. clicking the empty cell right
  // after an existing "O" and typing "WNER" yields "OWNER" anchored at
  // the O's square, not the invalid "WNER" anchored after it.
  const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
  const bool vertical = board_is_dir_vertical(dir);
  int anchor_row = row;
  int anchor_col = col;
  while (brd != NULL) {
    const int pr = vertical ? anchor_row - 1 : anchor_row;
    const int pc = vertical ? anchor_col : anchor_col - 1;
    if (pr < 0 || pc < 0 ||
        board_get_letter(brd, pr, pc) == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    anchor_row = pr;
    anchor_col = pc;
  }
  gs->board_anchor_row = anchor_row;
  gs->board_anchor_col = anchor_col;
  gs->board_dir = dir;
  gs->board_entry_active = true;
  // The pending entry (last in history) is the turn being entered.
  if (gs->history_count > 0) {
    gs->edit_history_idx = gs->history_count - 1;
  }
  gs->edit_field = TUI_EDIT_FIELD_MOVE;
  gs->edit_rack_user_modified = false;
  gs->edit_leave_buf[0] = '\0';
  gs->edit_leave_len = 0;
  gs->edit_leave_cursor = 0;
  // Seed the word with the leading playthrough letters (from the true
  // anchor up to — but excluding — the clicked empty cell) so the typing
  // cursor starts on the clicked cell.
  char coord[8];
  tui_board_builder_coord_token(gs, coord, sizeof(coord));
  char leading[48];
  int li = 0;
  {
    int r = anchor_row;
    int c = anchor_col;
    while (!(r == row && c == col) && brd != NULL &&
           li < (int)sizeof(leading) - 4) {
      const MachineLetter ml = board_get_letter(brd, r, c);
      const char *hl = gs->ld != NULL ? gs->ld->ld_ml_to_hl[ml] : NULL;
      for (int k = 0;
           hl != NULL && hl[k] != '\0' && li < (int)sizeof(leading) - 1; k++) {
        leading[li++] = hl[k];
      }
      if (vertical) {
        r++;
      } else {
        c++;
      }
    }
  }
  leading[li] = '\0';
  if (li > 0) {
    snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s %s", coord,
             leading);
  } else {
    snprintf(gs->edit_move_buf, sizeof(gs->edit_move_buf), "%s", coord);
  }
  gs->edit_move_len = (int)strlen(gs->edit_move_buf);
  gs->edit_move_cursor = gs->edit_move_len;
  tui_game_state_parse_edit_buf(gs);
  tui_autofill_playthrough(gs);
}

// Cancel board move-entry: return placed tiles to the rack and exit edit
// mode. Caller holds the mutex.
static void tui_board_builder_cancel(TuiGameState *gs) {
  gs->board_entry_active = false;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  gs->edit_history_idx = -1;
  tui_game_state_parse_edit_buf(gs);
}

// Machine letter for a single uppercase A-Z letter, or 0 if not found.
static MachineLetter tui_ml_for_upper(const TuiGameState *gs, char up) {
  if (gs->ld == NULL) {
    return 0;
  }
  for (MachineLetter ml = 1; ml < MACHINE_LETTER_MAX_VALUE; ml++) {
    const char *hl = gs->ld->ld_ml_to_hl[ml];
    if (hl != NULL && hl[0] == up && hl[1] == '\0') {
      return ml;
    }
  }
  return 0;
}

// Type one tile letter into the board move-entry word. Casing mirrors the
// history-cell MOVE editor: Shift+letter designates a blank (stored
// lowercase). In play-vs-computer the human's rack is known, so a plain
// letter that isn't in the rack — but for which a blank is available — is
// treated as a blank without Shift. Appends to the word token, re-parses,
// and absorbs any played-through board tiles. Caller holds the mutex.
// Resolve a typed letter against the human's live rack in
// play-vs-computer: real tile, blank, or rejected. The rack is finite
// and known — only allow a tile the rack still has AFTER accounting for
// tiles the in-progress move already placed (so you can't type a second
// E when you hold one E; the played-through E on the board is not yours
// to retype). Unshifted letters fall back to a blank when the real tile
// is exhausted but a blank remains; Shift+letter explicitly requests a
// blank. Returns false to reject the keystroke (neither available).
// Caller holds the mutex.
static bool tui_pvc_resolve_typed_letter(const TuiGameState *gs, char up,
                                         bool shift, bool *out_blank) {
  const MachineLetter ml = tui_ml_for_upper(gs, up);
  const Rack *rack =
      gs->game != NULL
          ? player_get_rack(game_get_player(gs->game, gs->human_player_idx))
          : NULL;
  int placed_real = 0;
  int placed_blank = 0;
  for (const char *p = gs->edit_move_inferred_rack; *p != '\0'; p++) {
    if (*p == up) {
      placed_real++;
    } else if (*p == '?') {
      placed_blank++;
    }
  }
  const int avail_real = (rack != NULL && ml != 0)
                             ? (int)rack_get_letter(rack, ml) - placed_real
                             : 0;
  const int avail_blank =
      rack != NULL
          ? (int)rack_get_letter(rack, BLANK_MACHINE_LETTER) - placed_blank
          : 0;
  if (shift) {
    if (avail_blank <= 0) {
      return false; // explicit blank requested but none left
    }
    *out_blank = true;
  } else if (avail_real > 0) {
    *out_blank = false;
  } else if (avail_blank > 0) {
    *out_blank = true; // out of the real tile — use a blank
  } else {
    return false; // no real tile and no blank — reject the keystroke
  }
  return true;
}

// For the cell editor's MOVE buffer, compute the board square the next
// typed WORD letter would land on: the coord token's square advanced
// along the play direction by the number of word letters before the
// cursor (played-through letters already absorbed into the buffer count,
// which is what keeps the position aligned with the board). Returns
// false when the buffer has no parseable coord token yet.
static bool tui_cell_word_landing_square(const TuiGameState *gs, int *out_row,
                                         int *out_col) {
  const char *buf = gs->edit_move_buf;
  const char *space = strchr(buf, ' ');
  if (space == NULL) {
    return false;
  }
  // Coord token: digits-then-letter = horizontal; letter-then-digits =
  // vertical. Same convention as parse_coord_token / the CGP notation.
  int row = -1;
  int col = -1;
  bool vertical = false;
  const char c0 = buf[0];
  if (c0 >= '0' && c0 <= '9') {
    int digits = 0;
    const char *p = buf;
    while (*p >= '0' && *p <= '9') {
      digits = digits * 10 + (*p - '0');
      p++;
    }
    if (!((*p >= 'A' && *p <= 'O') || (*p >= 'a' && *p <= 'o'))) {
      return false;
    }
    row = digits - 1;
    col = (*p >= 'a') ? *p - 'a' : *p - 'A';
    vertical = false;
  } else if ((c0 >= 'A' && c0 <= 'O') || (c0 >= 'a' && c0 <= 'o')) {
    col = (c0 >= 'a') ? c0 - 'a' : c0 - 'A';
    int digits = 0;
    const char *p = buf + 1;
    while (*p >= '0' && *p <= '9') {
      digits = digits * 10 + (*p - '0');
      p++;
    }
    if (digits == 0) {
      return false;
    }
    row = digits - 1;
    vertical = true;
  } else {
    return false;
  }
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM) {
    return false;
  }
  // Count word letters before the cursor.
  int word_letters = 0;
  const int word_start = (int)(space - buf) + 1;
  for (int i = word_start; i < gs->edit_move_cursor && buf[i] != '\0'; i++) {
    const char wc = buf[i];
    if ((wc >= 'a' && wc <= 'z') || (wc >= 'A' && wc <= 'Z')) {
      word_letters++;
    }
  }
  *out_row = vertical ? row + word_letters : row;
  *out_col = vertical ? col : col + word_letters;
  return *out_row < BOARD_DIM && *out_col < BOARD_DIM;
}

static void tui_board_entry_type_letter(TuiGameState *gs, char ch, bool shift,
                                        bool is_pvc) {
  if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
    return;
  }
  const char up = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
  bool blank = shift; // Shift+letter is always a blank.
  if (is_pvc && !tui_pvc_resolve_typed_letter(gs, up, shift, &blank)) {
    return;
  }
  const char glyph = blank ? (char)(up - 'A' + 'a') : up;
  // Insert a space before the first word character (buffer is just the
  // coord token after anchoring).
  const bool need_space = strchr(gs->edit_move_buf, ' ') == NULL;
  const int extra = need_space ? 2 : 1;
  if (gs->edit_move_len + extra >= (int)sizeof(gs->edit_move_buf)) {
    return;
  }
  if (need_space) {
    gs->edit_move_buf[gs->edit_move_len++] = ' ';
  }
  gs->edit_move_buf[gs->edit_move_len++] = glyph;
  gs->edit_move_buf[gs->edit_move_len] = '\0';
  gs->edit_move_cursor = gs->edit_move_len;
  tui_game_state_parse_edit_buf(gs);
  tui_autofill_playthrough(gs);
}

// Toggle the move-builder's direction, re-anchoring from the ORIGIN
// (the cell the user clicked) so leading playthrough re-derives for the
// new direction. The word token can't simply be carried over: it mixes
// the user's placed tiles with absorbed played-through board letters
// from the OLD direction, which are meaningless in the new one — that
// was the "direction is stuck horizontal after playthrough" bug. So:
// extract the user's tiles (skip word letters sitting on occupied
// squares along the old direction), re-anchor, and retype them; the
// typing path re-absorbs whatever the new direction plays through.
// Caller holds the mutex.
static void tui_board_builder_toggle_dir(TuiGameState *gs) {
  const Board *brd = gs->game != NULL ? game_get_board(gs->game) : NULL;
  char word[64];
  tui_board_builder_extract_word(gs, word, sizeof(word));
  char user_tiles[64];
  int n_user = 0;
  const bool old_vertical = board_is_dir_vertical(gs->board_dir);
  int r = gs->board_anchor_row;
  int c = gs->board_anchor_col;
  for (int i = 0; word[i] != '\0' && n_user < (int)sizeof(user_tiles) - 1;) {
    const bool on_board =
        brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
        board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
    if (on_board) {
      // Played-through square: its human-readable letter may span
      // multiple word chars — skip them all.
      const MachineLetter ml = board_get_letter(brd, r, c);
      const char *hl = gs->ld != NULL ? gs->ld->ld_ml_to_hl[ml] : NULL;
      int skip = hl != NULL ? (int)strlen(hl) : 1;
      while (skip-- > 0 && word[i] != '\0') {
        i++;
      }
    } else {
      user_tiles[n_user++] = word[i++];
    }
    if (old_vertical) {
      r++;
    } else {
      c++;
    }
  }
  user_tiles[n_user] = '\0';
  const int new_dir =
      old_vertical ? BOARD_HORIZONTAL_DIRECTION : BOARD_VERTICAL_DIRECTION;
  tui_board_builder_set_anchor(gs, gs->board_origin_row, gs->board_origin_col,
                               new_dir);
  // Retype the user's tiles so they re-place along the new direction
  // (lowercase in the buffer = blank, retyped as Shift+letter).
  const bool is_pvc = gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER;
  for (int i = 0; i < n_user; i++) {
    const char ch = user_tiles[i];
    const bool was_blank = ch >= 'a' && ch <= 'z';
    const char up = was_blank ? (char)(ch - 'a' + 'A') : ch;
    tui_board_entry_type_letter(gs, up, was_blank, is_pvc);
  }
}

// Retract the last placed tile from the board move-entry word, skipping
// back over any trailing played-through (board) tiles. With nothing
// placed, step the anchor back one cell. Caller holds the mutex.
static void tui_board_entry_backspace(TuiGameState *gs) {
  const char *space = strchr(gs->edit_move_buf, ' ');
  const int word_off =
      space != NULL ? (int)(space - gs->edit_move_buf) + 1 : -1;
  int word_len = (word_off >= 0) ? gs->edit_move_len - word_off : 0;
  if (word_len > 0) {
    const Board *brd = game_get_board(gs->game);
    const bool vertical = board_is_dir_vertical(gs->board_dir);
    // Each word char maps to cell anchor + index along the direction.
    // Pop trailing played-through cells (occupied on the board), then
    // pop one placed tile.
    while (word_len > 0) {
      const int r = gs->board_anchor_row + (vertical ? (word_len - 1) : 0);
      const int c = gs->board_anchor_col + (vertical ? 0 : (word_len - 1));
      const bool occupied =
          brd != NULL && r >= 0 && r < BOARD_DIM && c >= 0 && c < BOARD_DIM &&
          board_get_letter(brd, r, c) != ALPHABET_EMPTY_SQUARE_MARKER;
      word_len--; // drop this char
      if (!occupied) {
        break; // it was a placed tile — stop here
      }
    }
    if (word_len <= 0) {
      gs->edit_move_buf[word_off - 1] = '\0'; // drop the space too
      gs->edit_move_len = word_off - 1;
    } else {
      gs->edit_move_buf[word_off + word_len] = '\0';
      gs->edit_move_len = word_off + word_len;
    }
    gs->edit_move_cursor = gs->edit_move_len;
    tui_game_state_parse_edit_buf(gs);
    tui_autofill_playthrough(gs);
  } else {
    // Nothing placed: step the ORIGIN back one cell (the walked-back
    // anchor re-derives; stepping the anchor itself got pinned against
    // leading playthrough).
    int nr = gs->board_origin_row;
    int nc = gs->board_origin_col;
    if (board_is_dir_vertical(gs->board_dir)) {
      if (nr > 0) {
        nr--;
      }
    } else if (nc > 0) {
      nc--;
    }
    tui_board_builder_set_anchor(gs, nr, nc, gs->board_dir);
  }
}

// Tag placed-tile squares with the player's index so the renderer colors
// them. Mirrors the bot worker's owner-tagging loop.
static void tui_tag_move_owners(Board *board, const Move *move,
                                int player_idx) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int dir = move_get_dir(move);
  int r = move_get_row_start(move);
  int c = move_get_col_start(move);
  const int n = move_get_tiles_length(move);
  for (int t = 0; t < n; t++) {
    if (move_get_tile(move, t) != PLAYED_THROUGH_MARKER) {
      board_set_square_owner(board, r, c, player_idx);
    }
    if (board_is_dir_vertical(dir)) {
      r++;
    } else {
      c++;
    }
  }
}

// Commit the current edit-buffer preview move as the human's play in a
// live play-vs-computer game: play with drawing (real bag-backed rack),
// finalize the pending history entry, charge the clock, and clear the
// editor so the bot's next poll sees the computer on turn. Shared by the
// board-entry Enter and the history-cell Enter — the cell is an alternate
// move-entry surface in play-vs-computer ("8D WORD" typed directly).
// Returns true when a move was committed. Caller holds the mutex.
static bool tui_pvc_commit_preview_move(TuiGameState *gs) {
  if (gs->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER || gs->game == NULL ||
      game_over(gs->game)) {
    return false;
  }
  tui_game_state_parse_edit_buf(gs);
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->edit_move_score < 0 ||
      gs->edit_move_kind != TUI_EDIT_MOVE_KIND_PLACEMENT) {
    return false; // not a legal placement yet — keep editing
  }
  const int idx = gs->edit_history_idx;
  if (idx < 0 || idx >= gs->history_count) {
    return false;
  }
  TuiHistoryEntry *e = &gs->history[idx];
  const int player_idx = e->player_idx;
  // Only the human's own live pending turn is committable. Without the
  // player/on-turn guards, opening the COMPUTER's pending entry (it exists
  // while the bot is thinking) and pressing Enter would play a move as the
  // computer and race the bot thread's own commit.
  if (!e->pending || player_idx != gs->human_player_idx ||
      game_get_player_on_turn_index(gs->game) != gs->human_player_idx) {
    return false;
  }

  Rack leave;
  rack_set_dist_size(&leave, ld_get_size(gs->ld));
  play_move(gs->edit_preview_move, gs->game, &leave);
  tui_tag_move_owners(game_get_board(gs->game), gs->edit_preview_move,
                      player_idx);
  snprintf(e->move_str, sizeof(e->move_str), "%s", gs->edit_move_canonical);
  e->score = gs->edit_move_score;
  {
    StringBuilder *sb = string_builder_create();
    string_builder_add_rack(sb, &leave, gs->ld, false);
    char *dump = string_builder_dump(sb, NULL);
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", dump);
    free(dump);
    string_builder_destroy(sb);
  }
  const int post =
      equity_to_int(player_get_score(game_get_player(gs->game, player_idx)));
  int bonus = 0;
  if (game_over(gs->game)) {
    const Rack *opp =
        player_get_rack(game_get_player(gs->game, 1 - player_idx));
    if (opp != NULL && !rack_is_empty(opp)) {
      bonus = equity_to_int(calculate_end_rack_points(opp, gs->ld));
      e->end_bonus = bonus;
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, opp, gs->ld, false);
      char *dump = string_builder_dump(sb, NULL);
      snprintf(e->end_rack_str, sizeof(e->end_rack_str), "%s", dump);
      free(dump);
      string_builder_destroy(sb);
    }
  }
  e->total_after = post - bonus;
  e->pending = false;
  // Charge wall time to the human, clamping clock skew to 0.
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed = (double)(now.tv_sec - gs->turn_started.tv_sec) +
                   (double)(now.tv_nsec - gs->turn_started.tv_nsec) / 1e9;
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }
  gs->seconds_used[player_idx] += elapsed;
  gs->turn_started = now;
  e->clock_at_end =
      gs->time_per_side_seconds - (int)gs->seconds_used[player_idx];
  gs->board_entry_active = false;
  gs->edit_history_idx = -1;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  if (game_over(gs->game) && gs->history_count > 0) {
    gs->history_cursor = gs->history_count - 1;
    gs->analysis_cursor = 0;
  } else {
    // Keep following the live game so the next (computer) turn shows.
    gs->history_cursor = -1;
  }
  tui_game_state_parse_edit_buf(gs);
  atomic_fetch_add(&gs->render_version, 1);
  return true;
}

// Submit the in-progress board move. Play-vs-computer plays the move with
// drawing (real game) and hands the turn to the bot; annotation plays it
// without drawing and seeds the next pending turn. No-op unless the
// buffer is a legal placement. Caller holds the mutex.
static void tui_board_entry_submit(TuiGameState *gs) {
  if (!gs->board_entry_active) {
    return;
  }
  if (gs->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    tui_pvc_commit_preview_move(gs);
    return;
  }
  tui_game_state_parse_edit_buf(gs);
  if (!gs->edit_preview_move_valid || gs->edit_preview_move == NULL ||
      gs->edit_move_score < 0 ||
      gs->edit_move_kind != TUI_EDIT_MOVE_KIND_PLACEMENT) {
    return; // not a legal placement yet — keep editing
  }
  const int idx = gs->edit_history_idx;
  if (idx < 0 || idx >= gs->history_count) {
    return;
  }
  const int player_idx = gs->history[idx].player_idx;
  TuiHistoryEntry *e = &gs->history[idx];

  // Annotation: place tiles without drawing; the annotator owns racks.
  // Mirrors the history-cell RACK-Enter commit (no revalidate — the
  // incremental engine state advanced by play_move_without_drawing_tiles
  // is authoritative for a forward move).
  play_move_without_drawing_tiles(gs->edit_preview_move, gs->game);
  tui_tag_move_owners(game_get_board(gs->game), gs->edit_preview_move,
                      player_idx);
  snprintf(e->move_str, sizeof(e->move_str), "%s", gs->edit_move_canonical);
  e->score = gs->edit_move_score;
  // Seed the rack from the move's played tiles when the annotator
  // hasn't typed a fuller rack — matches the cell editor's behavior.
  if (e->rack_str[0] == '\0' && gs->edit_move_inferred_rack[0] != '\0') {
    snprintf(e->rack_str, sizeof(e->rack_str), "%s",
             gs->edit_move_inferred_rack);
  }
  if (gs->edit_move_leave[0] != '\0') {
    snprintf(e->leave_str, sizeof(e->leave_str), "%s", gs->edit_move_leave);
  }
  e->pending = false;
  e->total_after =
      equity_to_int(player_get_score(game_get_player(gs->game, player_idx)));
  const int next_player = game_get_player_on_turn_index(gs->game);
  if (idx + 1 >= gs->history_count) {
    tui_bot_worker_append_pending_history(gs, next_player, NULL,
                                          gs->time_per_side_seconds);
  }
  gs->board_entry_active = false;
  gs->edit_history_idx = -1;
  gs->edit_move_buf[0] = '\0';
  gs->edit_move_len = 0;
  gs->edit_move_cursor = 0;
  gs->history_cursor = gs->history_count - 1;
  tui_game_state_parse_edit_buf(gs);
  atomic_fetch_add(&gs->render_version, 1);
}

int main(int argc, char *argv[]) {
  const CliArgs args = parse_args(argc, argv);
  if (args.error) {
    return 2;
  }
  if (args.show_help) {
    print_usage();
    return 0;
  }

  // Redirect config load/save to the --config path (if given) before any
  // config access below. No-op when NULL.
  tui_config_set_path_override(args.config_path);

  setlocale(LC_ALL, "");

  // Redirect stderr to a known file BEFORE notcurses takes over the
  // TTY. notcurses puts the terminal in alt-screen mode, so any
  // `log_fatal` message sent to stderr gets blitted under the TUI
  // and is invisible by the time the crash handler restores the
  // screen. Capturing to a file gives us a paper trail for the
  // engine's last-words message. Best-effort: a failure here is
  // not fatal (the TUI still runs, we just lose the breadcrumb).
  freopen("/tmp/magpie_stderr.log", "w", stderr);
  // Unbuffer stderr so engine log_fatal output reaches disk before
  // abort().
  setvbuf(stderr, NULL, _IONBF, 0);

  notcurses_options opts = {
      // NO_QUIT_SIGHANDLERS prevents notcurses from installing its
      // own SIGABRT/SIGSEGV/etc handlers during init — we install our
      // own immediately below and want them to be the ONLY thing on
      // the signal, otherwise notcurses's chained handler eats the
      // crash before our backtrace dump runs.
      .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_QUIT_SIGHANDLERS,
  };
  struct notcurses *nc = notcurses_core_init(&opts, NULL);
  if (nc == NULL) {
    return 1;
  }
  g_crash_nc = nc;
  install_crash_handlers();
  // Hard-disable scrolling on the std plane: macOS Terminal can otherwise
  // scroll the alt screen when render coords overflow the visible area
  // mid-resize, and that scroll is irreversible.
  ncplane_set_scrolling(notcurses_stdplane(nc), false);
  // Enable mouse-button reports. With NCMICE_BUTTON_EVENT the input
  // stream now delivers NCTYPE_PRESS / NCTYPE_RELEASE / NCSCROLL_*
  // records alongside keystrokes. No handlers wired yet — this is a
  // pure smoke-test of what enabling does to the terminal's feel
  // (notably: text selection now requires the platform modifier,
  // typically Option/Cmd on macOS).
  // BUTTON_EVENT gives press/release for left/right/wheel; DRAG_EVENT
  // adds motion-while-held so the Analysis scrollbar thumb can be
  // dragged smoothly across the track.
  const unsigned mice_eventmask = NCMICE_BUTTON_EVENT | NCMICE_DRAG_EVENT;
  notcurses_mice_enable(nc, mice_eventmask);
  bool mouse_enabled = true;

  // Ask the terminal to report focus-in / focus-out events
  // (xterm DEC mode 1004). When the terminal loses focus — e.g.
  // because the user pressed Cmd+Shift+4 to start a screenshot
  // — we'll temporarily disable mouse reporting so macOS's
  // window server isn't fighting us for cursor capture, which
  // is what causes the screenshot UI's appearance to lag.
  // Notcurses doesn't surface focus events directly, so the
  // terminal's CSI I / CSI O sequences get replayed as raw
  // bytes (Esc, '[', 'I' / 'O') through notcurses_get; the
  // input drain loop watches for that pattern.
  (void)!write(STDOUT_FILENO, "\x1b[?1004h", 8);

  // Load saved settings (if any), then resolve each in order: theme first
  // (it controls the picker palette for the others), then lexicon, then
  // time. Any picker may be skipped if the value is already known and
  // --reconfigure was not passed.
  TuiConfig loaded = {0};
  const bool config_existed = !args.no_config && tui_config_load(&loaded);
  TuiConfig to_save = loaded;
  bool should_save = false;

  // THEME ---------------------------------------------------------------
  ThemeName chosen_theme = THEME_DARK;
  if (args.theme_arg != NULL) {
    chosen_theme = theme_get_by_id(args.theme_arg)->name;
  } else if (config_existed && loaded.theme_set && !args.reconfigure) {
    chosen_theme = loaded.theme;
  } else {
    const ThemeName initial =
        loaded.theme_set ? loaded.theme : theme_auto_detect(nc);
    chosen_theme = tui_onboarding_run(nc, initial);
    to_save.theme = chosen_theme;
    to_save.theme_set = true;
    should_save = true;
  }
  const Theme *theme = theme_get(chosen_theme);

  // LEXICON -------------------------------------------------------------
  char chosen_lexicon[TUI_LEXICON_NAME_MAX];
  chosen_lexicon[0] = '\0';
  if (config_existed && loaded.lexicon_set && !args.reconfigure) {
    strncpy(chosen_lexicon, loaded.lexicon, sizeof(chosen_lexicon) - 1);
    chosen_lexicon[sizeof(chosen_lexicon) - 1] = '\0';
  } else {
    const char *initial = loaded.lexicon_set ? loaded.lexicon : NULL;
    if (!tui_lexicon_picker_run(nc, theme, initial, chosen_lexicon,
                                sizeof(chosen_lexicon))) {
      notcurses_stop(nc);
      return 0;
    }
    strncpy(to_save.lexicon, chosen_lexicon, sizeof(to_save.lexicon) - 1);
    to_save.lexicon[sizeof(to_save.lexicon) - 1] = '\0';
    to_save.lexicon_set = true;
    should_save = true;
  }

  // TIME ----------------------------------------------------------------
  int chosen_time = 0;
  if (config_existed && loaded.time_per_side_set && !args.reconfigure) {
    chosen_time = loaded.time_per_side_seconds;
  } else {
    const int initial =
        loaded.time_per_side_set ? loaded.time_per_side_seconds : 0;
    chosen_time = tui_time_picker_run(nc, theme, initial);
    if (chosen_time < 0) {
      notcurses_stop(nc);
      return 0;
    }
    to_save.time_per_side_seconds = chosen_time;
    to_save.time_per_side_set = true;
    should_save = true;
  }

  if (should_save && !args.no_config) {
    tui_config_save(&to_save);
  }

  struct ncplane *std_plane = notcurses_stdplane(nc);

  // Initialize the game with the chosen lexicon. Static so it
  // lives in BSS rather than on main()'s stack — sizeof(TuiGameState)
  // is in the tens-of-MB range once ANALYSIS_ROW_CAP * TUI_HISTORY_MAX
  // saved-snapshot rows are accounted for, which would overflow
  // the default 8 MB pthread stack on macOS.
  static TuiGameState game_state = {0};
  char init_error[256] = {0};
  const uint64_t seed = (uint64_t)time(NULL);
  const bool initial_load_rit = loaded.load_rit_set ? loaded.load_rit : false;
  if (!tui_game_state_init(chosen_lexicon, seed, initial_load_rit, &game_state,
                           init_error, sizeof(init_error))) {
    render_init_error(std_plane, theme, chosen_lexicon, init_error);
    notcurses_render(nc);
    ncinput input;
    notcurses_get(nc, NULL, &input);
    notcurses_stop(nc);
    return 1;
  }

  tui_game_state_set_time_per_side(&game_state, chosen_time);
  // Pixel-grid border thickness: from config when present, else default 2.
  game_state.border_thickness =
      loaded.border_thickness_set ? loaded.border_thickness : 2;
  game_state.blank_uppercase =
      loaded.blank_uppercase_set ? loaded.blank_uppercase : true;
  game_state.premium_labels = loaded.premium_labels_set
                                  ? loaded.premium_labels
                                  : TUI_PREMIUM_LABELS_UPPERCASE;
  game_state.board_scale = loaded.board_scale_set ? loaded.board_scale : 1;
  game_state.antialias = loaded.antialias_set ? loaded.antialias : true;
  game_state.score_subscripts = loaded.score_subscripts_set
                                    ? loaded.score_subscripts
                                    : TUI_SCORE_SUBSCRIPTS_OFF;
  game_state.rack_sort =
      loaded.rack_sort_set ? loaded.rack_sort : TUI_RACK_SORT_ALPHA;
  const bool pixel_supported = notcurses_canpixel(nc);
  const bool font_available = game_state.glyph_cache != NULL;
  // If the user's saved scale=2 can't be honored on this machine, fall
  // back transparently rather than show a broken board. The user's
  // saved preference stays in tui.toml for next time.
  if (game_state.board_scale >= 2 && (!pixel_supported || !font_available)) {
    game_state.board_scale = 1;
  }
  // Bot worker stays idle at launch — the startup menu picks the
  // game mode, and the time picker that follows ("Watch computer
  // play") is what actually fires off the first game. The pixel
  // worker is plumbing for rendering, unrelated to game-state, so
  // it can start right away.
  tui_pixel_worker_start(&game_state);

  // Modal state: which (if any) modal is open. Drives keyboard routing
  // and the status-bar control hints.
  bool running = true;

  // Focus-event detection state. The terminal sends CSI I on
  // focus-in and CSI O on focus-out when DEC mode 1004 is on
  // (which we enabled above). Notcurses doesn't surface focus
  // events directly — it replays the bytes through the input
  // queue as ESC, '[', 'I' or 'O'. This 3-state machine watches
  // for that pattern. `focus_pending_esc` is set when the
  // sequence aborted with just an ESC buffered, so the next
  // drain pass can deliver a real Esc keypress that the modal
  // handlers expect. Mouse mode is auto-disabled on focus-out
  // and re-enabled on focus-in so the macOS screenshot UI
  // doesn't fight the terminal for cursor capture.
  int focus_state = 0; // 0 = normal, 1 = saw ESC, 2 = saw ESC '['
  bool focus_pending_esc = false;
  // First-launch experience: show the startup menu before any game
  // gets played. Picking "Watch computer play" routes through the
  // time picker and resumes the bot-vs-bot flow that used to be
  // the default. Other modes (load position, load game, annotate,
  // play vs computer) are dimmed in the menu until wired.
  //
  // --watch on the command line skips the menu and starts a fresh
  // watch game using the saved (or default) settings. Useful for
  // debugging — reattaching with lldb on each crash without having
  // to click through the menu first.
  TuiModalState modal = TUI_MODAL_STARTUP_MENU;
  if (args.watch) {
    pthread_mutex_lock(&game_state.mutex);
    tui_game_state_set_time_per_side(&game_state, chosen_time);
    tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
    pthread_mutex_unlock(&game_state.mutex);
    tui_bot_worker_start(&game_state);
    modal = TUI_MODAL_NONE;
  }
  int startup_menu_focus = TUI_STARTUP_WATCH;
  int main_menu_focus = 0;
  int settings_focus = 0;
  // Where to return when Esc is pressed inside the Settings modal.
  // Reached from the main menu → return to the menu so the user can
  // pick another entry. Reached from the command-bar S → return to
  // no modal, since that's where the user was.
  TuiModalState settings_return = TUI_MODAL_MAIN_MENU;
  int time_focus = 0;
  // Where Esc inside the time picker should return to. Reached
  // from the main menu's New Game → MAIN_MENU; reached via the
  // command bar's N / /new → NONE.
  TuiModalState time_picker_return = TUI_MODAL_MAIN_MENU;
  // Where Esc inside the startup menu should return to. At app
  // launch there's no prior modal to return to (Esc just dismisses
  // it). When the user opens it via Esc → New game we want Esc to
  // step back to the main menu.
  TuiModalState startup_menu_return = TUI_MODAL_NONE;
  // Watch-setup modal: row focus, pre-set to "Start game" so Enter
  // on first open kicks off the bot game with the displayed
  // defaults (time / lexicon / sim params).
  int watch_setup_focus = TUI_WATCH_SETUP_START;
  // Watch setup runs on its own copy of the lexicon + time control
  // so adjusters can preview without mutating the live session
  // settings. Initialized when the modal opens; committed to
  // chosen_lexicon / chosen_time only when the user hits "Start
  // game". Esc closes the modal and the locals are abandoned.
  char watch_setup_lexicon[TUI_LEXICON_NAME_MAX] = "";
  int watch_setup_time = 0;
  // Play-vs-computer setup: editable player names, who moves first, and
  // the focused row / name caret. Defaults focus to Start so a quick
  // Enter launches with the defaults.
  int play_setup_focus = TUI_PLAY_SETUP_START;
  char play_setup_human_name[32] = "You";
  char play_setup_computer_name[32] = "Computer";
  int play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
  int play_setup_name_cursor = 0;
  // Annotate setup: lexicon (◀/▶ cycled, language-scoped) plus
  // two free-form player names. Same modal-local pattern as
  // Watch setup — commits to the session only on Start.
  char annotate_setup_lexicon[TUI_LEXICON_NAME_MAX] = "";
  char annotate_setup_p1_name[32] = "";
  char annotate_setup_p2_name[32] = "";
  int annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
  // Caret position within the currently-focused name field, in
  // bytes. Bounded by the name's strlen(); shared between P1 and
  // P2 because only one name row is focused at a time.
  int annotate_setup_name_cursor = 0;
  // Load-position modal state. Buffer holds the user-entered text
  // (raw CGP or a dragged file path); cursor is the byte offset
  // of the insertion point. The position is parsed live whenever
  // the buffer changes — `dirty` triggers a parse at the top of
  // the next frame; `parse_ok` records the last parse result so
  // Enter can fire only when the CGP is loadable. `error_msg`
  // displays the last parse / file error inside the modal until
  // the next edit clears it.
  char load_position_buf[2048] = {0};
  int load_position_len = 0;
  int load_position_cursor = 0;
  bool load_position_dirty = false;
  bool load_position_parse_ok = false;
  char load_position_error[160] = {0};
  // Load-game modal state. Mirrors the load-position modal but
  // holds a multi-line GCG game record. GCGs are typically much
  // bigger than CGPs (a 25-turn record can run several KB), so
  // the buffer is correspondingly larger.
  char load_game_buf[16384] = {0};
  int load_game_len = 0;
  int load_game_cursor = 0;
  bool load_game_dirty = false;
  bool load_game_parse_ok = false;
  char load_game_error[160] = {0};
  // Width of the input area's wrap column — matches the modal's
  // interior width so Up/Down arrow can walk visual rows.
  enum { LOAD_POSITION_WRAP_W = 73 };
  // Quit-confirmation modal: focus tracks Yes/No (0 = No, 1 = Yes),
  // default No since it's the safer option. quit_confirm_return is
  // the modal to return to when the user picks No / hits Esc; the
  // caller (main menu Q or command-bar Q) sets this before opening.
  int quit_confirm_focus = 0;
  TuiModalState quit_confirm_return = TUI_MODAL_NONE;
  // Modal-style lexicon picker state. Lazily allocated when the user
  // enters the modal; destroyed before exit.
  LexiconList *lexicon_list = NULL;
  int lexicon_focus = 0;
  // Settings rows for Antialias / Subscript / Border are hidden when
  // the board isn't rendering at 2x — they're 2x-only settings. The
  // renderer in game_render.c filters identically; this predicate
  // mirrors it so up/down navigation can skip past hidden rows.
  // Forward-declared lambda style: we capture the relevant state by
  // re-evaluating each call rather than threading args in.
#define SETTINGS_2X_ONLY(idx)                                                  \
  ((idx) == TUI_SETTINGS_AA || (idx) == TUI_SETTINGS_SUBSCRIPTS ||             \
   (idx) == TUI_SETTINGS_BORDER)
  // Frame-pacing anchor: at the top of every iteration we sleep until
  // next_frame_deadline, then advance the deadline by FRAME_NS. Sitting
  // at the top means the various `continue` paths below can't bypass
  // the throttle the way they did with a bottom-of-loop sleep.
  struct timespec next_frame_deadline;
  clock_gettime(CLOCK_MONOTONIC, &next_frame_deadline);
  // Conditional-render bookkeeping. The 2x pixel board is drawn as ~225
  // per-cell sprixels; notcurses_render re-emits them on every frame
  // even when nothing changed, which pins a static screen (e.g. waiting
  // on the human in play-vs-computer) at a few fps and burns CPU. So we
  // only run the render path when something actually changed: input was
  // processed, the bot bumped render_version, a modal is open, the
  // wall-clock second ticked (live clock countdown), or the bot is
  // animating a spinner. Otherwise the last frame stays on screen
  // untouched.
  bool frame_dirty = true; // render the first frame
  // Input→display latency probe: timestamp when input first dirtied the
  // current (not-yet-rendered) frame, so we can measure keypress-to-pixels.
  struct timespec input_dirty_ts = {0, 0};
  bool input_dirty_pending = false;
  uint64_t rendered_version = ~(uint64_t)0;
  long rendered_wall_sec = -1;
  while (running) {
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long remaining_ns =
          (long)(next_frame_deadline.tv_sec - now.tv_sec) * 1000000000L +
          (long)(next_frame_deadline.tv_nsec - now.tv_nsec);
      if (remaining_ns > 0) {
        // On schedule — sleep the rest of the budget and advance the
        // deadline from where it was, so we hit a consistent 60fps.
        struct timespec sleep_ts = {.tv_sec = remaining_ns / 1000000000L,
                                    .tv_nsec = remaining_ns % 1000000000L};
        nanosleep(&sleep_ts, NULL);
        next_frame_deadline.tv_nsec += FRAME_NS;
      } else {
        // Last render exceeded FRAME_NS (typical when a bot play
        // invalidates the pixel-composite cache). Don't try to catch up
        // — re-anchor the deadline at now + FRAME_NS so subsequent
        // frames are paced from this late-but-current point. Otherwise
        // we'd sprint a few unthrottled frames until we caught up,
        // showing up as 200+fps spikes in the EMA on every move.
        next_frame_deadline = now;
        next_frame_deadline.tv_nsec += FRAME_NS;
      }
      if (next_frame_deadline.tv_nsec >= 1000000000L) {
        next_frame_deadline.tv_sec += next_frame_deadline.tv_nsec / 1000000000L;
        next_frame_deadline.tv_nsec %= 1000000000L;
      }
    }
    // Live-preview parse for the Load-position modal. The buffer
    // dirty flag is set by edits inside the modal's input handler;
    // we run the parse here (once per frame, regardless of how
    // many keys arrived in the burst) so the board / racks behind
    // the modal reflect the latest CGP text and Enter has a
    // definitive parse_ok flag to consult.
    if (modal == TUI_MODAL_LOAD_POSITION && load_position_dirty) {
      load_position_dirty = false;
      char working[2048];
      snprintf(working, sizeof(working), "%s", load_position_buf);
      // Strip leading + trailing whitespace.
      char *start = working;
      while (*start == ' ' || *start == '\t' || *start == '\n' ||
             *start == '\r') {
        start++;
      }
      size_t wlen = strlen(start);
      while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                          start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
        start[--wlen] = '\0';
      }
      // Strip surrounding quotes (terminals wrap dragged paths).
      if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                        (start[0] == '\'' && start[wlen - 1] == '\''))) {
        start[wlen - 1] = '\0';
        start++;
        wlen -= 2;
      }
      if (strncmp(start, "file://", 7) == 0) {
        start += 7;
        wlen -= 7;
      }
      // Heuristic for path vs raw CGP.
      bool looks_like_path =
          wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                       (wlen > 4 && strcmp(start + wlen - 4, ".cgp") == 0));
      if (looks_like_path) {
        for (size_t i = 0; i < wlen; i++) {
          if (start[i] == ' ' || start[i] == '\t' || start[i] == '\n') {
            looks_like_path = false;
            break;
          }
        }
      }
      char cgp_payload[4096];
      cgp_payload[0] = '\0';
      bool resolve_ok = true;
      if (wlen == 0) {
        load_position_error[0] = '\0';
        resolve_ok = false;
      } else if (looks_like_path) {
        char path[1024];
        if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
          const char *home = getenv("HOME");
          if (home != NULL) {
            snprintf(path, sizeof(path), "%s%s", home,
                     start[1] == '\0' ? "" : start + 1);
          } else {
            snprintf(path, sizeof(path), "%s", start);
          }
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
          snprintf(load_position_error, sizeof(load_position_error),
                   "Cannot open %s", path);
          resolve_ok = false;
        } else {
          size_t n = fread(cgp_payload, 1, sizeof(cgp_payload) - 1, fp);
          fclose(fp);
          cgp_payload[n] = '\0';
        }
      } else {
        snprintf(cgp_payload, sizeof(cgp_payload), "%s", start);
      }
      if (resolve_ok) {
        // Stop the bot if a previous load started one (currently
        // we never start the bot on load, but be safe).
        if (game_state.bot_started) {
          atomic_store(&game_state.bot_stop, true);
          pthread_join(game_state.bot_thread, NULL);
          game_state.bot_started = false;
          atomic_store(&game_state.bot_stop, false);
        }
        ErrorStack *err = error_stack_create();
        pthread_mutex_lock(&game_state.mutex);
        game_load_cgp(game_state.game, cgp_payload, err);
        const bool ok = error_stack_is_empty(err);
        if (ok) {
          // On success, reset per-turn history / cursors so the
          // panels reflect a fresh starting state for the loaded
          // position.
          for (int i = 0; i < game_state.history_count; i++) {
            TuiHistoryEntry *e = &game_state.history[i];
            if (e->board_before != NULL) {
              board_destroy(e->board_before);
              e->board_before = NULL;
            }
            if (e->rack_before != NULL) {
              rack_destroy(e->rack_before);
              e->rack_before = NULL;
            }
            if (e->opp_rack_before != NULL) {
              rack_destroy(e->opp_rack_before);
              e->opp_rack_before = NULL;
            }
            if (e->sim_results_saved != NULL) {
              sim_results_destroy(e->sim_results_saved);
              e->sim_results_saved = NULL;
            }
            if (e->endgame_moves_saved != NULL) {
              free(e->endgame_moves_saved);
              e->endgame_moves_saved = NULL;
              e->endgame_moves_saved_count = 0;
            }
            if (e->loaded_move != NULL) {
              free(e->loaded_move);
              e->loaded_move = NULL;
            }
          }
          game_state.history_count = 0;
          game_state.history_cursor = -1;
          game_state.analysis_cursor = -1;
          game_state.analysis_cursor_column = 0;
          game_state.analysis_anchored_move[0] = '\0';
          game_state.seconds_used[0] = 0.0;
          game_state.seconds_used[1] = 0.0;
          clock_gettime(CLOCK_MONOTONIC, &game_state.turn_started);
          // Seed a pending history entry for the upcoming turn so
          // the History panel shows "1." waiting for input, with
          // the on-turn player's rack snapshotted and clocks
          // reset to full. This matches how the bot worker
          // appends a pending entry at the start of every turn —
          // the user is now "the bot" deciding what comes next.
          const int on_turn = game_get_player_on_turn_index(game_state.game);
          const Rack *on_turn_rack =
              player_get_rack(game_get_player(game_state.game, on_turn));
          tui_bot_worker_append_pending_history(
              &game_state, on_turn, on_turn_rack,
              game_state.time_per_side_seconds);
          load_position_error[0] = '\0';
          load_position_parse_ok = true;
        } else {
          char *msg = error_stack_get_string_and_reset(err);
          snprintf(load_position_error, sizeof(load_position_error), "%s",
                   msg != NULL ? msg : "Parse error");
          free(msg);
          load_position_parse_ok = false;
        }
        pthread_mutex_unlock(&game_state.mutex);
        error_stack_destroy(err);
      } else {
        load_position_parse_ok = false;
      }
    }

    // Live-preview parse for the LOAD_GAME modal. Same shape as
    // LOAD_POSITION's parse pass: trim / unquote / strip file://,
    // decide path-vs-raw, read the file if needed, then feed the
    // GCG through gcg_parser_create + parse_gcg_settings +
    // parse_gcg_events. The events parser internally resets the
    // game and replays moves, so on success the game ends in
    // its final-state position.
    if (modal == TUI_MODAL_LOAD_GAME && load_game_dirty) {
      load_game_dirty = false;
      // Working buffer big enough to copy the entire load buffer.
      // GCGs can be several KB; we keep this on the stack but
      // sized to the modal buffer.
      static char working[sizeof(load_game_buf)];
      snprintf(working, sizeof(working), "%s", load_game_buf);
      char *start = working;
      while (*start == ' ' || *start == '\t' || *start == '\n' ||
             *start == '\r') {
        start++;
      }
      size_t wlen = strlen(start);
      while (wlen > 0 && (start[wlen - 1] == ' ' || start[wlen - 1] == '\t' ||
                          start[wlen - 1] == '\n' || start[wlen - 1] == '\r')) {
        start[--wlen] = '\0';
      }
      if (wlen >= 2 && ((start[0] == '"' && start[wlen - 1] == '"') ||
                        (start[0] == '\'' && start[wlen - 1] == '\''))) {
        start[wlen - 1] = '\0';
        start++;
        wlen -= 2;
      }
      if (strncmp(start, "file://", 7) == 0) {
        start += 7;
        wlen -= 7;
      }
      bool looks_like_path =
          wlen > 0 && (start[0] == '/' || start[0] == '~' ||
                       (wlen > 4 && strcmp(start + wlen - 4, ".gcg") == 0));
      if (looks_like_path) {
        // A path can't contain embedded newlines. Multi-line raw
        // GCGs will always fail this check.
        for (size_t i = 0; i < wlen; i++) {
          if (start[i] == '\n') {
            looks_like_path = false;
            break;
          }
        }
      }
      // GCG payload buffer — GCGs can be sizable (a long game
      // with notes can run into the tens of KB), so allocate
      // generously on the heap rather than blowing the stack.
      char *gcg_payload = NULL;
      bool resolve_ok = true;
      if (wlen == 0) {
        load_game_error[0] = '\0';
        resolve_ok = false;
      } else if (looks_like_path) {
        char path[1024];
        if (start[0] == '~' && (start[1] == '/' || start[1] == '\0')) {
          const char *home = getenv("HOME");
          if (home != NULL) {
            snprintf(path, sizeof(path), "%s%s", home,
                     start[1] == '\0' ? "" : start + 1);
          } else {
            snprintf(path, sizeof(path), "%s", start);
          }
        } else {
          snprintf(path, sizeof(path), "%s", start);
        }
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) {
          snprintf(load_game_error, sizeof(load_game_error), "Cannot open %s",
                   path);
          resolve_ok = false;
        } else {
          fseek(fp, 0, SEEK_END);
          long fsize = ftell(fp);
          fseek(fp, 0, SEEK_SET);
          if (fsize < 0 || fsize > (long)(1 << 20)) {
            // Cap at 1 MiB — anything bigger is almost certainly
            // not a real GCG.
            snprintf(load_game_error, sizeof(load_game_error),
                     "%s is too large", path);
            fclose(fp);
            resolve_ok = false;
          } else {
            gcg_payload = malloc((size_t)fsize + 1);
            size_t n = fread(gcg_payload, 1, (size_t)fsize, fp);
            fclose(fp);
            gcg_payload[n] = '\0';
          }
        }
      } else {
        gcg_payload = malloc(wlen + 1);
        memcpy(gcg_payload, start, wlen);
        gcg_payload[wlen] = '\0';
      }
      // Strip a trailing incomplete `>nickname: rack` line.
      // Quackle and cross-tables.com emit this as a hint about the
      // on-turn player's rack at the time the GCG was exported,
      // but it is not a valid GCG event — MAGPIE's strict parser
      // rejects the whole file otherwise. We just truncate it
      // here; loading the events that precede it is what the user
      // actually wants.
      if (resolve_ok && gcg_payload != NULL) {
        size_t plen = strlen(gcg_payload);
        while (plen > 0 &&
               (gcg_payload[plen - 1] == '\n' ||
                gcg_payload[plen - 1] == '\r' || gcg_payload[plen - 1] == ' ' ||
                gcg_payload[plen - 1] == '\t')) {
          gcg_payload[--plen] = '\0';
        }
        size_t line_start = plen;
        while (line_start > 0 && gcg_payload[line_start - 1] != '\n') {
          line_start--;
        }
        if (line_start < plen && gcg_payload[line_start] == '>') {
          int tokens = 0;
          bool in_token = false;
          for (size_t i = line_start; i < plen; i++) {
            const char c = gcg_payload[i];
            const bool ws = c == ' ' || c == '\t';
            if (!ws && !in_token) {
              tokens++;
              in_token = true;
            } else if (ws) {
              in_token = false;
            }
          }
          // Real GCG event lines have at least 4 whitespace-
          // separated tokens (the end-rack-points form). Anything
          // shorter is the trailing rack hint.
          if (tokens < 4) {
            gcg_payload[line_start] = '\0';
          }
        }
      }
      if (resolve_ok) {
        if (game_state.bot_started) {
          atomic_store(&game_state.bot_stop, true);
          pthread_join(game_state.bot_thread, NULL);
          game_state.bot_started = false;
          atomic_store(&game_state.bot_stop, false);
        }
        ErrorStack *err = error_stack_create();
        GameHistory *history = game_history_create();
        GCGParser *parser = gcg_parser_create(gcg_payload, history,
                                              game_state.active_lexicon, err);
        bool ok = error_stack_is_empty(err);
        if (ok) {
          parse_gcg_settings(parser, err);
          ok = error_stack_is_empty(err);
        }
        pthread_mutex_lock(&game_state.mutex);
        if (ok) {
          parse_gcg_events(parser, game_state.game, err);
          ok = error_stack_is_empty(err);
        }
        if (ok) {
          for (int i = 0; i < game_state.history_count; i++) {
            TuiHistoryEntry *e = &game_state.history[i];
            if (e->board_before != NULL) {
              board_destroy(e->board_before);
              e->board_before = NULL;
            }
            if (e->rack_before != NULL) {
              rack_destroy(e->rack_before);
              e->rack_before = NULL;
            }
            if (e->opp_rack_before != NULL) {
              rack_destroy(e->opp_rack_before);
              e->opp_rack_before = NULL;
            }
            if (e->sim_results_saved != NULL) {
              sim_results_destroy(e->sim_results_saved);
              e->sim_results_saved = NULL;
            }
            if (e->endgame_moves_saved != NULL) {
              free(e->endgame_moves_saved);
              e->endgame_moves_saved = NULL;
              e->endgame_moves_saved_count = 0;
            }
            if (e->loaded_move != NULL) {
              free(e->loaded_move);
              e->loaded_move = NULL;
            }
          }
          game_state.history_count = 0;
          game_state.history_cursor = -1;
          game_state.analysis_cursor = -1;
          game_state.analysis_cursor_column = 0;
          game_state.analysis_anchored_move[0] = '\0';
          game_state.seconds_used[0] = 0.0;
          game_state.seconds_used[1] = 0.0;
          clock_gettime(CLOCK_MONOTONIC, &game_state.turn_started);

          // Wipe analysis state left over from any prior session in
          // this process. Without this, loading a GCG after watching
          // a Magpie-vs-Magpie game leaves the previous game's
          // endgame leaderboard and sim plays stranded in the
          // Analysis panel. The structs are owned by the
          // TuiGameState, so destroying + recreating is the safest
          // way to drop their internal data without needing a
          // MoveList for sim_results_reset.
          tui_endgame_snapshot_clear(&game_state.endgame_snapshot);
          atomic_store(&game_state.endgame_results_active, false);
          atomic_store(&game_state.endgame_results_turn_idx, -1);
          if (game_state.sim_results != NULL) {
            sim_results_destroy(game_state.sim_results);
          }
          game_state.sim_results = sim_results_create(0.005);
          atomic_store(&game_state.sim_results_active, false);
          atomic_store(&game_state.sim_results_turn_idx, -1);

          // Surface real player names from the GCG so the pill
          // headers read "Quackle" / "New Player 1" instead of
          // the generic "P1" / "P2". Empty when not set.
          for (int p = 0; p < 2; p++) {
            const char *pname = game_history_player_get_name(history, p);
            if (pname != NULL) {
              snprintf(game_state.player_names[p],
                       sizeof(game_state.player_names[p]), "%s", pname);
            } else {
              game_state.player_names[p][0] = '\0';
            }
          }

          // Walk the parsed events and append a TuiHistoryEntry
          // per move so the History panel reflects the whole
          // game. The walker runs in three passes:
          //   1. Text-field pass — build each entry's move/score/
          //      rack strings using the game at its final post-
          //      replay state (so '.' playthrough markers can be
          //      resolved against the final board). Parsed
          //      position / direction / word are stashed into
          //      `entry_meta` for the later passes.
          //   2. Snapshot pass — for each entry, replay events
          //      [0..k) to recover the state immediately before
          //      that move, duplicate the board, apply owner
          //      stamps for the prior events' tiles, and snapshot
          //      racks from event metadata. Quadratic in number
          //      of moves but n is tiny so it doesn't matter.
          //   3. Restore-final pass — replay all events to put
          //      the live game back at its final state, then
          //      stamp owners on the live board so the cursor-
          //      off-history view still colors correctly.
          typedef struct {
            int engine_idx;
            int player_idx;
            int row;
            int col;
            int dir; // 0 = across, 1 = down
            char word[80];
            bool has_position;
          } GcgEntryMeta;
          static GcgEntryMeta entry_meta[TUI_HISTORY_MAX];
          int meta_count = 0;

          const int num_events = game_history_get_num_events(history);
          // ── Pass 1: text fields ─────────────────────────────────────
          for (int evi = 0;
               evi < num_events && game_state.history_count < TUI_HISTORY_MAX;
               evi++) {
            GameEvent *event = game_history_get_event(history, evi);
            const game_event_t etype = game_event_get_type(event);
            if (etype != GAME_EVENT_TILE_PLACEMENT_MOVE &&
                etype != GAME_EVENT_PASS && etype != GAME_EVENT_EXCHANGE) {
              continue;
            }
            const int player_idx = game_event_get_player_index(event);
            const Equity move_eq = game_event_get_move_score(event);
            const Equity cume_eq = game_event_get_cumulative_score(event);
            const char *cgp_move = game_event_get_cgp_move_string(event);
            const Rack *rack = game_event_get_const_rack(event);
            TuiHistoryEntry *entry =
                &game_state.history[game_state.history_count++];
            memset(entry, 0, sizeof(*entry));
            entry->player_idx = player_idx;
            entry->pending = false;
            entry->score = (move_eq != EQUITY_UNDEFINED_VALUE &&
                            move_eq != EQUITY_INITIAL_VALUE &&
                            move_eq != EQUITY_PASS_VALUE)
                               ? equity_to_int(move_eq)
                               : 0;
            entry->total_after = (cume_eq != EQUITY_UNDEFINED_VALUE &&
                                  cume_eq != EQUITY_INITIAL_VALUE &&
                                  cume_eq != EQUITY_PASS_VALUE)
                                     ? equity_to_int(cume_eq)
                                     : 0;
            entry->clock_at_start = game_state.time_per_side_seconds;
            entry->opp_clock_at_start = game_state.time_per_side_seconds;

            GcgEntryMeta *meta = &entry_meta[meta_count++];
            meta->engine_idx = evi;
            meta->player_idx = player_idx;
            meta->has_position = false;
            meta->word[0] = '\0';

            // Snapshot the engine's validated move into the entry so
            // the board renderer can ghost the played tiles when
            // the cursor lands on the "Plays" row in the Analysis
            // panel. parse_gcg_events ran with validate=true so
            // every tile-placement event has its vms populated;
            // we deep-copy out of it since the GameHistory will be
            // destroyed before the TuiHistoryEntry is.
            const ValidatedMoves *vms = game_event_get_vms(event);
            if (vms != NULL && validated_moves_get_number_of_moves(vms) > 0) {
              const Move *src = validated_moves_get_move(vms, 0);
              if (src != NULL) {
                entry->loaded_move = move_create();
                move_copy(entry->loaded_move, src);
              }
            }

            if (cgp_move != NULL) {
              // Reformat GCG moves into the compact form the rest
              // of the TUI expects: exchanges → "-AAU", and tile
              // placements get each '.' resolved to "(L)" using
              // the final board. Newly-played blanks stay
              // lowercase — render_move_styled handles the
              // bold/dim distinction.
              if (strncmp(cgp_move, "ex ", 3) == 0) {
                snprintf(entry->move_str, sizeof(entry->move_str), "-%s",
                         cgp_move + 3);
              } else if (strncmp(cgp_move, "(exch ", 6) == 0) {
                const char *close_paren = strchr(cgp_move, ')');
                if (close_paren != NULL) {
                  const int letters_len = (int)(close_paren - (cgp_move + 6));
                  char tmp[sizeof(entry->move_str)];
                  tmp[0] = '-';
                  const int cap = (int)sizeof(tmp) - 2;
                  const int copy = letters_len < cap ? letters_len : cap;
                  memcpy(tmp + 1, cgp_move + 6, (size_t)copy);
                  tmp[1 + copy] = '\0';
                  snprintf(entry->move_str, sizeof(entry->move_str), "%s", tmp);
                } else {
                  snprintf(entry->move_str, sizeof(entry->move_str), "%s",
                           cgp_move);
                }
              } else {
                const char *m = cgp_move;
                int row = -1;
                int col = -1;
                int dir = 0;
                if (*m >= '0' && *m <= '9') {
                  int r = 0;
                  while (*m >= '0' && *m <= '9') {
                    r = r * 10 + (*m - '0');
                    m++;
                  }
                  if (*m >= 'A' && *m <= 'A' + BOARD_DIM - 1) {
                    col = *m - 'A';
                    m++;
                  }
                  row = r - 1;
                  dir = 0;
                } else if (*m >= 'A' && *m <= 'A' + BOARD_DIM - 1) {
                  col = *m - 'A';
                  m++;
                  int r = 0;
                  while (*m >= '0' && *m <= '9') {
                    r = r * 10 + (*m - '0');
                    m++;
                  }
                  row = r - 1;
                  dir = 1;
                }
                if (row >= 0 && row < BOARD_DIM && col >= 0 &&
                    col < BOARD_DIM && *m == ' ') {
                  meta->row = row;
                  meta->col = col;
                  meta->dir = dir;
                  meta->has_position = true;
                  snprintf(meta->word, sizeof(meta->word), "%s", m + 1);

                  char buf[80];
                  const size_t prefix_len = (size_t)(m - cgp_move) + 1;
                  if (prefix_len < sizeof(buf)) {
                    memcpy(buf, cgp_move, prefix_len);
                    size_t out = prefix_len;
                    m++;
                    int r = row;
                    int c = col;
                    const Board *board = game_get_board(game_state.game);
                    while (*m != '\0' && out + 5 < sizeof(buf) &&
                           r < BOARD_DIM && c < BOARD_DIM) {
                      if (*m == '.') {
                        const MachineLetter ml = board_get_letter(board, r, c);
                        const char *hl = ml != ALPHABET_EMPTY_SQUARE_MARKER
                                             ? game_state.ld->ld_ml_to_hl[ml]
                                             : ".";
                        out += (size_t)snprintf(buf + out, sizeof(buf) - out,
                                                "(%s)", hl);
                      } else {
                        buf[out++] = *m;
                      }
                      m++;
                      if (dir == 0) {
                        c++;
                      } else {
                        r++;
                      }
                    }
                    buf[out] = '\0';
                    snprintf(entry->move_str, sizeof(entry->move_str), "%s",
                             buf);
                  } else {
                    snprintf(entry->move_str, sizeof(entry->move_str), "%s",
                             cgp_move);
                  }
                } else {
                  snprintf(entry->move_str, sizeof(entry->move_str), "%s",
                           cgp_move);
                }
              }
            }
            if (rack != NULL && !rack_is_empty(rack)) {
              StringBuilder *rsb = string_builder_create();
              string_builder_add_rack(rsb, rack, game_state.ld, false);
              char *rack_dump = string_builder_dump(rsb, NULL);
              if (rack_dump != NULL) {
                snprintf(entry->rack_str, sizeof(entry->rack_str), "%s",
                         rack_dump);
                free(rack_dump);
              }
              string_builder_destroy(rsb);

              // Leave = rack − tiles played by this event. Mirrors
              // what the bot-worker stashes for live turns and lets
              // the Sim/Analysis panel show "move · leave · score"
              // when the user is reviewing a loaded GCG and no sim
              // ran for the turn.
              Rack *leave = rack_duplicate(rack);
              if (etype == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                  meta->has_position) {
                for (const char *p = meta->word; *p != '\0'; p++) {
                  if (*p == '.') {
                    continue;
                  }
                  MachineLetter ml = 0;
                  if (*p >= 'A' && *p <= 'Z') {
                    ml = (MachineLetter)(*p - 'A' + 1);
                  } else if (*p >= 'a' && *p <= 'z') {
                    ml = 0; // newly played blank
                  } else {
                    continue;
                  }
                  if (rack_get_letter(leave, ml) > 0) {
                    rack_take_letter(leave, ml);
                  }
                }
              } else if (etype == GAME_EVENT_EXCHANGE && cgp_move != NULL) {
                const char *t = NULL;
                if (strncmp(cgp_move, "ex ", 3) == 0) {
                  t = cgp_move + 3;
                } else if (strncmp(cgp_move, "(exch ", 6) == 0) {
                  t = cgp_move + 6;
                } else if (cgp_move[0] == '-') {
                  t = cgp_move + 1;
                }
                if (t != NULL) {
                  for (; *t != '\0' && *t != ')'; t++) {
                    MachineLetter ml = 0;
                    if (*t >= 'A' && *t <= 'Z') {
                      ml = (MachineLetter)(*t - 'A' + 1);
                    } else if (*t == '?' || (*t >= 'a' && *t <= 'z')) {
                      ml = 0;
                    } else {
                      continue;
                    }
                    if (rack_get_letter(leave, ml) > 0) {
                      rack_take_letter(leave, ml);
                    }
                  }
                }
              }
              // GAME_EVENT_PASS: leave is unchanged from rack.
              if (!rack_is_empty(leave)) {
                StringBuilder *lsb = string_builder_create();
                string_builder_add_rack(lsb, leave, game_state.ld, false);
                char *leave_dump = string_builder_dump(lsb, NULL);
                if (leave_dump != NULL) {
                  snprintf(entry->leave_str, sizeof(entry->leave_str), "%s",
                           leave_dump);
                  free(leave_dump);
                }
                string_builder_destroy(lsb);
              }
              rack_destroy(leave);
            }
          }

          // ── Pass 2: per-turn snapshots via incremental replay ────────
          ErrorStack *replay_err = error_stack_create();
          for (int k = 0; k < meta_count; k++) {
            const GcgEntryMeta *meta = &entry_meta[k];
            game_play_n_events(history, game_state.game, meta->engine_idx,
                               false, replay_err);
            if (!error_stack_is_empty(replay_err)) {
              error_stack_reset(replay_err);
              break;
            }
            TuiHistoryEntry *entry = &game_state.history[k];
            Board *board_snap =
                board_duplicate(game_get_board(game_state.game));
            // Stamp owners onto the snapshot for every move that
            // played before this one.
            for (int prior = 0; prior < k; prior++) {
              const GcgEntryMeta *pm = &entry_meta[prior];
              if (pm->has_position) {
                tui_apply_gcg_move_owner(board_snap, pm->row, pm->col, pm->dir,
                                         pm->word, pm->player_idx);
              }
            }
            entry->board_before = board_snap;

            // Pre-move rack snapshot from the event's own rack
            // field (more reliable than the engine's mid-replay
            // game state, which can have stale or empty racks
            // depending on which sub-step we stopped at).
            GameEvent *event =
                game_history_get_event(history, meta->engine_idx);
            const Rack *evt_rack = game_event_get_const_rack(event);
            if (evt_rack != NULL && !rack_is_empty(evt_rack)) {
              entry->rack_before = rack_duplicate(evt_rack);
            }
            // Opponent's rack at this moment is approximated by
            // the rack the opponent had at THEIR next move (they
            // hadn't drawn anything between the two turns).
            for (int next = meta->engine_idx + 1; next < num_events; next++) {
              GameEvent *nev = game_history_get_event(history, next);
              if (game_event_get_player_index(nev) == (1 - meta->player_idx)) {
                const Rack *nrack = game_event_get_const_rack(nev);
                if (nrack != NULL && !rack_is_empty(nrack)) {
                  entry->opp_rack_before = rack_duplicate(nrack);
                }
                break;
              }
            }
          }

          // ── Pass 3: restore final state, stamp live-board owners ─────
          game_play_n_events(history, game_state.game, num_events, false,
                             replay_err);
          error_stack_reset(replay_err);
          error_stack_destroy(replay_err);
          {
            Board *live_board = game_get_board(game_state.game);
            for (int k = 0; k < meta_count; k++) {
              const GcgEntryMeta *pm = &entry_meta[k];
              if (pm->has_position) {
                tui_apply_gcg_move_owner(live_board, pm->row, pm->col, pm->dir,
                                         pm->word, pm->player_idx);
              }
            }
          }

          // Land the user on turn 1 so the loaded game opens with
          // the first move highlighted and that turn's pre-move
          // board / rack already visible.
          if (game_state.history_count > 0) {
            game_state.history_cursor = 0;
          }

          load_game_error[0] = '\0';
          load_game_parse_ok = true;
        } else {
          char *msg = error_stack_get_string_and_reset(err);
          snprintf(load_game_error, sizeof(load_game_error), "%s",
                   msg != NULL ? msg : "Parse error");
          free(msg);
          load_game_parse_ok = false;
        }
        pthread_mutex_unlock(&game_state.mutex);
        gcg_parser_destroy(parser);
        game_history_destroy(history);
        error_stack_destroy(err);
      } else {
        load_game_parse_ok = false;
      }
      free(gcg_payload);
    }

    // Decide whether this frame needs a render at all (see the
    // conditional-render note above the loop). Skipping the render path
    // on an unchanged frame avoids re-emitting the ~225-sprixel 2x board
    // every tick, which is what pinned a static screen at a few fps.
    struct timespec render_now;
    clock_gettime(CLOCK_MONOTONIC, &render_now);
    const uint64_t cur_render_version = atomic_load(&game_state.render_version);
    bool bot_animating = false;
    pthread_mutex_lock(&game_state.mutex);
    if (game_state.history_count > 0) {
      const TuiHistoryEntry *last_entry =
          &game_state.history[game_state.history_count - 1];
      // A pending bot turn shows an animated spinner — keep rendering so
      // it animates. The human's own pending turn has no spinner, so it
      // doesn't force renders (that's the static idle case we optimize).
      bot_animating = last_entry->pending &&
                      !(game_state.app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
                        last_entry->player_idx == game_state.human_player_idx);
    }
    pthread_mutex_unlock(&game_state.mutex);
    const bool need_render = frame_dirty || modal != TUI_MODAL_NONE ||
                             cur_render_version != rendered_version ||
                             render_now.tv_sec != rendered_wall_sec ||
                             bot_animating;
    if (need_render) {
      // Time the FULL render path (cell composition + pixel ncblits AND
      // the notcurses_render emit) so the fps readout reflects the real
      // per-frame cost, not just the graphics emit. The mutex wait is
      // timed separately for the perf trace — a slow frame whose time is
      // all lock_us means contention with the bot worker, not rendering.
      struct timespec render_begin;
      clock_gettime(CLOCK_MONOTONIC, &render_begin);
      pthread_mutex_lock(&game_state.mutex);
      struct timespec lock_acquired;
      clock_gettime(CLOCK_MONOTONIC, &lock_acquired);
      const long lock_us =
          (long)(lock_acquired.tv_sec - render_begin.tv_sec) * 1000000L +
          (long)(lock_acquired.tv_nsec - render_begin.tv_nsec) / 1000L;
      tui_game_render(std_plane, theme, &game_state, chosen_time, modal);
      pthread_mutex_unlock(&game_state.mutex);
      if (modal == TUI_MODAL_MAIN_MENU) {
        tui_game_render_menu(std_plane, theme, main_menu_focus);
      } else if (modal == TUI_MODAL_SETTINGS) {
        const char *current_lexicon =
            to_save.lexicon_set ? to_save.lexicon : chosen_lexicon;
        const bool current_load_rit =
            to_save.load_rit_set ? to_save.load_rit : initial_load_rit;
        tui_game_render_settings(
            std_plane, theme, settings_focus, game_state.board_scale,
            game_state.antialias, game_state.score_subscripts,
            game_state.border_thickness, pixel_supported, font_available,
            game_state.premium_labels, game_state.blank_uppercase,
            game_state.rack_sort, current_lexicon, current_load_rit);
      } else if (modal == TUI_MODAL_TIME_PICKER) {
        tui_game_render_time_picker(std_plane, theme, time_focus);
      } else if (modal == TUI_MODAL_LEXICON_PICKER && lexicon_list != NULL) {
        tui_game_render_lexicon_picker(std_plane, theme, lexicon_list,
                                       lexicon_focus);
      } else if (modal == TUI_MODAL_QUIT_CONFIRM) {
        tui_game_render_quit_confirm(std_plane, theme, quit_confirm_focus);
      } else if (modal == TUI_MODAL_STARTUP_MENU) {
        tui_game_render_startup_menu(std_plane, theme, startup_menu_focus);
      } else if (modal == TUI_MODAL_WATCH_SETUP) {
        // Render from the modal's own local copy of lexicon + time
        // so adjusters preview against the in-modal value, not the
        // live session value.
        if (lexicon_list == NULL) {
          lexicon_list = tui_lexicon_list_load();
        }
        char lang_buf[32] = "(unknown)";
        if (lexicon_list != NULL) {
          const int idx =
              tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
          if (idx >= 0) {
            tui_lexicon_list_language_name(lexicon_list, idx, lang_buf,
                                           sizeof(lang_buf));
          }
        }
        tui_game_render_watch_setup(std_plane, theme, watch_setup_focus,
                                    watch_setup_time, lang_buf,
                                    watch_setup_lexicon, game_state.sim_plies,
                                    game_state.sim_candidates);
      } else if (modal == TUI_MODAL_LOAD_POSITION) {
        tui_game_render_load_position(std_plane, theme, load_position_buf,
                                      load_position_cursor,
                                      load_position_error);
      } else if (modal == TUI_MODAL_LOAD_GAME) {
        tui_game_render_load_game(std_plane, theme, load_game_buf,
                                  load_game_cursor, load_game_error);
      } else if (modal == TUI_MODAL_ANNOTATE_SETUP) {
        tui_game_render_annotate_setup(
            std_plane, theme, annotate_setup_focus, annotate_setup_lexicon,
            annotate_setup_p1_name, annotate_setup_p2_name,
            annotate_setup_name_cursor);
      } else if (modal == TUI_MODAL_PLAY_SETUP) {
        if (lexicon_list == NULL) {
          lexicon_list = tui_lexicon_list_load();
        }
        char play_lang_buf[32] = "(unknown)";
        if (lexicon_list != NULL) {
          const int idx =
              tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
          if (idx >= 0) {
            tui_lexicon_list_language_name(lexicon_list, idx, play_lang_buf,
                                           sizeof(play_lang_buf));
          }
        }
        tui_game_render_play_setup(
            std_plane, theme, play_setup_focus, play_setup_human_name,
            play_setup_computer_name, play_setup_first_move,
            play_setup_name_cursor, watch_setup_time, play_lang_buf,
            watch_setup_lexicon, game_state.sim_plies,
            game_state.sim_candidates);
      }
      // Time the UI thread's full render path so the debug overlay
      // can surface the worst-case frame in the last second. Captures
      // notcurses_render too, where the Kitty graphics emit lives.
      extern void tui_debug_record_frame_us(long);
      extern void tui_debug_record_sprixel_stats(uint64_t emits,
                                                 uint64_t elides);
      struct timespec frame_start;
      clock_gettime(CLOCK_MONOTONIC, &frame_start);
      notcurses_render(nc);
      struct timespec frame_end;
      clock_gettime(CLOCK_MONOTONIC, &frame_end);
      // Full render time (compose + blit + emit) drives the fps readout.
      const long frame_us =
          (long)(frame_end.tv_sec - render_begin.tv_sec) * 1000000L +
          (long)(frame_end.tv_nsec - render_begin.tv_nsec) / 1000L;
      // notcurses_render (graphics emit) time, for the perf trace only.
      const long emit_us =
          (long)(frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
          (long)(frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
      tui_debug_record_frame_us(frame_us);
      // Keypress-to-pixels latency: from when input first dirtied this
      // frame to when its render finished. -1 when this render wasn't
      // triggered by input (e.g. a clock tick).
      long input_lag_us = -1;
      if (input_dirty_pending) {
        input_lag_us =
            (long)(frame_end.tv_sec - input_dirty_ts.tv_sec) * 1000000L +
            (long)(frame_end.tv_nsec - input_dirty_ts.tv_nsec) / 1000L;
        input_dirty_pending = false;
      }
      // Publish the latest measured keypress latency to the status bar.
      // Only update on input-triggered frames so the last value persists
      // (clock-tick frames carry no latency and would otherwise blank it).
      if (input_lag_us >= 0) {
        extern void tui_debug_set_input_lag_us(long us);
        tui_debug_set_input_lag_us(input_lag_us);
      }
      // Snapshot notcurses' sprixel emission counters so the debug
      // overlay can show whether re-emits happen on idle frames.
      {
        ncstats *st = notcurses_stats_alloc(nc);
        if (st != NULL) {
          notcurses_stats(nc, st);
          tui_debug_record_sprixel_stats(st->sprixelemissions,
                                         st->sprixelelisions);
          // Opt-in perf trace (MAGPIE_FPS_DEBUG=1) — logged to
          // /tmp/magpie_stderr.log. For each rendered frame: notcurses_render
          // wall time, sprixels emitted vs elided this frame (high emit = the
          // board planes are NOT eliding), and board tile blits this frame.
          if (getenv("MAGPIE_FPS_DEBUG") != NULL) {
            static uint64_t dbg_emit;
            static uint64_t dbg_elide;
            extern int tui_debug_last_tile_blits(void);
            extern unsigned long tui_debug_tile_invalidations(void);
            extern unsigned long tui_debug_rack_blits(void);
            extern unsigned long tui_debug_glyph_rasters(void);
            static unsigned long dbg_rack;
            static unsigned long dbg_rasters;
            const unsigned long cur_rack = tui_debug_rack_blits();
            const unsigned long cur_rasters = tui_debug_glyph_rasters();
            fprintf(stderr,
                    "[fps] full_us=%ld lock_us=%ld emit_us=%ld emit+=%llu "
                    "elide+=%llu blits=%d rack+=%lu rast+=%lu inv=%lu "
                    "input_lag_us=%ld\n",
                    frame_us, lock_us, emit_us,
                    (unsigned long long)(st->sprixelemissions - dbg_emit),
                    (unsigned long long)(st->sprixelelisions - dbg_elide),
                    tui_debug_last_tile_blits(), cur_rack - dbg_rack,
                    cur_rasters - dbg_rasters, tui_debug_tile_invalidations(),
                    input_lag_us);
            dbg_rack = cur_rack;
            dbg_rasters = cur_rasters;
            dbg_emit = st->sprixelemissions;
            dbg_elide = st->sprixelelisions;
          }
          free(st);
        }
      }
      rendered_version = cur_render_version;
      rendered_wall_sec = render_now.tv_sec;
      frame_dirty = false;
    } // end if (need_render)

    // Service a pending SIGUSR1 screenshot request. Done after the frame
    // timing/stats so the (heavy) composite + PNG encode doesn't inflate
    // the measured frame time. Composites the captured pixel planes off-
    // terminal; see frame_dump.c.
    if (tui_frame_dump_pending()) {
      tui_frame_dump_write(nc, std_plane, theme, NULL);
    }

    // Input is polled non-blocking. Each frame, drain ALL pending
    // input before re-rendering — otherwise a paste (which arrives
    // as a burst of individual key events) re-renders once per
    // character and crawls visibly on screen. The inner do/while
    // keeps polling until notcurses_get returns 0 (queue empty);
    // handler `continue` statements naturally re-poll for the next
    // key inside this inner loop rather than skipping to the next
    // frame.
    do {
      const struct timespec nonblocking = {0, 0};
      ncinput input;
      uint32_t key;
      // If the previous drain pass ended with a buffered ESC that
      // never resolved into a focus event, synthesize an Esc
      // keypress now so the normal handlers fire (just one frame
      // late). Otherwise pull the next key from notcurses.
      // True only for this iteration when we manufactured the Esc
      // from a previously-buffered byte. Keeps the focus-event
      // detector below from re-buffering the same Esc forever —
      // which used to manifest as "first Esc does nothing, second
      // Esc finally fires" because each synthesized Esc fell back
      // into focus_state=1 and got stalled.
      bool synthesized_esc = false;
      if (focus_pending_esc) {
        memset(&input, 0, sizeof(input));
        input.id = NCKEY_ESC;
        input.evtype = NCTYPE_PRESS;
        key = NCKEY_ESC;
        focus_pending_esc = false;
        synthesized_esc = true;
      } else {
        key = notcurses_get(nc, &nonblocking, &input);
      }
      if (key == (uint32_t)-1) {
        running = false;
        break;
      }
      if (key == 0) {
        // Input queue is empty. If we have an in-flight ESC
        // waiting for a follow-up byte, the burst has finished
        // without forming a focus sequence — schedule a real Esc
        // keypress for the next drain pass.
        if (focus_state >= 1) {
          focus_pending_esc = true;
        }
        // The buffered '[' (focus_state == 2) is dropped silently;
        // a bare ESC + '[' isn't meaningful to any of our modals,
        // so re-injecting it would be cosmetic noise. Keep the
        // simpler path.
        focus_state = 0;
        // No more input this frame — drop out of the drain loop so
        // the outer while re-renders.
        break;
      }
      // A real key/mouse event arrived — mark the frame dirty so the
      // conditional-render gate above renders the result next tick.
      frame_dirty = true;
      if (!input_dirty_pending) {
        clock_gettime(CLOCK_MONOTONIC, &input_dirty_ts);
        input_dirty_pending = true;
      }
      // Focus-event detection. We buffer ESC and ESC '[' silently
      // (the existing handlers never reach them while the sequence
      // is still in flight). On the third byte, either we
      // complete a focus event (CSI I / CSI O) — disable/enable
      // mouse mode and consume — or the sequence breaks and the
      // current byte falls through to normal handling. The
      // previously-buffered ESC/'[' are dropped; an Esc alone
      // followed in the same burst by an arbitrary key isn't a
      // pattern any of our modals expect.
      //
      // synthesized_esc skips this — that Esc came from our own
      // re-injection and is already known to be a real keypress;
      // re-buffering it would just deadlock.
      if (!synthesized_esc && focus_state == 0 && key == NCKEY_ESC &&
          input.evtype != NCTYPE_RELEASE) {
        focus_state = 1;
        continue;
      }
      if (focus_state == 1) {
        if (key == '[') {
          focus_state = 2;
          continue;
        }
        // Mismatch — treat the buffered ESC as a real Esc by
        // re-injecting it next iteration, then fall through with
        // the current key.
        focus_pending_esc = true;
        focus_state = 0;
        // Fall through; current key handled normally below.
      } else if (focus_state == 2) {
        if (key == 'I' || key == 'O') {
          const bool focus_in = (key == 'I');
          if (focus_in && !mouse_enabled) {
            notcurses_mice_enable(nc, mice_eventmask);
            mouse_enabled = true;
          } else if (!focus_in && mouse_enabled) {
            notcurses_mice_disable(nc);
            mouse_enabled = false;
          }
          focus_state = 0;
          continue;
        }
        // Mismatch on the third byte. Drop the buffered '['
        // and synthesize the original Esc on the next pass.
        focus_pending_esc = true;
        focus_state = 0;
        // Fall through.
      }
      if (input.evtype == NCTYPE_RELEASE) {
        // A scrollbar drag ends on any release event regardless of
        // where the cursor is — the user may have let go anywhere
        // on the screen.
        if (game_state.analysis_scrollbar_dragging) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.analysis_scrollbar_dragging = false;
          pthread_mutex_unlock(&game_state.mutex);
        }
        continue;
      }

      // ── Annotation cell editor ────────────────────────────────────
      // Two independent edit fields per pending entry: MOVE (row 1
      // "8H POND") and RACK (row 2 "AEINRT"). edit_field selects
      // which buffer is taking keystrokes; the other buffer keeps
      // its content so clicking between rows preserves work in
      // progress.
      // Mouse events fall through to the panel-router / click
      // handler below even while a field is being edited — that's
      // how clicking on the other row of the same entry switches
      // fields, and how clicking on a different panel deselects
      // the field.
      const bool is_mouse_event =
          key == NCKEY_BUTTON1 || key == NCKEY_BUTTON2 ||
          key == NCKEY_BUTTON3 || key == NCKEY_BUTTON4 ||
          key == NCKEY_BUTTON5 || key == NCKEY_BUTTON6 ||
          key == NCKEY_BUTTON7 || key == NCKEY_BUTTON8 ||
          key == NCKEY_BUTTON9 || key == NCKEY_BUTTON10 ||
          key == NCKEY_BUTTON11 || key == NCKEY_MOTION;
      if (modal == TUI_MODAL_NONE && game_state.edit_history_idx >= 0 &&
          !is_mouse_event) {
        const int idx = game_state.edit_history_idx;
        const bool field_move = game_state.edit_field == TUI_EDIT_FIELD_MOVE;
        const bool field_leave = game_state.edit_field == TUI_EDIT_FIELD_LEAVE;
        char *buf = field_move ? game_state.edit_move_buf
                               : (field_leave ? game_state.edit_leave_buf
                                              : game_state.edit_rack_buf);
        int *plen = field_move ? &game_state.edit_move_len
                               : (field_leave ? &game_state.edit_leave_len
                                              : &game_state.edit_rack_len);
        int *pcur = field_move ? &game_state.edit_move_cursor
                               : (field_leave ? &game_state.edit_leave_cursor
                                              : &game_state.edit_rack_cursor);
        const size_t buf_cap =
            field_move ? sizeof(game_state.edit_move_buf)
                       : (field_leave ? sizeof(game_state.edit_leave_buf)
                                      : sizeof(game_state.edit_rack_buf));

        // ── Board move-entry sub-mode ───────────────────────────────
        // When a board anchor is active, keystrokes drive the on-board
        // move builder rather than the history-cell text editor. The
        // builder keeps edit_move_buf in sync so the board ghost, the
        // on-board cursor arrow, and the History cell all track what's
        // being typed. Space toggles direction (keyboard parallel to
        // clicking the anchor); arrows relocate the anchor; Backspace
        // retracts a tile; Enter submits; Esc cancels.
        if (game_state.board_entry_active) {
          const bool is_pvc =
              game_state.app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER;
          if (key == NCKEY_ESC) {
            pthread_mutex_lock(&game_state.mutex);
            tui_board_builder_cancel(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
            pthread_mutex_lock(&game_state.mutex);
            tui_board_entry_submit(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          if (key == ' ' || key == NCKEY_TAB || key == '\t') {
            // Space or Tab toggles direction (keyboard parallel to
            // clicking the origin cell again).
            pthread_mutex_lock(&game_state.mutex);
            tui_board_builder_toggle_dir(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          if (key == NCKEY_LEFT || key == NCKEY_RIGHT || key == NCKEY_UP ||
              key == NCKEY_DOWN) {
            // Arrows move the ORIGIN (the user's typing start cell);
            // the walked-back anchor re-derives from it. Moving the
            // anchor directly got stuck against leading playthrough:
            // set_anchor immediately walked it back again.
            pthread_mutex_lock(&game_state.mutex);
            int origin_r = game_state.board_origin_row;
            int origin_c = game_state.board_origin_col;
            if (key == NCKEY_LEFT && origin_c > 0) {
              origin_c--;
            } else if (key == NCKEY_RIGHT && origin_c < BOARD_DIM - 1) {
              origin_c++;
            } else if (key == NCKEY_UP && origin_r > 0) {
              origin_r--;
            } else if (key == NCKEY_DOWN && origin_r < BOARD_DIM - 1) {
              origin_r++;
            }
            // Re-anchoring drops any in-progress tiles back to the rack
            // and keeps the current direction.
            tui_board_builder_set_anchor(&game_state, origin_r, origin_c,
                                         game_state.board_dir);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
            pthread_mutex_lock(&game_state.mutex);
            tui_board_entry_backspace(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          if (key >= 0x20 && key < 0x7f) {
            const char ch = (char)key;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
              pthread_mutex_lock(&game_state.mutex);
              tui_board_entry_type_letter(&game_state, ch,
                                          ncinput_shift_p(&input), is_pvc);
              pthread_mutex_unlock(&game_state.mutex);
            }
            // Swallow other printables (digits, punctuation) in board
            // mode so they don't leak into the coord token.
            continue;
          }
          // Swallow anything else while board entry is active.
          continue;
        }

        if (key == NCKEY_ESC) {
          pthread_mutex_lock(&game_state.mutex);
          // Blur-commit: save whatever's typed before leaving.
          tui_commit_edit_and_revalidate(&game_state);
          game_state.edit_history_idx = -1;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }

        // Shift+arrow: navigate between turns (plain arrows cycle
        // within the current turn's editable fields below). Loads the
        // target entry's stored move_str / rack_str into the edit
        // buffers, lands on the MOVE field, and slides the history
        // cursor along so the board / pills snap to the target turn.
        if (ncinput_shift_p(&input) &&
            (key == NCKEY_LEFT || key == NCKEY_RIGHT || key == NCKEY_UP ||
             key == NCKEY_DOWN)) {
          const int dir = (key == NCKEY_LEFT || key == NCKEY_UP) ? -1 : 1;
          pthread_mutex_lock(&game_state.mutex);
          const int target = game_state.edit_history_idx + dir;
          if (target >= 0 && target < game_state.history_count) {
            // Blur-commit the turn we're leaving before loading the
            // target's stored text.
            tui_commit_edit_and_revalidate(&game_state);
            TuiHistoryEntry *e = &game_state.history[target];
            snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                     "%s", e->move_str);
            game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
            game_state.edit_move_cursor = game_state.edit_move_len;
            snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                     "%s", e->rack_str);
            game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
            game_state.edit_rack_cursor = game_state.edit_rack_len;
            // Treat a non-empty stored rack as user-authored so the
            // editor doesn't re-derive it from the move's inferred
            // letters the moment focus lands.
            game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
            // Switching turns — drop any stale carryover seed from a
            // previously-edited turn so it doesn't merge into this
            // entry's rack.
            game_state.edit_rack_carryover[0] = '\0';
            game_state.edit_history_idx = target;
            game_state.history_cursor = target;
            game_state.edit_field = TUI_EDIT_FIELD_MOVE;
            tui_game_state_parse_edit_buf(&game_state);
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }

        // Tab from RACK commits the rack (and the leave derived
        // from it) onto the entry, then hops back to MOVE without
        // exiting edit mode. That way the row-2 rack display goes
        // through the same alphagram-sorted committed-entry path
        // as Enter, so tabbing between fields doesn't leave the
        // cell showing a raw typed-order rack.
        // Up-arrow shares this path — it's the "row above me is
        // the move row, take me there" gesture annotation users
        // reach for instinctively.
        if ((key == NCKEY_TAB || key == '\t' || key == NCKEY_UP) &&
            !field_move) {
          pthread_mutex_lock(&game_state.mutex);
          tui_game_state_parse_edit_buf(&game_state);
          // Alphagram the rack buffer on focus-leave so the sorted
          // form sticks across re-edits. Once the user has tabbed
          // out, the typed order is no longer interesting — what
          // they see in the cell is the alphagram, and re-opening
          // the field should land them on that same alphagram.
          // Only the sort order (state->rack_sort) can change it
          // from here on.
          if (game_state.edit_rack_valid && game_state.edit_rack_len > 0) {
            char sorted_rack[24];
            char raw[24];
            const int n_raw = game_state.edit_rack_len < (int)sizeof(raw) - 1
                                  ? game_state.edit_rack_len
                                  : (int)sizeof(raw) - 1;
            memcpy(raw, game_state.edit_rack_buf, (size_t)n_raw);
            raw[n_raw] = '\0';
            tui_format_alphagram_for_sort(raw, game_state.ld,
                                          game_state.rack_sort, sorted_rack,
                                          sizeof(sorted_rack));
            snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                     "%s", sorted_rack);
            game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
            game_state.edit_rack_cursor = game_state.edit_rack_len;
          }
          if (idx < game_state.history_count && game_state.edit_rack_valid) {
            TuiHistoryEntry *e = &game_state.history[idx];
            int n = game_state.edit_rack_len;
            if (n >= (int)sizeof(e->rack_str)) {
              n = (int)sizeof(e->rack_str) - 1;
            }
            memcpy(e->rack_str, game_state.edit_rack_buf, (size_t)n);
            e->rack_str[n] = '\0';
            // User-typed leave (via click into LEAVE) wins over the
            // auto-derived edit_move_leave. Otherwise fall back to
            // the auto-derived value.
            if (game_state.edit_leave_len > 0) {
              snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                       game_state.edit_leave_buf);
            } else if (game_state.edit_move_leave[0] != '\0') {
              snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                       game_state.edit_move_leave);
            }
          }
          game_state.edit_field = TUI_EDIT_FIELD_MOVE;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }

        // Play-vs-computer: the history cell doubles as a keyboard
        // move-entry surface for the human's live pending turn — type
        // "8D WORD" and press Enter. Enter routes through the same
        // commit as board entry (real bag draw, clock charge, bot
        // handoff). The annotation commit paths below must stay
        // unreachable in this mode: they play WITHOUT drawing, which
        // desyncs the bag and never hands the turn to the bot. Tab /
        // Down field switches from MOVE are swallowed too — the rack
        // row is read-only in play-vs-computer (racks come from the
        // bag). All other keys (typing, Backspace, arrows, Esc) fall
        // through to the normal editor handlers.
        if (game_state.app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER &&
            ((key == NCKEY_ENTER || key == '\r' || key == '\n') ||
             ((key == NCKEY_TAB || key == '\t' || key == NCKEY_DOWN) &&
              field_move))) {
          if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
            pthread_mutex_lock(&game_state.mutex);
            tui_pvc_commit_preview_move(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
          }
          continue;
        }

        // Enter on the MOVE field advances to the RACK field
        // (the user's "natural next step" after typing a move).
        // The move ALSO commits to the entry here — without that,
        // pressing Enter twice (move-Enter then rack-Enter) would
        // leave the move never persisted: the second Enter only
        // commits the rack, and the user would lose the move text
        // along with its score when edit mode exited.
        // Enter on the RACK field is the final commit — it stamps
        // the entry's rack and exits edit mode.
        // Tab from MOVE shares this branch so the two keys feel
        // interchangeable.
        const bool tab_from_move =
            (key == NCKEY_TAB || key == '\t') && field_move;
        // Down-arrow from MOVE is a Tab alias — the "row below me
        // is the rack row, take me there" gesture.
        const bool down_from_move = key == NCKEY_DOWN && field_move;
        if (((key == NCKEY_ENTER || key == '\r' || key == '\n') ||
             tab_from_move || down_from_move) &&
            field_move) {
          pthread_mutex_lock(&game_state.mutex);
          tui_game_state_parse_edit_buf(&game_state);
          if (idx < game_state.history_count) {
            TuiHistoryEntry *e = &game_state.history[idx];
            switch (game_state.edit_move_kind) {
            case TUI_EDIT_MOVE_KIND_PLACEMENT:
            case TUI_EDIT_MOVE_KIND_EXCHANGE:
            case TUI_EDIT_MOVE_KIND_PASS: {
              // Exchanges go onto the entry in the TUI's compact
              // "-ABC" form (matching how finalize_history shows
              // bot-played exchanges). The engine validator still
              // sees "ex ABC" via edit_move_canonical above; this
              // is purely a display normalization at the commit
              // boundary.
              if (strncmp(game_state.edit_move_canonical, "ex ", 3) == 0) {
                snprintf(e->move_str, sizeof(e->move_str), "-%s",
                         game_state.edit_move_canonical + 3);
              } else {
                snprintf(e->move_str, sizeof(e->move_str), "%s",
                         game_state.edit_move_canonical);
              }
              // Always re-sync the score on every commit, even
              // when the engine rejected the new move (score < 0).
              // Otherwise editing XIPHOID → XYSTI leaves the old
              // XIPHOID score stale on the entry. Same for the
              // leave: clear it when the new state doesn't infer
              // one.
              e->score = game_state.edit_move_score >= 0
                             ? game_state.edit_move_score
                             : 0;
              // While the user hasn't authored the rack, the
              // entry's rack_str needs to track the move's
              // inferred letters. Editing XiPHOID → XIPHOID would
              // otherwise leave the stale "IODHPX?" baked onto the
              // entry, even though the rack panel updates live.
              // Same idea for the rack buffer (so re-opening the
              // editor lands on the fresh inferred seed). When the
              // rack tracks inferred, leave is exactly empty —
              // skip the parser's edit_move_leave value (which was
              // computed against the old buffer and is stale by now).
              if (!game_state.edit_rack_user_modified) {
                if (game_state.edit_move_inferred_rack[0] != '\0') {
                  char sorted[24];
                  tui_format_alphagram_for_sort(
                      game_state.edit_move_inferred_rack, game_state.ld,
                      game_state.rack_sort, sorted, sizeof(sorted));
                  snprintf(e->rack_str, sizeof(e->rack_str), "%s", sorted);
                  snprintf(game_state.edit_rack_buf,
                           sizeof(game_state.edit_rack_buf), "%s", sorted);
                  game_state.edit_rack_len =
                      (int)strlen(game_state.edit_rack_buf);
                  game_state.edit_rack_cursor = game_state.edit_rack_len;
                } else {
                  e->rack_str[0] = '\0';
                  game_state.edit_rack_buf[0] = '\0';
                  game_state.edit_rack_len = 0;
                  game_state.edit_rack_cursor = 0;
                }
                e->leave_str[0] = '\0';
                game_state.edit_move_leave[0] = '\0';
              } else if (game_state.edit_leave_len > 0) {
                snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                         game_state.edit_leave_buf);
              } else if (game_state.edit_move_leave[0] != '\0') {
                snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                         game_state.edit_move_leave);
              } else {
                e->leave_str[0] = '\0';
              }
              break;
            }
            default:
              break;
            }
          }
          // If the rack buffer is still empty, seed it from the
          // move's inferred letters — alphagrammed per the user's
          // rack_sort so the seed matches what the rack panel
          // shows for the same tiles. Subsequent typing into RACK
          // appends raw to the seeded form (and a focus-leave will
          // re-sort if needed).
          if (game_state.edit_rack_len == 0 &&
              game_state.edit_move_inferred_rack[0] != '\0') {
            char sorted_seed[24];
            tui_format_alphagram_for_sort(game_state.edit_move_inferred_rack,
                                          game_state.ld, game_state.rack_sort,
                                          sorted_seed, sizeof(sorted_seed));
            snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                     "%s", sorted_seed);
            game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
            game_state.edit_rack_cursor = game_state.edit_rack_len;
            // Auto-seed from inferred — the user hasn't taken
            // authorship of the rack content yet, so the rack
            // should continue tracking move edits.
            game_state.edit_rack_user_modified = false;
            tui_game_state_parse_edit_buf(&game_state);
          }
          game_state.edit_field = TUI_EDIT_FIELD_RACK;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }

        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          pthread_mutex_lock(&game_state.mutex);
          tui_game_state_parse_edit_buf(&game_state);
          if (idx < game_state.history_count) {
            TuiHistoryEntry *e = &game_state.history[idx];
            if (field_move) {
              // MOVE commit branches by the parser's kind. The
              // canonical string (already normalized to engine
              // form: "8H WORD" / "ex AB" / "pass") goes into
              // move_str, and the parser's inferred rack fills
              // rack_str when the user hasn't typed one. The
              // score (when the engine returned one) is stamped
              // on the entry so the cell keeps showing it after
              // edit mode exits.
              switch (game_state.edit_move_kind) {
              case TUI_EDIT_MOVE_KIND_PLACEMENT:
              case TUI_EDIT_MOVE_KIND_EXCHANGE:
              case TUI_EDIT_MOVE_KIND_PASS: {
                // Same "-ABC" display normalization the Tab/Enter-
                // on-MOVE commit path uses. Belt-and-braces — this
                // branch should be unreachable since Enter-on-MOVE
                // is caught above, but if anything ever routes here
                // we don't want stale "ex ABC" leaking onto entries.
                if (strncmp(game_state.edit_move_canonical, "ex ", 3) == 0) {
                  snprintf(e->move_str, sizeof(e->move_str), "-%s",
                           game_state.edit_move_canonical + 3);
                } else {
                  snprintf(e->move_str, sizeof(e->move_str), "%s",
                           game_state.edit_move_canonical);
                }
                if (e->rack_str[0] == '\0' &&
                    game_state.edit_move_inferred_rack[0] != '\0') {
                  snprintf(e->rack_str, sizeof(e->rack_str), "%s",
                           game_state.edit_move_inferred_rack);
                }
                // Always re-sync the score on commit (see the
                // earlier commit branch's note).
                e->score = game_state.edit_move_score >= 0
                               ? game_state.edit_move_score
                               : 0;
                if (game_state.edit_leave_len > 0) {
                  snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                           game_state.edit_leave_buf);
                } else if (game_state.edit_move_leave[0] != '\0') {
                  snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                           game_state.edit_move_leave);
                } else {
                  e->leave_str[0] = '\0';
                }
                break;
              }
              case TUI_EDIT_MOVE_KIND_WORD_ONLY:
              case TUI_EDIT_MOVE_KIND_PARTIAL:
              case TUI_EDIT_MOVE_KIND_INVALID:
              case TUI_EDIT_MOVE_KIND_EMPTY:
              default:
                // Nothing to commit yet — bare word triggers the
                // (not-yet-implemented) placement enumerator on
                // focus-away, and partial / invalid buffers wait
                // for the user to keep typing.
                break;
              }
            } else {
              // RACK field commit. Alphagram the buffer in place so
              // re-opening this entry lands on the sorted form
              // rather than the raw typed order. Matches the Tab/Up
              // path's behavior.
              if (game_state.edit_rack_valid && game_state.edit_rack_len > 0) {
                char sorted_rack[24];
                char raw[24];
                const int n_raw =
                    game_state.edit_rack_len < (int)sizeof(raw) - 1
                        ? game_state.edit_rack_len
                        : (int)sizeof(raw) - 1;
                memcpy(raw, game_state.edit_rack_buf, (size_t)n_raw);
                raw[n_raw] = '\0';
                tui_format_alphagram_for_sort(raw, game_state.ld,
                                              game_state.rack_sort, sorted_rack,
                                              sizeof(sorted_rack));
                snprintf(game_state.edit_rack_buf,
                         sizeof(game_state.edit_rack_buf), "%s", sorted_rack);
                game_state.edit_rack_len =
                    (int)strlen(game_state.edit_rack_buf);
                game_state.edit_rack_cursor = game_state.edit_rack_len;
              }
              if (game_state.edit_rack_valid) {
                int n = game_state.edit_rack_len;
                if (n >= (int)sizeof(e->rack_str)) {
                  n = (int)sizeof(e->rack_str) - 1;
                }
                memcpy(e->rack_str, game_state.edit_rack_buf, (size_t)n);
                e->rack_str[n] = '\0';
              }
              // Stamp the leave. User-typed buffer (via click+type
              // in LEAVE) overrides the parser's auto-derived value.
              if (game_state.edit_leave_len > 0) {
                snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                         game_state.edit_leave_buf);
              } else if (game_state.edit_move_leave[0] != '\0') {
                snprintf(e->leave_str, sizeof(e->leave_str), "%s",
                         game_state.edit_move_leave);
              }
            }
          }
          // If the rack commit followed a *legal* play (engine-
          // accepted placement / exchange / pass), advance the
          // annotation to the next turn: play the move on the
          // engine, tag tile owners so placed tiles render in their
          // player's permanent color, append a fresh pending entry
          // for the OTHER player, and re-open the editor on it. If
          // the play wasn't legal (score < 0, partial, bare word,
          // invalid), fall through to the existing "just exit edit
          // mode" behavior — the user keeps editing the same turn.
          if (field_move == false && game_state.edit_preview_move_valid &&
              game_state.edit_preview_move != NULL &&
              game_state.edit_move_score >= 0 &&
              (game_state.edit_move_kind == TUI_EDIT_MOVE_KIND_PLACEMENT ||
               game_state.edit_move_kind == TUI_EDIT_MOVE_KIND_EXCHANGE ||
               game_state.edit_move_kind == TUI_EDIT_MOVE_KIND_PASS)) {
            const int playing_idx = (idx < game_state.history_count)
                                        ? game_state.history[idx].player_idx
                                        : 0;
            // play_move_without_drawing_tiles: place the tiles, debit
            // them from the player's rack, update score, advance the
            // turn — but DON'T draw replacements from the bag. The
            // annotator owns the rack content. After this call the
            // player's rack holds only their leave; the next turn's
            // editor sees that leave when sync_player_rack_to_editor
            // checks the live rack, and shows it in the pill.
            play_move_without_drawing_tiles(game_state.edit_preview_move,
                                            game_state.game);
            // Tag the placed-tile squares with the player_idx so the
            // per-cell pixel plane picks the correct tile_bg color.
            // Mirrors the bot-worker path at bot_worker.c:918-933.
            if (move_get_type(game_state.edit_preview_move) ==
                GAME_EVENT_TILE_PLACEMENT_MOVE) {
              const int dir = move_get_dir(game_state.edit_preview_move);
              int r = move_get_row_start(game_state.edit_preview_move);
              int c = move_get_col_start(game_state.edit_preview_move);
              const int n_tiles =
                  move_get_tiles_length(game_state.edit_preview_move);
              for (int t = 0; t < n_tiles; t++) {
                if (move_get_tile(game_state.edit_preview_move, t) !=
                    PLAYED_THROUGH_MARKER) {
                  board_set_square_owner(game_get_board(game_state.game), r, c,
                                         playing_idx);
                }
                if (board_is_dir_vertical(dir)) {
                  r++;
                } else {
                  c++;
                }
              }
            }
            // Mark the current entry as no longer pending and stamp
            // its cumulative score. play_move_without_drawing_tiles
            // already added the move's points to the engine player's
            // running total, so reading it now gives the correct
            // total_after for this turn (e.g., turn 3's DONKEYS adds
            // 87 to P1's 24 from turn 1 → total_after = 111). The
            // history renderer uses this for the "+87/111" column.
            if (idx < game_state.history_count) {
              game_state.history[idx].pending = false;
              game_state.history[idx].total_after =
                  equity_to_int(player_get_score(
                      game_get_player(game_state.game, playing_idx)));
            }
            // Advance to the next turn. If the committed turn was the
            // LAST entry, append a fresh pending entry for the next
            // player. If it was a MIDDLE turn being re-committed (a
            // later turn already exists), DON'T append — just slide
            // the editor onto the already-existing next turn. Without
            // this guard, re-committing turn 2 would spawn a phantom
            // extra pending turn at the end.
            const int next_player =
                game_get_player_on_turn_index(game_state.game);
            if (idx + 1 >= game_state.history_count) {
              tui_bot_worker_append_pending_history(
                  &game_state, next_player, NULL,
                  game_state.time_per_side_seconds);
              game_state.edit_history_idx = game_state.history_count - 1;
            } else {
              game_state.edit_history_idx = idx + 1;
            }
            // Move the history-cursor onto the new pending entry too.
            // pick_render_board / pick_render_rack rewind to a cursor-
            // pinned committed entry's snapshot, so leaving the cursor
            // on turn 1 would keep showing the pre-turn-1 (empty) board
            // and racks even after several commits. Pending entries
            // fall through to the live engine state, which is exactly
            // what the annotator wants to see while editing the next
            // turn.
            game_state.history_cursor = game_state.edit_history_idx;
            game_state.edit_field = TUI_EDIT_FIELD_MOVE;
            game_state.edit_leave_buf[0] = '\0';
            game_state.edit_leave_len = 0;
            game_state.edit_leave_cursor = 0;
            game_state.edit_rack_carryover[0] = '\0';
            game_state.edit_rack_user_modified = false;
            {
              const TuiHistoryEntry *dest =
                  &game_state.history[game_state.edit_history_idx];
              if (dest->move_str[0] != '\0' || dest->rack_str[0] != '\0') {
                // Advancing onto an EXISTING (already-edited) next
                // turn: load its stored move / rack so re-committing
                // a middle turn doesn't blank the turn after it.
                snprintf(game_state.edit_move_buf,
                         sizeof(game_state.edit_move_buf), "%s",
                         dest->move_str);
                game_state.edit_move_len =
                    (int)strlen(game_state.edit_move_buf);
                game_state.edit_move_cursor = game_state.edit_move_len;
                snprintf(game_state.edit_rack_buf,
                         sizeof(game_state.edit_rack_buf), "%s",
                         dest->rack_str);
                game_state.edit_rack_len =
                    (int)strlen(game_state.edit_rack_buf);
                game_state.edit_rack_cursor = game_state.edit_rack_len;
                game_state.edit_rack_user_modified = dest->rack_str[0] != '\0';
              } else {
                // Fresh turn: empty buffers, then carry the player's
                // most-recent leave forward as the rack seed. Stored
                // in edit_rack_carryover so sync merges carryover +
                // the move's played tiles into the effective rack as
                // the user types (e.g. "KAM" on a turn carrying
                // "ERST" yields "AEKMRST").
                game_state.edit_move_buf[0] = '\0';
                game_state.edit_move_len = 0;
                game_state.edit_move_cursor = 0;
                game_state.edit_rack_buf[0] = '\0';
                game_state.edit_rack_len = 0;
                game_state.edit_rack_cursor = 0;
                for (int prev = game_state.edit_history_idx - 1; prev >= 0;
                     prev--) {
                  const TuiHistoryEntry *pe = &game_state.history[prev];
                  if (pe->pending || pe->player_idx != next_player) {
                    continue;
                  }
                  if (pe->leave_str[0] != '\0') {
                    snprintf(game_state.edit_rack_carryover,
                             sizeof(game_state.edit_rack_carryover), "%s",
                             pe->leave_str);
                    snprintf(game_state.edit_rack_buf,
                             sizeof(game_state.edit_rack_buf), "%s",
                             pe->leave_str);
                    game_state.edit_rack_len =
                        (int)strlen(game_state.edit_rack_buf);
                    game_state.edit_rack_cursor = game_state.edit_rack_len;
                  }
                  break;
                }
              }
            }
            tui_game_state_parse_edit_buf(&game_state);
            // Replay the full committed history forward on a fresh
            // board, surfacing any per-entry validation errors (tile
            // collision, disconnected play, rack missing letters,
            // etc.) so the user sees them inline. Cheap-enough at
            // typical history lengths and only runs on commit, not
            // every keystroke.
            tui_game_state_revalidate_history(&game_state);
            pthread_mutex_unlock(&game_state.mutex);
            continue;
          }
          // Exit edit mode but keep the buffers around so a
          // re-open lands on whatever the user was typing.
          game_state.edit_history_idx = -1;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        // Plain Left/Right inside the editor follow the unified
        // step-through sequence:
        //   [4>] / 1> → 1.MOVE → 1.RACK → 2> → 2.MOVE → 2.RACK → 3> → ...
        // Within a field's text the arrow moves the cursor; at the
        // far edge of the field it advances/retreats one position
        // in the sequence (which may exit the editor onto the next
        // / current label). Shift+arrow stays as label-skip and was
        // handled by the earlier Shift+arrow block.
        if (key == NCKEY_LEFT) {
          pthread_mutex_lock(&game_state.mutex);
          if (*pcur > 0) {
            (*pcur)--;
          } else if (field_leave) {
            // LEAVE start → RACK end of same turn. (LEAVE itself
            // is not in the forward arrow sequence — it's only
            // reachable via click — but if you arrived there by
            // clicking, Left walks you back to RACK.)
            game_state.edit_field = TUI_EDIT_FIELD_RACK;
            game_state.edit_rack_cursor = game_state.edit_rack_len;
          } else if (!field_move) {
            // RACK start → MOVE end of same turn.
            game_state.edit_field = TUI_EDIT_FIELD_MOVE;
            game_state.edit_move_cursor = game_state.edit_move_len;
          } else {
            // MOVE start → exit editor onto this entry's label.
            // Blur-commit first so the typed move/rack/leave persist.
            const int leaving = game_state.edit_history_idx;
            tui_commit_edit_and_revalidate(&game_state);
            game_state.history_cursor = leaving;
            game_state.edit_history_idx = -1;
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key == NCKEY_RIGHT) {
          pthread_mutex_lock(&game_state.mutex);
          if (*pcur < *plen) {
            (*pcur)++;
          } else if (field_move) {
            // MOVE end → RACK start of same turn.
            game_state.edit_field = TUI_EDIT_FIELD_RACK;
            game_state.edit_rack_cursor = 0;
          } else {
            // RACK end (or LEAVE end) → forward out of the turn.
            // Blur-commit, then advance: onto the next turn's label
            // if it exists, or — when this is the LAST turn and it
            // holds a valid move — create the next turn and land on
            // its label. Forward-nav past the final turn is how new
            // turns get created (no Enter required).
            const int cur = game_state.edit_history_idx;
            tui_commit_edit_and_revalidate(&game_state);
            if (cur + 1 < game_state.history_count) {
              game_state.history_cursor = cur + 1;
              game_state.edit_history_idx = -1;
            } else if (cur >= 0 && cur < game_state.history_count &&
                       game_state.history[cur].move_str[0] != '\0' &&
                       game_state.history[cur].error_str[0] == '\0') {
              // After revalidate the engine sits at the post-game
              // position, so the on-turn index is the next player.
              const int next_player =
                  game_get_player_on_turn_index(game_state.game);
              tui_bot_worker_append_pending_history(
                  &game_state, next_player, NULL,
                  game_state.time_per_side_seconds);
              game_state.history_cursor = game_state.history_count - 1;
              game_state.edit_history_idx = -1;
            }
            // else: last turn with no valid move — stay put.
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key == NCKEY_HOME) {
          pthread_mutex_lock(&game_state.mutex);
          *pcur = 0;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key == NCKEY_END) {
          pthread_mutex_lock(&game_state.mutex);
          *pcur = *plen;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
          pthread_mutex_lock(&game_state.mutex);
          if (*pcur > 0) {
            memmove(&buf[*pcur - 1], &buf[*pcur], (size_t)(*plen - *pcur + 1));
            (*pcur)--;
            (*plen)--;
            if (!field_move) {
              game_state.edit_rack_user_modified = true;
              game_state.edit_rack_carryover[0] = '\0';
            }
            tui_game_state_parse_edit_buf(&game_state);
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key == NCKEY_DEL) {
          pthread_mutex_lock(&game_state.mutex);
          if (*pcur < *plen) {
            memmove(&buf[*pcur], &buf[*pcur + 1], (size_t)(*plen - *pcur));
            (*plen)--;
            if (!field_move) {
              game_state.edit_rack_user_modified = true;
              game_state.edit_rack_carryover[0] = '\0';
            }
            tui_game_state_parse_edit_buf(&game_state);
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        if (key >= 0x20 && key < 0x7f) {
          pthread_mutex_lock(&game_state.mutex);
          // RACK field caps at 7 tiles regardless of buffer size —
          // a Scrabble rack never holds more than that, and the
          // visible zone in the cell is 7 + 1 (cursor) cells wide.
          // Hitting the cap leaves the cursor parked in the 8th
          // cell as a visual "no room" signal.
          const int per_field_cap = field_move ? (int)buf_cap - 1 : 7;
          if (*plen < per_field_cap && *plen + 1 < (int)buf_cap) {
            // Default: uppercase letters in both fields. Space is
            // meaningful in MOVE (between coord and word) but not
            // in RACK.
            // In the MOVE field, a Shift+letter on a real key
            // (i.e., the terminal delivers an uppercase ASCII
            // letter, not a lowercase one) means "this tile is a
            // played blank designated as that letter". Store the
            // glyph as LOWERCASE so it parses as a blank in the
            // engine's move notation, which is the same convention
            // GCG / CGP / sim outputs use. The parser turns each
            // lowercase letter in the word into a '?' for the
            // inferred rack — so the rack panel correctly shows
            // one blank tile per played blank.
            char ch = (char)key;
            if (!field_move && ch == ' ') {
              pthread_mutex_unlock(&game_state.mutex);
              continue;
            }
            // MOVE field: a second space is never meaningful (the one
            // space separates coord from word), and autofill may have
            // already supplied it when it absorbed a leading played-
            // through letter right after the coord — swallow dupes so
            // "8H<space>" out of habit can't split the word token.
            if (field_move && ch == ' ' && strchr(buf, ' ') != NULL) {
              pthread_mutex_unlock(&game_state.mutex);
              continue;
            }
            const bool is_letter =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            // Coord-token letters (before the first space, e.g. the F
            // in "8F ...") are positions, not tiles — only letters in
            // the word part go through rack resolution.
            const bool typing_word =
                *pcur > 0 && memchr(buf, ' ', (size_t)*pcur) != NULL;
            if (field_move && is_letter && typing_word &&
                game_state.app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
              // Play-vs-computer: racks are known, so resolve the
              // letter against the human's live rack — same rule as
              // board entry. An unshifted letter the rack doesn't
              // hold becomes a blank when one is available (no Shift
              // required); a letter with neither a real tile nor a
              // blank left is rejected outright. EXCEPT when the
              // letter lands on an occupied square: typing the board's
              // own letter there is playthrough spelling (e.g. the C
              // in "1A COMBINES" over an existing C) — pass it through
              // verbatim; any other letter would collide, so reject.
              const char up_ch =
                  (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
              int land_row = -1;
              int land_col = -1;
              bool resolved = false;
              if (game_state.game != NULL &&
                  tui_cell_word_landing_square(&game_state, &land_row,
                                               &land_col)) {
                const MachineLetter board_ml = board_get_letter(
                    game_get_board(game_state.game), land_row, land_col);
                if (board_ml != ALPHABET_EMPTY_SQUARE_MARKER) {
                  if (get_unblanked_machine_letter(board_ml) ==
                      tui_ml_for_upper(&game_state, up_ch)) {
                    ch = up_ch; // playthrough: spell the board letter
                    resolved = true;
                  } else {
                    pthread_mutex_unlock(&game_state.mutex);
                    continue; // collides with a different board letter
                  }
                }
              }
              if (!resolved) {
                bool as_blank = false;
                if (!tui_pvc_resolve_typed_letter(&game_state, up_ch,
                                                  ncinput_shift_p(&input),
                                                  &as_blank)) {
                  pthread_mutex_unlock(&game_state.mutex);
                  continue;
                }
                ch = as_blank ? (char)(up_ch - 'A' + 'a') : up_ch;
              }
            } else if (field_move && is_letter && ncinput_shift_p(&input)) {
              // Shift+letter on MOVE → played blank (lowercase).
              if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
              }
            } else if (ch >= 'a' && ch <= 'z') {
              // Default: case-fold lowercase up to uppercase.
              ch = (char)(ch - 'a' + 'A');
            }
            memmove(&buf[*pcur + 1], &buf[*pcur], (size_t)(*plen - *pcur + 1));
            buf[*pcur] = ch;
            (*pcur)++;
            (*plen)++;
            if (!field_move) {
              // User typed into the RACK field — the buffer is now
              // theirs; the rack should NOT snap back to whatever
              // the move infers on the next move edit. Also drop the
              // carryover seed so it stops auto-merging played tiles.
              game_state.edit_rack_user_modified = true;
              game_state.edit_rack_carryover[0] = '\0';
            }
            tui_game_state_parse_edit_buf(&game_state);
            // MOVE field: auto-extend through any existing tiles the
            // word now runs into, so played-through letters fill in
            // without retyping.
            if (field_move && *pcur == *plen) {
              tui_autofill_playthrough(&game_state);
            }
          }
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
        (void)idx;
        (void)buf;
        continue;
      }

      // Scroll wheel anywhere on screen scrolls the Analysis panel
      // (currently the only scrollable widget). 3 ranks per wheel
      // notch matches common terminal feel.
      if ((key == NCKEY_BUTTON4 || key == NCKEY_BUTTON5) &&
          modal == TUI_MODAL_NONE) {
        pthread_mutex_lock(&game_state.mutex);
        const int total = game_state.last_rendered_analysis_row_count;
        const int view = atomic_load(&game_state.analysis_visible_rows);
        const int max_scroll = total > view ? total - view : 0;
        int sof = game_state.analysis_scroll_offset;
        sof += (key == NCKEY_BUTTON5 ? 3 : -3);
        if (sof < 0) {
          sof = 0;
        }
        if (sof > max_scroll) {
          sof = max_scroll;
        }
        game_state.analysis_scroll_offset = sof;
        // Drag the cursor along with the view so the auto-scroll
        // pass at render time doesn't snap the view right back to
        // wherever the cursor was sitting.
        int cur = game_state.analysis_cursor;
        if (cur < sof) {
          cur = sof;
        }
        if (view > 0 && cur > sof + view - 1) {
          cur = sof + view - 1;
        }
        if (total > 0 && cur >= total) {
          cur = total - 1;
        }
        game_state.analysis_cursor = cur;
        pthread_mutex_unlock(&game_state.mutex);
        continue;
      }
      // Scrollbar click + drag. A press on the scrollbar's track
      // starts a drag; subsequent BUTTON1 events while dragging map
      // y → scroll_offset proportionally so the thumb tracks the
      // cursor. Drag ends at NCTYPE_RELEASE (handled above).
      if (key == NCKEY_BUTTON1 && modal == TUI_MODAL_NONE) {
        const int sb_top = atomic_load(&game_state.analysis_scrollbar_top);
        const int sb_bot = atomic_load(&game_state.analysis_scrollbar_bottom);
        const int sb_col = atomic_load(&game_state.analysis_scrollbar_col);
        const int sb_total = atomic_load(&game_state.analysis_scrollbar_total);
        const int sb_view = atomic_load(&game_state.analysis_scrollbar_view);
        const bool sb_active = sb_total > sb_view && sb_view > 0;
        // Hit-test forgiveness — a click on either the scrollbar
        // column or the panel's right border (one cell further
        // right) counts. Widening to the LEFT would eat clicks on
        // the rightmost row-content cell (the sprd column), which
        // is what caused MOVE-column row clicks to sometimes
        // register as scrollbar clicks instead.
        const bool over_sb = sb_active && input.y >= sb_top &&
                             input.y <= sb_bot && input.x >= sb_col &&
                             input.x <= sb_col + 1;
        if (over_sb || game_state.analysis_scrollbar_dragging) {
          pthread_mutex_lock(&game_state.mutex);
          game_state.analysis_scrollbar_dragging = true;
          const int track_h = sb_bot - sb_top + 1;
          const int max_scroll = sb_total - sb_view;
          int y_rel = (int)input.y - sb_top;
          if (y_rel < 0) {
            y_rel = 0;
          }
          if (y_rel > track_h - 1) {
            y_rel = track_h - 1;
          }
          const int denom = track_h > 1 ? track_h - 1 : 1;
          int sof = (int)((long long)y_rel * max_scroll / denom);
          if (sof < 0) {
            sof = 0;
          }
          if (sof > max_scroll) {
            sof = max_scroll;
          }
          game_state.analysis_scroll_offset = sof;
          // Move the cursor along with the view so the auto-scroll
          // pass at render time doesn't immediately snap the view
          // back to wherever the cursor was sitting.
          int cur = game_state.analysis_cursor;
          if (cur < sof) {
            cur = sof;
          }
          if (sb_view > 0 && cur > sof + sb_view - 1) {
            cur = sof + sb_view - 1;
          }
          const int total = game_state.last_rendered_analysis_row_count;
          if (total > 0 && cur >= total) {
            cur = total - 1;
          }
          game_state.analysis_cursor = cur;
          // Focus the Analysis panel while dragging so the user can
          // see the cursor highlight on the scrolled rows.
          game_state.focused_panel = TUI_FOCUS_ANALYSIS;
          pthread_mutex_unlock(&game_state.mutex);
          continue;
        }
      }
      // Left mouse click → focus the panel under the cursor. Only
      // NCKEY_BUTTON1 (left-button press) for now; scroll wheel and
      // right-click are reserved for future per-panel interactions.
      if (key == NCKEY_BUTTON1 && modal == TUI_MODAL_NONE) {
        const int hit =
            tui_game_panel_at(std_plane, &game_state, input.y, input.x);
        if (hit >= 0) {
          pthread_mutex_lock(&game_state.mutex);
          if (hit != 0 && game_state.slash_active) {
            game_state.slash_active = false;
            game_state.slash_len = 0;
            game_state.slash_cursor = 0;
            game_state.slash_buf[0] = '\0';
          }
          // First click into a panel just focuses it (with the cursor
          // reset to the label). A click on History when it's ALREADY
          // focused reads the per-entry hit map to move the in-panel
          // cursor — clicks on the title / chrome row snap back to
          // -1, clicks on a turn jump to that entry.
          //
          // Exception: a click that lands directly on a PENDING entry
          // skips the two-step "focus first, act second" dance and
          // jumps straight into edit mode. The pending row is the
          // primary input surface in annotation mode; needing to
          // click twice to start typing felt broken in testing.
          bool pending_edit_handled = false;
          // Play-vs-computer never opens the history-cell text editor:
          // the human enters moves on the board, and the cell editor's
          // blur-revalidation replays committed history from text, which
          // would desync the live bag-drawn racks. Clicks on History in
          // that mode just move the browse cursor (handled below).
          if (hit == TUI_FOCUS_HISTORY &&
              game_state.app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER) {
            int entry_row_off = 0;
            const int target =
                tui_history_cursor_field_at(input.y, input.x, &entry_row_off);
            // Click into any history entry's textedit (move/rack/
            // leave) opens the editor on that field — committed
            // entries included. Previously the pending-only check
            // meant clicks on already-finalized turns just parked
            // the history cursor on the row label, with no way to
            // jump straight into the cell. Annotation flow needs to
            // revise prior turns, so all entries are addressable.
            if (target >= 0 && target < game_state.history_count) {
              const TuiHistoryEntry *e = &game_state.history[target];
              int field;
              switch (entry_row_off) {
              case 1:
                field = TUI_EDIT_FIELD_RACK;
                break;
              case 2:
                field = TUI_EDIT_FIELD_LEAVE;
                break;
              default:
                field = TUI_EDIT_FIELD_MOVE;
                break;
              }
              // Switching to a DIFFERENT entry: blur-commit the turn
              // we're leaving (so typed text persists), then reload
              // the clicked entry's stored move/rack/leave into the
              // editor buffers. (Clicking within the SAME entry —
              // e.g. JUNKY typed in MOVE, then clicking its RACK row
              // — leaves the buffers untouched so work-in-progress
              // survives a field switch.)
              if (game_state.edit_history_idx != target) {
                if (game_state.edit_history_idx >= 0) {
                  tui_commit_edit_and_revalidate(&game_state);
                  // commit may re-fetch e via revalidate side effects;
                  // re-resolve the pointer to be safe.
                  e = &game_state.history[target];
                }
                snprintf(game_state.edit_move_buf,
                         sizeof(game_state.edit_move_buf), "%s", e->move_str);
                game_state.edit_move_len =
                    (int)strlen(game_state.edit_move_buf);
                game_state.edit_move_cursor = game_state.edit_move_len;
                snprintf(game_state.edit_rack_buf,
                         sizeof(game_state.edit_rack_buf), "%s", e->rack_str);
                game_state.edit_rack_len =
                    (int)strlen(game_state.edit_rack_buf);
                game_state.edit_rack_cursor = game_state.edit_rack_len;
                // Committed rack reads as user-authored so it doesn't
                // snap back to the move's inferred letters on the
                // next keystroke.
                game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
                // Leave buffer tracks the entry's stored leave so
                // clicking into a different turn shows its leave.
                snprintf(game_state.edit_leave_buf,
                         sizeof(game_state.edit_leave_buf), "%s", e->leave_str);
                game_state.edit_leave_len =
                    (int)strlen(game_state.edit_leave_buf);
                game_state.edit_leave_cursor = game_state.edit_leave_len;
                // Drop stale carryover from a previously-edited turn.
                game_state.edit_rack_carryover[0] = '\0';
              }
              game_state.focused_panel = TUI_FOCUS_HISTORY;
              game_state.history_cursor = target;
              game_state.analysis_cursor = 0;
              game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
              game_state.analysis_anchored_move[0] = '\0';
              game_state.edit_history_idx = target;
              game_state.edit_field = field;
              // Clicking into the history-cell text editor takes over
              // from any in-progress board move-entry so keystrokes go
              // to the cell, not the board.
              game_state.board_entry_active = false;
              tui_game_state_parse_edit_buf(&game_state);
              pending_edit_handled = true;
            }
          }
          if (pending_edit_handled) {
            // Already entered edit mode; skip the focus-only fallback.
          } else if (hit == TUI_FOCUS_HISTORY) {
            // A click acts on the entry under the cursor IMMEDIATELY,
            // focusing the panel as a side effect — no "first click
            // focuses, second click selects" dance. (Two-step focus
            // felt broken everywhere it existed; the board and the
            // pending-cell click already worked this way.)
            game_state.focused_panel = TUI_FOCUS_HISTORY;
            int entry_row_off = 0;
            const int target =
                tui_history_cursor_field_at(input.y, input.x, &entry_row_off);
            if (target >= -1) {
              game_state.history_cursor = target;
              game_state.analysis_cursor = 0;
              game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
              game_state.analysis_anchored_move[0] = '\0';
              // Non-pending row clicked: just move the in-panel cursor.
              game_state.edit_history_idx = -1;
            }
            (void)entry_row_off;
          } else if (hit == TUI_FOCUS_ANALYSIS) {
            // Same single-click routing as History: focus + move the
            // in-panel cursor in one click. The hit also reports which
            // column was clicked (rank gutter vs move text) so the
            // cursor lands in the right mode. Title / chrome clicks
            // snap to -1.
            game_state.focused_panel = TUI_FOCUS_ANALYSIS;
            TuiAnalysisColumn clicked_col = TUI_ANALYSIS_COLUMN_RANK;
            const int target =
                tui_analysis_cursor_column_at(input.y, input.x, &clicked_col);
            if (target >= -1) {
              game_state.analysis_cursor = target;
              game_state.analysis_cursor_column = clicked_col;
              if (clicked_col == TUI_ANALYSIS_COLUMN_MOVE && target >= 0 &&
                  target < game_state.last_rendered_analysis_row_count) {
                snprintf(game_state.analysis_anchored_move,
                         sizeof(game_state.analysis_anchored_move), "%s",
                         game_state.last_rendered_analysis_rows[target].move);
              } else {
                game_state.analysis_anchored_move[0] = '\0';
              }
            }
          } else if (hit == TUI_FOCUS_BOARD &&
                     game_state.app_mode != TUI_APP_MODE_WATCH) {
            // Board move-entry: click an empty cell to anchor and start
            // typing; click the same anchor again to toggle direction.
            // A click on an occupied cell (that isn't the anchor) just
            // focuses the board.
            int cell_row = -1;
            int cell_col = -1;
            if (tui_board_cell_at(std_plane, &game_state, input.y, input.x,
                                  &cell_row, &cell_col)) {
              const Board *brd = game_get_board(game_state.game);
              const bool empty_cell =
                  brd != NULL && board_get_letter(brd, cell_row, cell_col) ==
                                     ALPHABET_EMPTY_SQUARE_MARKER;
              if (game_state.board_entry_active &&
                  cell_row == game_state.board_origin_row &&
                  cell_col == game_state.board_origin_col) {
                // Re-click on the ORIGIN cell (the one the user
                // clicked, not the playthrough-walked-back anchor —
                // comparing against the anchor made the toggle
                // unreachable whenever the walk-back had moved it).
                tui_board_builder_toggle_dir(&game_state);
              } else if (empty_cell) {
                const int dir = tui_board_builder_default_dir(
                    &game_state, cell_row, cell_col);
                tui_board_builder_set_anchor(&game_state, cell_row, cell_col,
                                             dir);
              }
            }
            game_state.focused_panel = TUI_FOCUS_BOARD;
          } else {
            game_state.focused_panel = hit;
          }
          pthread_mutex_unlock(&game_state.mutex);
        }
        continue;
      }
      if (key == NCKEY_RESIZE) {
        unsigned new_rows = 0;
        unsigned new_cols = 0;
        notcurses_refresh(nc, &new_rows, &new_cols);
        ncplane_resize_simple(std_plane, new_rows, new_cols);
        // A font-size change is delivered as a resize and shifts every
        // cached pixel composite to the wrong size. Drop all child planes
        // so the next render rebuilds them at the new cell-pixel ratio.
        tui_game_render_reset_grids();
        continue;
      }

      if (modal == TUI_MODAL_LOAD_POSITION) {
        if (key == NCKEY_ESC) {
          // Cancel — reset the previewed game back to idle so the
          // user returns to the empty board they came from.
          pthread_mutex_lock(&game_state.mutex);
          game_reset(game_state.game);
          pthread_mutex_unlock(&game_state.mutex);
          modal = TUI_MODAL_STARTUP_MENU;
          continue;
        }
        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          // Enter commits whatever the live-preview parse produced.
          // CGPs don't contain newlines, so there's no reason for
          // Enter to do anything else inside the input. If the last
          // parse failed, the error stays visible and Enter is a
          // no-op.
          if (load_position_parse_ok) {
            modal = TUI_MODAL_NONE;
          }
          continue;
        }
        if (key == NCKEY_LEFT) {
          if (load_position_cursor > 0) {
            load_position_cursor--;
          }
          continue;
        }
        if (key == NCKEY_RIGHT) {
          if (load_position_cursor < load_position_len) {
            load_position_cursor++;
          }
          continue;
        }
        if (key == NCKEY_HOME) {
          load_position_cursor = 0;
          continue;
        }
        if (key == NCKEY_END) {
          load_position_cursor = load_position_len;
          continue;
        }
        if (key == NCKEY_UP || key == NCKEY_DOWN) {
          // Walk one visual row in the wrapped layout. Visual rows
          // are formed by '\n' OR by reaching LOAD_POSITION_WRAP_W
          // cells — same logic the renderer uses to lay out the
          // input area, so cursor motion lines up with what the
          // user sees.
          const int wrap_w = LOAD_POSITION_WRAP_W;
          // Find the visual (row, col) of the current cursor.
          int cur_row = 0;
          int cur_col = 0;
          int target_offset = 0;
          for (int i = 0; i < load_position_cursor; i++) {
            if (load_position_buf[i] == '\n') {
              cur_row++;
              cur_col = 0;
              continue;
            }
            cur_col++;
            if (cur_col >= wrap_w) {
              cur_row++;
              cur_col = 0;
            }
          }
          const int desired_row = cur_row + (key == NCKEY_DOWN ? 1 : -1);
          if (desired_row < 0) {
            target_offset = 0;
          } else {
            // Walk again, this time landing at (desired_row, cur_col)
            // or end-of-row if shorter.
            int row = 0;
            int col = 0;
            int i = 0;
            target_offset = load_position_cursor; // default: stay put
            bool found = false;
            for (; i <= load_position_len; i++) {
              if (row == desired_row && col == cur_col) {
                target_offset = i;
                found = true;
                break;
              }
              if (i == load_position_len) {
                break;
              }
              if (load_position_buf[i] == '\n') {
                if (row == desired_row) {
                  target_offset = i;
                  found = true;
                  break;
                }
                row++;
                col = 0;
                continue;
              }
              col++;
              if (col >= wrap_w) {
                if (row == desired_row) {
                  target_offset = i + 1;
                  found = true;
                  break;
                }
                row++;
                col = 0;
              }
            }
            if (!found) {
              // Past the end — clamp to end of buffer.
              target_offset = load_position_len;
            }
          }
          load_position_cursor = target_offset;
          continue;
        }
        if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
          if (load_position_cursor > 0) {
            memmove(&load_position_buf[load_position_cursor - 1],
                    &load_position_buf[load_position_cursor],
                    (size_t)(load_position_len - load_position_cursor + 1));
            load_position_cursor--;
            load_position_len--;
            load_position_dirty = true;
          }
          continue;
        }
        if (key == NCKEY_DEL) {
          if (load_position_cursor < load_position_len) {
            memmove(&load_position_buf[load_position_cursor],
                    &load_position_buf[load_position_cursor + 1],
                    (size_t)(load_position_len - load_position_cursor));
            load_position_len--;
            load_position_dirty = true;
          }
          continue;
        }
        // See LOAD_GAME handler — translate Ctrl+letter into the
        // 0x01..0x1a ASCII range so the readline helper recognizes
        // the binding regardless of how the terminal encodes Ctrl.
        uint32_t rl_key_p = key;
        if ((input.modifiers & NCKEY_MOD_CTRL) || input.ctrl) {
          if (key >= 'a' && key <= 'z') {
            rl_key_p = key - 'a' + 1;
          } else if (key >= 'A' && key <= 'Z') {
            rl_key_p = key - 'A' + 1;
          }
        }
        if (tui_text_readline_key(rl_key_p, load_position_buf,
                                  &load_position_cursor, &load_position_len,
                                  &load_position_dirty)) {
          continue;
        }
        if (key >= 0x20 && key < 0x7f) {
          if (load_position_len + 1 < (int)sizeof(load_position_buf)) {
            memmove(&load_position_buf[load_position_cursor + 1],
                    &load_position_buf[load_position_cursor],
                    (size_t)(load_position_len - load_position_cursor + 1));
            load_position_buf[load_position_cursor] = (char)key;
            load_position_cursor++;
            load_position_len++;
            load_position_dirty = true;
          }
          continue;
        }
        continue;
      }

      if (modal == TUI_MODAL_LOAD_GAME) {
        // Mirrors the LOAD_POSITION handler: Esc cancels and resets
        // the previewed game; Enter commits when the live parse
        // succeeded; arrows / Home / End / Backspace / Del / printable
        // chars edit the buffer. The wrap column matches the
        // load-game modal's interior width so Up/Down line up with
        // visual rows. GCGs are real multi-line records — Enter
        // here also inserts a newline, not just submits, so users
        // can build one by hand.
        if (key == NCKEY_ESC) {
          pthread_mutex_lock(&game_state.mutex);
          game_reset(game_state.game);
          pthread_mutex_unlock(&game_state.mutex);
          modal = TUI_MODAL_STARTUP_MENU;
          continue;
        }
        if (key == NCKEY_LEFT) {
          if (load_game_cursor > 0) {
            load_game_cursor--;
          }
          continue;
        }
        if (key == NCKEY_RIGHT) {
          if (load_game_cursor < load_game_len) {
            load_game_cursor++;
          }
          continue;
        }
        if (key == NCKEY_HOME) {
          load_game_cursor = 0;
          continue;
        }
        if (key == NCKEY_END) {
          load_game_cursor = load_game_len;
          continue;
        }
        if (key == NCKEY_UP || key == NCKEY_DOWN) {
          const int wrap_w = LOAD_POSITION_WRAP_W;
          int cur_row = 0;
          int cur_col = 0;
          int target_offset = 0;
          for (int i = 0; i < load_game_cursor; i++) {
            if (load_game_buf[i] == '\n') {
              cur_row++;
              cur_col = 0;
              continue;
            }
            cur_col++;
            if (cur_col >= wrap_w) {
              cur_row++;
              cur_col = 0;
            }
          }
          const int desired_row = cur_row + (key == NCKEY_DOWN ? 1 : -1);
          if (desired_row < 0) {
            target_offset = 0;
          } else {
            int row = 0;
            int col = 0;
            int i = 0;
            target_offset = load_game_cursor;
            bool found = false;
            for (; i <= load_game_len; i++) {
              if (row == desired_row && col == cur_col) {
                target_offset = i;
                found = true;
                break;
              }
              if (i == load_game_len) {
                break;
              }
              if (load_game_buf[i] == '\n') {
                if (row == desired_row) {
                  target_offset = i;
                  found = true;
                  break;
                }
                row++;
                col = 0;
                continue;
              }
              col++;
              if (col >= wrap_w) {
                if (row == desired_row) {
                  target_offset = i + 1;
                  found = true;
                  break;
                }
                row++;
                col = 0;
              }
            }
            if (!found) {
              target_offset = load_game_len;
            }
          }
          load_game_cursor = target_offset;
          continue;
        }
        if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
          if (load_game_cursor > 0) {
            memmove(&load_game_buf[load_game_cursor - 1],
                    &load_game_buf[load_game_cursor],
                    (size_t)(load_game_len - load_game_cursor + 1));
            load_game_cursor--;
            load_game_len--;
            load_game_dirty = true;
          }
          continue;
        }
        if (key == NCKEY_DEL) {
          if (load_game_cursor < load_game_len) {
            memmove(&load_game_buf[load_game_cursor],
                    &load_game_buf[load_game_cursor + 1],
                    (size_t)(load_game_len - load_game_cursor));
            load_game_len--;
            load_game_dirty = true;
          }
          continue;
        }
        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          // The modal is single-line (path input only) — drag the
          // .gcg file from the Finder onto the window and the
          // terminal pastes its path. Pasting raw GCG content is
          // not supported (macOS Terminal's "warn before paste"
          // mitigation silently strips newlines, leaving the
          // parser unable to tokenize). Enter just submits when
          // the live-preview parse succeeded.
          if (load_game_parse_ok) {
            modal = TUI_MODAL_NONE;
          }
          continue;
        }
        // Translate Ctrl+letter into the corresponding ASCII control
        // code (0x01..0x1a) so the readline helper sees a uniform
        // input regardless of whether the terminal is using a
        // legacy raw-byte ctrl mapping or a modern protocol that
        // delivers ctrl as a modifier on the letter keycode.
        uint32_t rl_key_g = key;
        if ((input.modifiers & NCKEY_MOD_CTRL) || input.ctrl) {
          if (key >= 'a' && key <= 'z') {
            rl_key_g = key - 'a' + 1;
          } else if (key >= 'A' && key <= 'Z') {
            rl_key_g = key - 'A' + 1;
          }
        }
        if (tui_text_readline_key(rl_key_g, load_game_buf, &load_game_cursor,
                                  &load_game_len, &load_game_dirty)) {
          continue;
        }
        if (key >= 0x20 && key < 0x7f) {
          if (load_game_len + 1 < (int)sizeof(load_game_buf)) {
            memmove(&load_game_buf[load_game_cursor + 1],
                    &load_game_buf[load_game_cursor],
                    (size_t)(load_game_len - load_game_cursor + 1));
            load_game_buf[load_game_cursor] = (char)key;
            load_game_cursor++;
            load_game_len++;
            load_game_dirty = true;
          }
          continue;
        }
        continue;
      }

      if (modal == TUI_MODAL_WATCH_SETUP) {
        // Click on a setup row: select that row. For the "Start
        // game" row, also trigger commit (Enter). Other rows use
        // ←/→ to cycle values; click on the ◀ / ▶ chevrons fires
        // those adjusts directly so the user doesn't need to
        // switch to the keyboard.
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < TUI_WATCH_SETUP_ITEM_COUNT) {
            const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
            watch_setup_focus = hit;
            if (chev == TUI_MODAL_CHEVRON_LEFT) {
              key = NCKEY_LEFT;
            } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
              key = NCKEY_RIGHT;
            } else if (hit == TUI_WATCH_SETUP_START) {
              key = NCKEY_ENTER;
            } else {
              continue;
            }
          } else {
            continue;
          }
        }
        const bool key_up = key == NCKEY_UP || key == 'k' || key == 'K';
        const bool key_down = key == NCKEY_DOWN || key == 'j' || key == 'J';
        const bool key_left = key == NCKEY_LEFT || key == 'h' || key == 'H';
        const bool key_right = key == NCKEY_RIGHT || key == 'l' || key == 'L';
        if (key == NCKEY_ESC) {
          modal = TUI_MODAL_STARTUP_MENU;
          continue;
        }
        if (key_up) {
          if (watch_setup_focus > 0) {
            watch_setup_focus--;
          }
          continue;
        }
        if (key_down) {
          if (watch_setup_focus < TUI_WATCH_SETUP_ITEM_COUNT - 1) {
            watch_setup_focus++;
          }
          continue;
        }
        if (key_left || key_right) {
          const int dir = key_right ? 1 : -1;
          if (watch_setup_focus == TUI_WATCH_SETUP_TIME) {
            const int n = tui_time_picker_preset_count();
            const int cur = tui_time_picker_closest_index(watch_setup_time);
            int next = cur + dir;
            if (next < 0) {
              next = 0;
            }
            if (next >= n) {
              next = n - 1;
            }
            watch_setup_time = tui_time_picker_preset_seconds(next);
          } else if (watch_setup_focus == TUI_WATCH_SETUP_LANGUAGE) {
            // Cycle to the next/previous language group, snapping to
            // that group's first lexicon so the Lexicon row below
            // always shows a valid entry for the new language.
            // Mutates only the modal-local lexicon copy — committed
            // to the session on "Start game".
            if (lexicon_list == NULL) {
              lexicon_list = tui_lexicon_list_load();
            }
            if (lexicon_list != NULL) {
              int cur =
                  tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
              if (cur < 0) {
                cur = 0;
              }
              const int next =
                  tui_lexicon_list_step_language(lexicon_list, cur, dir);
              char buf[TUI_LEXICON_NAME_MAX];
              if (next != cur &&
                  tui_lexicon_list_name(lexicon_list, next, buf, sizeof(buf))) {
                snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                         buf);
              }
            }
          } else if (watch_setup_focus == TUI_WATCH_SETUP_LEXICON) {
            // Lazy-load the lexicon list on first use; the Settings
            // modal already keeps its own copy alive, so reuse it.
            // Cycling stays within the current language group — to
            // change language, use the Language row above. Mutates
            // only the modal-local copy.
            if (lexicon_list == NULL) {
              lexicon_list = tui_lexicon_list_load();
            }
            if (lexicon_list != NULL) {
              int cur =
                  tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
              if (cur < 0) {
                cur = 0;
              }
              const int next =
                  tui_lexicon_list_step_same_language(lexicon_list, cur, dir);
              char buf[TUI_LEXICON_NAME_MAX];
              if (next != cur &&
                  tui_lexicon_list_name(lexicon_list, next, buf, sizeof(buf))) {
                snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                         buf);
              }
            }
          } else if (watch_setup_focus == TUI_WATCH_SETUP_SIM_PLIES) {
            pthread_mutex_lock(&game_state.mutex);
            int v = game_state.sim_plies + dir;
            if (v < 1) {
              v = 1;
            }
            if (v > 1024) {
              v = 1024;
            }
            game_state.sim_plies = v;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (watch_setup_focus == TUI_WATCH_SETUP_SIM_CANDIDATES) {
            pthread_mutex_lock(&game_state.mutex);
            int v = game_state.sim_candidates + dir * 10;
            if (v < 2) {
              v = 2;
            }
            if (v > 1024) {
              v = 1024;
            }
            game_state.sim_candidates = v;
            pthread_mutex_unlock(&game_state.mutex);
          }
          continue;
        }
        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          if (watch_setup_focus == TUI_WATCH_SETUP_START) {
            // Commit the modal's local copies into the live session
            // settings. Before this point the adjusters touched
            // only watch_setup_lexicon / watch_setup_time, so an
            // Esc cancel leaves the underlying session untouched.
            chosen_time = watch_setup_time;
            snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                     watch_setup_lexicon);
            pthread_mutex_lock(&game_state.mutex);
            snprintf(game_state.pending_lexicon,
                     sizeof(game_state.pending_lexicon), "%s",
                     watch_setup_lexicon);
            pthread_mutex_unlock(&game_state.mutex);

            // Replicate the time-picker confirm path: stop any bot
            // that's running, reset state (re-init if lexicon /
            // RIT changed), kick off a fresh bot run with the
            // chosen settings.
            if (game_state.bot_started) {
              atomic_store(&game_state.bot_stop, true);
              pthread_join(game_state.bot_thread, NULL);
              game_state.bot_started = false;
              atomic_store(&game_state.bot_stop, false);
            }
            if (!args.no_config) {
              to_save.time_per_side_seconds = chosen_time;
              to_save.time_per_side_set = true;
              strncpy(to_save.lexicon, chosen_lexicon,
                      sizeof(to_save.lexicon) - 1);
              to_save.lexicon[sizeof(to_save.lexicon) - 1] = '\0';
              to_save.lexicon_set = true;
              tui_config_save(&to_save);
            }
            const bool needs_reinit =
                strcmp(game_state.pending_lexicon, game_state.active_lexicon) !=
                    0 ||
                game_state.pending_load_rit != game_state.active_load_rit;
            if (needs_reinit) {
              char new_lexicon[TUI_LEXICON_NAME_MAX];
              snprintf(new_lexicon, sizeof(new_lexicon), "%s",
                       game_state.pending_lexicon);
              const bool new_load_rit = game_state.pending_load_rit;
              const int saved_sim_plies = game_state.sim_plies;
              const int saved_sim_candidates = game_state.sim_candidates;
              tui_game_state_destroy(&game_state);
              char reinit_error[256] = {0};
              if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL),
                                       new_load_rit, &game_state, reinit_error,
                                       sizeof(reinit_error))) {
                if (!tui_game_state_init(chosen_lexicon, (uint64_t)time(NULL),
                                         initial_load_rit, &game_state,
                                         reinit_error, sizeof(reinit_error))) {
                  running = false;
                  modal = TUI_MODAL_NONE;
                  continue;
                }
              } else {
                snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                         new_lexicon);
              }
              // Preserve sim params across the destroy/init cycle —
              // tui_game_state_init resets them to defaults.
              game_state.sim_plies = saved_sim_plies;
              game_state.sim_candidates = saved_sim_candidates;
              tui_game_state_set_time_per_side(&game_state, chosen_time);
              // tui_game_state_init intentionally leaves the racks
              // empty so the startup menu can render an idle state
              // (Bag full, Racks empty). For a fresh watch game we
              // need real opening racks — same call the same-lexicon
              // branch below makes. Without this, both bots play
              // with empty racks, pass every turn, and the game
              // ends in 6 passes with the bag still at 100.
              pthread_mutex_lock(&game_state.mutex);
              tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
              pthread_mutex_unlock(&game_state.mutex);
            } else {
              pthread_mutex_lock(&game_state.mutex);
              tui_game_state_set_time_per_side(&game_state, chosen_time);
              tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
              pthread_mutex_unlock(&game_state.mutex);
            }
            pthread_mutex_lock(&game_state.mutex);
            game_state.app_mode = TUI_APP_MODE_WATCH;
            pthread_mutex_unlock(&game_state.mutex);
            tui_bot_worker_start(&game_state);
            modal = TUI_MODAL_NONE;
          }
          continue;
        }
        continue;
      }

      if (modal == TUI_MODAL_ANNOTATE_SETUP) {
        // Click anywhere on the modal: focus the clicked item.
        // Chevrons on the Lexicon row synthesize ←/→. The Start row
        // commits via synthesized Enter.
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < TUI_ANNOTATE_SETUP_ITEM_COUNT) {
            const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
            // Reset the name caret to end-of-text when switching to
            // a different name row so a fresh click on the row
            // doesn't strand the caret mid-word.
            if (hit != annotate_setup_focus) {
              if (hit == TUI_ANNOTATE_SETUP_P1_NAME) {
                annotate_setup_name_cursor =
                    (int)strlen(annotate_setup_p1_name);
              } else if (hit == TUI_ANNOTATE_SETUP_P2_NAME) {
                annotate_setup_name_cursor =
                    (int)strlen(annotate_setup_p2_name);
              }
            }
            annotate_setup_focus = hit;
            if (chev == TUI_MODAL_CHEVRON_LEFT) {
              key = NCKEY_LEFT;
            } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
              key = NCKEY_RIGHT;
            } else if (hit == TUI_ANNOTATE_SETUP_START) {
              key = NCKEY_ENTER;
            } else {
              continue;
            }
          } else {
            continue;
          }
        }

        const bool focus_p1 =
            annotate_setup_focus == TUI_ANNOTATE_SETUP_P1_NAME;
        const bool focus_p2 =
            annotate_setup_focus == TUI_ANNOTATE_SETUP_P2_NAME;
        const bool focus_name = focus_p1 || focus_p2;
        char *name_buf = focus_p1 ? annotate_setup_p1_name
                                  : (focus_p2 ? annotate_setup_p2_name : NULL);
        const size_t name_cap = focus_p1 ? sizeof(annotate_setup_p1_name)
                                         : sizeof(annotate_setup_p2_name);

        if (key == NCKEY_ESC) {
          modal = TUI_MODAL_STARTUP_MENU;
          continue;
        }
        if (key == NCKEY_UP || key == NCKEY_DOWN) {
          const int delta = key == NCKEY_UP ? -1 : 1;
          int next = annotate_setup_focus + delta;
          if (next < 0) {
            next = 0;
          }
          if (next >= TUI_ANNOTATE_SETUP_ITEM_COUNT) {
            next = TUI_ANNOTATE_SETUP_ITEM_COUNT - 1;
          }
          annotate_setup_focus = next;
          if (next == TUI_ANNOTATE_SETUP_P1_NAME) {
            annotate_setup_name_cursor = (int)strlen(annotate_setup_p1_name);
          } else if (next == TUI_ANNOTATE_SETUP_P2_NAME) {
            annotate_setup_name_cursor = (int)strlen(annotate_setup_p2_name);
          }
          continue;
        }
        // Tab / Shift-Tab cycles between the two name fields —
        // shortcuts for the common annotator workflow of entering
        // both nicknames in sequence. From a non-name row Tab
        // jumps to P1 (or P2 with Shift), so the first Tab from
        // the default Start focus drops you directly into a name
        // edit.
        if (key == NCKEY_TAB) {
          const bool shift = ncinput_shift_p(&input);
          int next;
          if (annotate_setup_focus == TUI_ANNOTATE_SETUP_P1_NAME) {
            next = TUI_ANNOTATE_SETUP_P2_NAME;
          } else if (annotate_setup_focus == TUI_ANNOTATE_SETUP_P2_NAME) {
            next = TUI_ANNOTATE_SETUP_P1_NAME;
          } else {
            next =
                shift ? TUI_ANNOTATE_SETUP_P2_NAME : TUI_ANNOTATE_SETUP_P1_NAME;
          }
          annotate_setup_focus = next;
          if (next == TUI_ANNOTATE_SETUP_P1_NAME) {
            annotate_setup_name_cursor = (int)strlen(annotate_setup_p1_name);
          } else {
            annotate_setup_name_cursor = (int)strlen(annotate_setup_p2_name);
          }
          continue;
        }
        if (annotate_setup_focus == TUI_ANNOTATE_SETUP_LEXICON &&
            (key == NCKEY_LEFT || key == NCKEY_RIGHT)) {
          const int dir = key == NCKEY_RIGHT ? 1 : -1;
          if (lexicon_list == NULL) {
            lexicon_list = tui_lexicon_list_load();
          }
          if (lexicon_list != NULL) {
            int cur =
                tui_lexicon_list_find(lexicon_list, annotate_setup_lexicon);
            if (cur < 0) {
              cur = 0;
            }
            const int next =
                tui_lexicon_list_step_same_language(lexicon_list, cur, dir);
            char namebuf[TUI_LEXICON_NAME_MAX];
            if (next != cur &&
                tui_lexicon_list_name(lexicon_list, next, namebuf,
                                      sizeof(namebuf))) {
              snprintf(annotate_setup_lexicon, sizeof(annotate_setup_lexicon),
                       "%s", namebuf);
            }
          }
          continue;
        }
        if (focus_name && name_buf != NULL) {
          const int len = (int)strlen(name_buf);
          if (key == NCKEY_LEFT) {
            if (annotate_setup_name_cursor > 0) {
              annotate_setup_name_cursor--;
            }
            continue;
          }
          if (key == NCKEY_RIGHT) {
            if (annotate_setup_name_cursor < len) {
              annotate_setup_name_cursor++;
            }
            continue;
          }
          if (key == NCKEY_HOME) {
            annotate_setup_name_cursor = 0;
            continue;
          }
          if (key == NCKEY_END) {
            annotate_setup_name_cursor = len;
            continue;
          }
          if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
            if (annotate_setup_name_cursor > 0) {
              memmove(name_buf + annotate_setup_name_cursor - 1,
                      name_buf + annotate_setup_name_cursor,
                      (size_t)(len - annotate_setup_name_cursor + 1));
              annotate_setup_name_cursor--;
            }
            continue;
          }
          if (key == NCKEY_DEL) {
            if (annotate_setup_name_cursor < len) {
              memmove(name_buf + annotate_setup_name_cursor,
                      name_buf + annotate_setup_name_cursor + 1,
                      (size_t)(len - annotate_setup_name_cursor));
            }
            continue;
          }
          if (key >= 0x20 && key < 0x7f && len + 1 < (int)name_cap) {
            memmove(name_buf + annotate_setup_name_cursor + 1,
                    name_buf + annotate_setup_name_cursor,
                    (size_t)(len - annotate_setup_name_cursor + 1));
            name_buf[annotate_setup_name_cursor] = (char)key;
            annotate_setup_name_cursor++;
            continue;
          }
        }
        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          // Enter on a non-Start row advances to the next field.
          // Enter on Start (or anywhere from the keyboard with focus
          // already on Start) commits.
          if (annotate_setup_focus != TUI_ANNOTATE_SETUP_START) {
            annotate_setup_focus++;
            if (annotate_setup_focus == TUI_ANNOTATE_SETUP_P1_NAME) {
              annotate_setup_name_cursor = (int)strlen(annotate_setup_p1_name);
            } else if (annotate_setup_focus == TUI_ANNOTATE_SETUP_P2_NAME) {
              annotate_setup_name_cursor = (int)strlen(annotate_setup_p2_name);
            }
            continue;
          }
          // Commit. Stop bot if running, reinit on lexicon change,
          // empty-board reset, set player names, append one
          // pending entry for P1, drop into annotation mode (no
          // bot started).
          if (game_state.bot_started) {
            atomic_store(&game_state.bot_stop, true);
            pthread_join(game_state.bot_thread, NULL);
            game_state.bot_started = false;
            atomic_store(&game_state.bot_stop, false);
          }
          snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                   annotate_setup_lexicon);
          pthread_mutex_lock(&game_state.mutex);
          snprintf(game_state.pending_lexicon,
                   sizeof(game_state.pending_lexicon), "%s",
                   annotate_setup_lexicon);
          pthread_mutex_unlock(&game_state.mutex);
          if (!args.no_config) {
            strncpy(to_save.lexicon, chosen_lexicon,
                    sizeof(to_save.lexicon) - 1);
            to_save.lexicon[sizeof(to_save.lexicon) - 1] = '\0';
            to_save.lexicon_set = true;
            tui_config_save(&to_save);
          }
          const bool needs_reinit =
              strcmp(game_state.pending_lexicon, game_state.active_lexicon) !=
                  0 ||
              game_state.pending_load_rit != game_state.active_load_rit;
          if (needs_reinit) {
            char new_lexicon[TUI_LEXICON_NAME_MAX];
            snprintf(new_lexicon, sizeof(new_lexicon), "%s",
                     game_state.pending_lexicon);
            const bool new_load_rit = game_state.pending_load_rit;
            const int saved_sim_plies = game_state.sim_plies;
            const int saved_sim_candidates = game_state.sim_candidates;
            tui_game_state_destroy(&game_state);
            char reinit_error[256] = {0};
            if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL),
                                     new_load_rit, &game_state, reinit_error,
                                     sizeof(reinit_error))) {
              if (!tui_game_state_init(chosen_lexicon, (uint64_t)time(NULL),
                                       initial_load_rit, &game_state,
                                       reinit_error, sizeof(reinit_error))) {
                running = false;
                modal = TUI_MODAL_NONE;
                continue;
              }
            } else {
              snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                       new_lexicon);
            }
            game_state.sim_plies = saved_sim_plies;
            game_state.sim_candidates = saved_sim_candidates;
            tui_game_state_set_time_per_side(&game_state, chosen_time);
          }
          pthread_mutex_lock(&game_state.mutex);
          game_state.app_mode = TUI_APP_MODE_ANNOTATE;
          tui_game_state_reset_game_for_annotation(&game_state);
          // Tear down all cached tile / arrow planes from the prior
          // game so the next render rebuilds them fresh against the
          // empty annotation board.
          tui_game_render_reset_grids();
          snprintf(game_state.player_names[0],
                   sizeof(game_state.player_names[0]), "%s",
                   annotate_setup_p1_name);
          snprintf(game_state.player_names[1],
                   sizeof(game_state.player_names[1]), "%s",
                   annotate_setup_p2_name);
          // Seed history with one pending entry for P1 so the
          // History panel reads "1." waiting for input. Rack is
          // NULL because the annotator will fill it in later.
          tui_bot_worker_append_pending_history(
              &game_state, 0, NULL, game_state.time_per_side_seconds);
          // Open the move editor on the seeded turn so the white
          // cursor lands in the move zone immediately — no extra
          // click needed before typing.
          game_state.edit_history_idx = 0;
          game_state.edit_field = TUI_EDIT_FIELD_MOVE;
          game_state.edit_move_buf[0] = '\0';
          game_state.edit_move_len = 0;
          game_state.edit_move_cursor = 0;
          game_state.edit_rack_buf[0] = '\0';
          game_state.edit_rack_len = 0;
          game_state.edit_rack_cursor = 0;
          game_state.edit_rack_user_modified = false;
          tui_game_state_parse_edit_buf(&game_state);
          game_state.focused_panel = TUI_FOCUS_HISTORY;
          game_state.history_cursor = 0;
          pthread_mutex_unlock(&game_state.mutex);
          // No tui_bot_worker_start — annotation mode is human-
          // driven. Move-entry / rack-entry UI will hook in later.
          modal = TUI_MODAL_NONE;
          continue;
        }
        continue;
      }

      if (modal == TUI_MODAL_PLAY_SETUP) {
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < TUI_PLAY_SETUP_ITEM_COUNT) {
            const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
            if (hit != play_setup_focus) {
              if (hit == TUI_PLAY_SETUP_HUMAN_NAME) {
                play_setup_name_cursor = (int)strlen(play_setup_human_name);
              } else if (hit == TUI_PLAY_SETUP_COMPUTER_NAME) {
                play_setup_name_cursor = (int)strlen(play_setup_computer_name);
              }
            }
            play_setup_focus = hit;
            if (hit == TUI_PLAY_SETUP_FIRST_MOVE &&
                chev == TUI_MODAL_CHEVRON_LEFT) {
              key = NCKEY_LEFT;
            } else if (hit == TUI_PLAY_SETUP_FIRST_MOVE &&
                       chev == TUI_MODAL_CHEVRON_RIGHT) {
              key = NCKEY_RIGHT;
            } else if (hit == TUI_PLAY_SETUP_START) {
              key = NCKEY_ENTER;
            } else {
              continue;
            }
          } else {
            continue;
          }
        }

        const bool focus_human = play_setup_focus == TUI_PLAY_SETUP_HUMAN_NAME;
        const bool focus_comp =
            play_setup_focus == TUI_PLAY_SETUP_COMPUTER_NAME;
        const bool focus_name = focus_human || focus_comp;
        char *name_buf = focus_human
                             ? play_setup_human_name
                             : (focus_comp ? play_setup_computer_name : NULL);
        const size_t name_cap = focus_human ? sizeof(play_setup_human_name)
                                            : sizeof(play_setup_computer_name);

        if (key == NCKEY_ESC) {
          modal = TUI_MODAL_STARTUP_MENU;
          startup_menu_focus = TUI_STARTUP_PLAY_VS_COMPUTER;
          continue;
        }
        if (key == NCKEY_UP || key == NCKEY_DOWN) {
          const int delta = key == NCKEY_UP ? -1 : 1;
          int next = play_setup_focus + delta;
          if (next < 0) {
            next = 0;
          }
          if (next >= TUI_PLAY_SETUP_ITEM_COUNT) {
            next = TUI_PLAY_SETUP_ITEM_COUNT - 1;
          }
          play_setup_focus = next;
          if (next == TUI_PLAY_SETUP_HUMAN_NAME) {
            play_setup_name_cursor = (int)strlen(play_setup_human_name);
          } else if (next == TUI_PLAY_SETUP_COMPUTER_NAME) {
            play_setup_name_cursor = (int)strlen(play_setup_computer_name);
          }
          continue;
        }
        if (key == NCKEY_TAB) {
          const bool shift = ncinput_shift_p(&input);
          int next = play_setup_focus + (shift ? -1 : 1);
          if (next < 0) {
            next = TUI_PLAY_SETUP_ITEM_COUNT - 1;
          }
          if (next >= TUI_PLAY_SETUP_ITEM_COUNT) {
            next = 0;
          }
          play_setup_focus = next;
          if (next == TUI_PLAY_SETUP_HUMAN_NAME) {
            play_setup_name_cursor = (int)strlen(play_setup_human_name);
          } else if (next == TUI_PLAY_SETUP_COMPUTER_NAME) {
            play_setup_name_cursor = (int)strlen(play_setup_computer_name);
          }
          continue;
        }
        if (!focus_name && (key == NCKEY_LEFT || key == NCKEY_RIGHT)) {
          const int dir = key == NCKEY_RIGHT ? 1 : -1;
          if (play_setup_focus == TUI_PLAY_SETUP_FIRST_MOVE) {
            play_setup_first_move =
                (play_setup_first_move + dir + TUI_PLAY_FIRST_COUNT) %
                TUI_PLAY_FIRST_COUNT;
          } else if (play_setup_focus == TUI_PLAY_SETUP_TIME) {
            const int n = tui_time_picker_preset_count();
            const int cur = tui_time_picker_closest_index(watch_setup_time);
            int next = cur + dir;
            if (next < 0) {
              next = 0;
            }
            if (next >= n) {
              next = n - 1;
            }
            watch_setup_time = tui_time_picker_preset_seconds(next);
          } else if (play_setup_focus == TUI_PLAY_SETUP_LANGUAGE) {
            if (lexicon_list == NULL) {
              lexicon_list = tui_lexicon_list_load();
            }
            if (lexicon_list != NULL) {
              int cur =
                  tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
              if (cur < 0) {
                cur = 0;
              }
              const int next =
                  tui_lexicon_list_step_language(lexicon_list, cur, dir);
              char namebuf[TUI_LEXICON_NAME_MAX];
              if (next != cur &&
                  tui_lexicon_list_name(lexicon_list, next, namebuf,
                                        sizeof(namebuf))) {
                snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                         namebuf);
              }
            }
          } else if (play_setup_focus == TUI_PLAY_SETUP_LEXICON) {
            if (lexicon_list == NULL) {
              lexicon_list = tui_lexicon_list_load();
            }
            if (lexicon_list != NULL) {
              int cur =
                  tui_lexicon_list_find(lexicon_list, watch_setup_lexicon);
              if (cur < 0) {
                cur = 0;
              }
              const int next =
                  tui_lexicon_list_step_same_language(lexicon_list, cur, dir);
              char namebuf[TUI_LEXICON_NAME_MAX];
              if (next != cur &&
                  tui_lexicon_list_name(lexicon_list, next, namebuf,
                                        sizeof(namebuf))) {
                snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                         namebuf);
              }
            }
          } else if (play_setup_focus == TUI_PLAY_SETUP_SIM_PLIES) {
            pthread_mutex_lock(&game_state.mutex);
            int v = game_state.sim_plies + dir;
            if (v < 1) {
              v = 1;
            }
            if (v > 1024) {
              v = 1024;
            }
            game_state.sim_plies = v;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (play_setup_focus == TUI_PLAY_SETUP_SIM_CANDIDATES) {
            pthread_mutex_lock(&game_state.mutex);
            int v = game_state.sim_candidates + dir * 10;
            if (v < 2) {
              v = 2;
            }
            if (v > 1024) {
              v = 1024;
            }
            game_state.sim_candidates = v;
            pthread_mutex_unlock(&game_state.mutex);
          }
          continue;
        }
        if (focus_name && name_buf != NULL) {
          const int len = (int)strlen(name_buf);
          if (key == NCKEY_LEFT) {
            if (play_setup_name_cursor > 0) {
              play_setup_name_cursor--;
            }
            continue;
          }
          if (key == NCKEY_RIGHT) {
            if (play_setup_name_cursor < len) {
              play_setup_name_cursor++;
            }
            continue;
          }
          if (key == NCKEY_HOME) {
            play_setup_name_cursor = 0;
            continue;
          }
          if (key == NCKEY_END) {
            play_setup_name_cursor = len;
            continue;
          }
          if (key == NCKEY_BACKSPACE || key == 0x7f || key == 0x08) {
            if (play_setup_name_cursor > 0) {
              memmove(name_buf + play_setup_name_cursor - 1,
                      name_buf + play_setup_name_cursor,
                      (size_t)(len - play_setup_name_cursor + 1));
              play_setup_name_cursor--;
            }
            continue;
          }
          if (key == NCKEY_DEL) {
            if (play_setup_name_cursor < len) {
              memmove(name_buf + play_setup_name_cursor,
                      name_buf + play_setup_name_cursor + 1,
                      (size_t)(len - play_setup_name_cursor));
            }
            continue;
          }
          if (key >= 0x20 && key < 0x7f && len + 1 < (int)name_cap) {
            memmove(name_buf + play_setup_name_cursor + 1,
                    name_buf + play_setup_name_cursor,
                    (size_t)(len - play_setup_name_cursor + 1));
            name_buf[play_setup_name_cursor] = (char)key;
            play_setup_name_cursor++;
            continue;
          }
        }
        if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          // Enter on a non-Start row advances to the next field; Enter
          // on Start launches the game.
          if (play_setup_focus != TUI_PLAY_SETUP_START) {
            play_setup_focus++;
            if (play_setup_focus == TUI_PLAY_SETUP_HUMAN_NAME) {
              play_setup_name_cursor = (int)strlen(play_setup_human_name);
            } else if (play_setup_focus == TUI_PLAY_SETUP_COMPUTER_NAME) {
              play_setup_name_cursor = (int)strlen(play_setup_computer_name);
            }
            continue;
          }
          // The engine always seats P1 on turn first, so "Human" means
          // the human is P1 (index 0); "Computer" makes the human P2;
          // "Random" flips a coin.
          int human_idx;
          if (play_setup_first_move == TUI_PLAY_FIRST_HUMAN) {
            human_idx = 0;
          } else if (play_setup_first_move == TUI_PLAY_FIRST_COMPUTER) {
            human_idx = 1;
          } else {
            human_idx = (int)((uint64_t)time(NULL) & 1ULL);
          }
          const char *hn =
              play_setup_human_name[0] != '\0' ? play_setup_human_name : "You";
          const char *cn = play_setup_computer_name[0] != '\0'
                               ? play_setup_computer_name
                               : "Computer";
          // Commit the modal's scratch time / lexicon into the session.
          chosen_time = watch_setup_time;
          snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                   watch_setup_lexicon);
          pthread_mutex_lock(&game_state.mutex);
          snprintf(game_state.pending_lexicon,
                   sizeof(game_state.pending_lexicon), "%s",
                   watch_setup_lexicon);
          pthread_mutex_unlock(&game_state.mutex);
          // Stop any running bot before reconfiguring the game.
          if (game_state.bot_started) {
            atomic_store(&game_state.bot_stop, true);
            pthread_join(game_state.bot_thread, NULL);
            game_state.bot_started = false;
            atomic_store(&game_state.bot_stop, false);
          }
          if (!args.no_config) {
            to_save.time_per_side_seconds = chosen_time;
            to_save.time_per_side_set = true;
            strncpy(to_save.lexicon, chosen_lexicon,
                    sizeof(to_save.lexicon) - 1);
            to_save.lexicon[sizeof(to_save.lexicon) - 1] = '\0';
            to_save.lexicon_set = true;
            tui_config_save(&to_save);
          }
          const bool play_needs_reinit =
              strcmp(game_state.pending_lexicon, game_state.active_lexicon) !=
                  0 ||
              game_state.pending_load_rit != game_state.active_load_rit;
          if (play_needs_reinit) {
            char new_lexicon[TUI_LEXICON_NAME_MAX];
            snprintf(new_lexicon, sizeof(new_lexicon), "%s",
                     game_state.pending_lexicon);
            const bool new_load_rit = game_state.pending_load_rit;
            const int saved_sim_plies = game_state.sim_plies;
            const int saved_sim_candidates = game_state.sim_candidates;
            tui_game_state_destroy(&game_state);
            char reinit_error[256] = {0};
            if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL),
                                     new_load_rit, &game_state, reinit_error,
                                     sizeof(reinit_error))) {
              if (!tui_game_state_init(chosen_lexicon, (uint64_t)time(NULL),
                                       initial_load_rit, &game_state,
                                       reinit_error, sizeof(reinit_error))) {
                running = false;
                modal = TUI_MODAL_NONE;
                continue;
              }
            } else {
              snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                       new_lexicon);
            }
            game_state.sim_plies = saved_sim_plies;
            game_state.sim_candidates = saved_sim_candidates;
          }
          pthread_mutex_lock(&game_state.mutex);
          tui_game_state_set_time_per_side(&game_state, chosen_time);
          tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
          game_state.app_mode = TUI_APP_MODE_PLAY_VS_COMPUTER;
          game_state.human_player_idx = human_idx;
          snprintf(game_state.player_names[human_idx],
                   sizeof(game_state.player_names[human_idx]), "%s", hn);
          snprintf(game_state.player_names[1 - human_idx],
                   sizeof(game_state.player_names[1 - human_idx]), "%s", cn);
          game_state.history_cursor = -1;
          game_state.focused_panel = TUI_FOCUS_BOARD;
          pthread_mutex_unlock(&game_state.mutex);
          // Rebuild cached tile / arrow planes against the fresh board.
          tui_game_render_reset_grids();
          // Start the bot — it idles on the human's turn and plays the
          // computer's.
          tui_bot_worker_start(&game_state);
          modal = TUI_MODAL_NONE;
          continue;
        }
        continue;
      }

      if (modal == TUI_MODAL_STARTUP_MENU) {
        // Helper: which menu items are currently selectable. Only
        // "Watch computer play" is wired up; others render dimmed
        // and the cursor skips past them. Keep this aligned with
        // the disabled mask inside tui_game_render_startup_menu.
        bool su_enabled[TUI_STARTUP_ITEM_COUNT];
        su_enabled[TUI_STARTUP_WATCH] = true;
        su_enabled[TUI_STARTUP_LOAD_POSITION] = true;
        su_enabled[TUI_STARTUP_LOAD_GAME] = true;
        su_enabled[TUI_STARTUP_ANNOTATE] = true;
        su_enabled[TUI_STARTUP_PLAY_VS_COMPUTER] = true;
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < TUI_STARTUP_ITEM_COUNT && su_enabled[hit]) {
            startup_menu_focus = hit;
            key = NCKEY_ENTER;
          } else {
            continue;
          }
        }
        if (key == NCKEY_ESC) {
          // Esc returns to whichever modal opened the startup menu.
          // First-launch: TUI_MODAL_NONE (dismisses to the bot game
          // already running underneath). Esc → New game: returns to
          // TUI_MODAL_MAIN_MENU so the user can pick Settings/Quit.
          modal = startup_menu_return;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          for (int i = startup_menu_focus - 1; i >= 0; i--) {
            if (su_enabled[i]) {
              startup_menu_focus = i;
              break;
            }
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          for (int i = startup_menu_focus + 1; i < TUI_STARTUP_ITEM_COUNT;
               i++) {
            if (su_enabled[i]) {
              startup_menu_focus = i;
              break;
            }
          }
        } else if (key == 'w' || key == 'W') {
          // Mnemonic shortcut: open the Watch setup modal. The setup
          // modal handles starting the game once the user confirms.
          modal = TUI_MODAL_WATCH_SETUP;
          snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                   chosen_lexicon);
          watch_setup_time = chosen_time;
        } else if (key == 'p' || key == 'P') {
          modal = TUI_MODAL_LOAD_POSITION;
          load_position_buf[0] = '\0';
          load_position_len = 0;
          load_position_cursor = 0;
          load_position_parse_ok = false;
          load_position_dirty = false;
          load_position_error[0] = '\0';
        } else if (key == 'g' || key == 'G') {
          modal = TUI_MODAL_LOAD_GAME;
          load_game_buf[0] = '\0';
          load_game_len = 0;
          load_game_cursor = 0;
          load_game_parse_ok = false;
          load_game_dirty = false;
          load_game_error[0] = '\0';
        } else if (key == 'a' || key == 'A') {
          modal = TUI_MODAL_ANNOTATE_SETUP;
          snprintf(annotate_setup_lexicon, sizeof(annotate_setup_lexicon), "%s",
                   chosen_lexicon);
          snprintf(annotate_setup_p1_name, sizeof(annotate_setup_p1_name),
                   "Player 1");
          snprintf(annotate_setup_p2_name, sizeof(annotate_setup_p2_name),
                   "Player 2");
          annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
          annotate_setup_name_cursor = (int)strlen(annotate_setup_p1_name);
        } else if (key == 'c' || key == 'C') {
          modal = TUI_MODAL_PLAY_SETUP;
          play_setup_focus = TUI_PLAY_SETUP_START;
          snprintf(play_setup_human_name, sizeof(play_setup_human_name), "You");
          snprintf(play_setup_computer_name, sizeof(play_setup_computer_name),
                   "Computer");
          play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
          play_setup_name_cursor = 0;
          snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                   chosen_lexicon);
          watch_setup_time = chosen_time;
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          if (startup_menu_focus == TUI_STARTUP_WATCH) {
            modal = TUI_MODAL_WATCH_SETUP;
            snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                     chosen_lexicon);
            watch_setup_time = chosen_time;
          } else if (startup_menu_focus == TUI_STARTUP_LOAD_POSITION) {
            modal = TUI_MODAL_LOAD_POSITION;
            load_position_buf[0] = '\0';
            load_position_len = 0;
            load_position_cursor = 0;
            load_position_parse_ok = false;
            load_position_dirty = false;
            load_position_error[0] = '\0';
          } else if (startup_menu_focus == TUI_STARTUP_LOAD_GAME) {
            modal = TUI_MODAL_LOAD_GAME;
            load_game_buf[0] = '\0';
            load_game_len = 0;
            load_game_cursor = 0;
            load_game_parse_ok = false;
            load_game_dirty = false;
            load_game_error[0] = '\0';
          } else if (startup_menu_focus == TUI_STARTUP_ANNOTATE) {
            modal = TUI_MODAL_ANNOTATE_SETUP;
            snprintf(annotate_setup_lexicon, sizeof(annotate_setup_lexicon),
                     "%s", chosen_lexicon);
            snprintf(annotate_setup_p1_name, sizeof(annotate_setup_p1_name),
                     "Player 1");
            snprintf(annotate_setup_p2_name, sizeof(annotate_setup_p2_name),
                     "Player 2");
            annotate_setup_focus = TUI_ANNOTATE_SETUP_P1_NAME;
            annotate_setup_name_cursor = (int)strlen(annotate_setup_p1_name);
          } else if (startup_menu_focus == TUI_STARTUP_PLAY_VS_COMPUTER) {
            // Single play-vs-computer setup modal: names, who moves
            // first, time, lexicon, and sim (computer-strength) params.
            // Reuses watch_setup_time / watch_setup_lexicon as the scratch
            // copies for the time / lexicon adjusters.
            modal = TUI_MODAL_PLAY_SETUP;
            play_setup_focus = TUI_PLAY_SETUP_START;
            snprintf(play_setup_human_name, sizeof(play_setup_human_name),
                     "You");
            snprintf(play_setup_computer_name, sizeof(play_setup_computer_name),
                     "Computer");
            play_setup_first_move = TUI_PLAY_FIRST_RANDOM;
            play_setup_name_cursor = 0;
            snprintf(watch_setup_lexicon, sizeof(watch_setup_lexicon), "%s",
                     chosen_lexicon);
            watch_setup_time = chosen_time;
          }
          // Disabled items are no-op for now. As each mode ships,
          // add its branch here and flip su_enabled[i] true above.
        }
        continue;
      }

      if (modal == TUI_MODAL_MAIN_MENU) {
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0) {
            main_menu_focus = hit;
            key = NCKEY_ENTER;
          } else {
            continue;
          }
        }
        if (key == NCKEY_ESC) {
          modal = TUI_MODAL_NONE;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          if (main_menu_focus > 0) {
            main_menu_focus--;
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          if (main_menu_focus < TUI_MENU_ITEM_COUNT - 1) {
            main_menu_focus++;
          }
        } else if (key == 'n' || key == 'N') {
          // Mnemonic shortcuts trigger the action immediately, matching
          // the hint shown to the right of each item in the modal. They
          // skip focus-then-Enter so the menu behaves like a launcher.
          // New game now routes through the startup menu so the user
          // can pick load/annotate modes alongside watch.
          modal = TUI_MODAL_STARTUP_MENU;
          startup_menu_focus = TUI_STARTUP_WATCH;
          startup_menu_return = TUI_MODAL_MAIN_MENU;
        } else if (key == 's' || key == 'S') {
          modal = TUI_MODAL_SETTINGS;
          settings_focus = 0;
          settings_return = TUI_MODAL_MAIN_MENU;
        } else if (key == 'q' || key == 'Q') {
          modal = TUI_MODAL_QUIT_CONFIRM;
          quit_confirm_focus = 0;
          quit_confirm_return = TUI_MODAL_MAIN_MENU;
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          if (main_menu_focus == TUI_MENU_NEW_GAME) {
            // Pivot to the startup menu so the user can pick what
            // KIND of new game (watch / load / annotate / vs-cpu) —
            // Watch from there opens the time picker and ends up
            // doing what this branch used to do directly.
            modal = TUI_MODAL_STARTUP_MENU;
            startup_menu_focus = TUI_STARTUP_WATCH;
            startup_menu_return = TUI_MODAL_MAIN_MENU;
          } else if (main_menu_focus == TUI_MENU_SETTINGS) {
            modal = TUI_MODAL_SETTINGS;
            settings_focus = 0;
            settings_return = TUI_MODAL_MAIN_MENU;
          } else if (main_menu_focus == TUI_MENU_QUIT) {
            modal = TUI_MODAL_QUIT_CONFIRM;
            quit_confirm_focus = 0;
            quit_confirm_return = TUI_MODAL_MAIN_MENU;
          } else if (main_menu_focus == TUI_MENU_BACK) {
            modal = TUI_MODAL_NONE;
          }
        }
        continue;
      }

      if (modal == TUI_MODAL_SETTINGS) {
        // Click on a settings row: select that row. Settings uses
        // ←/→ to adjust values rather than Enter, so a click just
        // changes focus — it doesn't trigger a value change. The
        // "Back" row is the exception; for it we synthesize an
        // Enter so the click commits. A click on the ◀ / ▶
        // chevrons of an already-focused row synthesizes ← / →
        // so the adjuster fires.
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0) {
            const TuiModalChevron chev = tui_modal_chevron_at(input.y, input.x);
            settings_focus = hit;
            if (chev == TUI_MODAL_CHEVRON_LEFT) {
              key = NCKEY_LEFT;
            } else if (chev == TUI_MODAL_CHEVRON_RIGHT) {
              key = NCKEY_RIGHT;
            } else if (hit == TUI_SETTINGS_BACK) {
              key = NCKEY_ENTER;
            } else {
              continue;
            }
          } else {
            continue;
          }
        }
        // Any left/right keystroke at this modal might mutate visual
        // state (scale, AA, border, premium labels, blanks). Bumping
        // render_version once at the top invalidates the pixel-blit
        // caches that key off it, regardless of which branch below
        // actually toggled something — cheap and avoids scattering
        // atomic_fetch_add through every handler.
        if (key == NCKEY_LEFT || key == 'h' || key == 'H' ||
            key == NCKEY_RIGHT || key == 'l' || key == 'L') {
          atomic_fetch_add(&game_state.render_version, 1);
        }
        if (key == NCKEY_ESC) {
          // Esc returns to whichever modal opened Settings — main menu
          // when reached via Esc → Settings (so the user can navigate
          // to another menu entry without re-opening from scratch), or
          // back to no modal when reached via the command-bar S
          // shortcut.
          modal = settings_return;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          const bool effective_2x =
              pixel_supported && font_available && game_state.board_scale >= 2;
          int idx = settings_focus - 1;
          while (idx > 0 && SETTINGS_2X_ONLY(idx) && !effective_2x) {
            idx--;
          }
          if (idx >= 0) {
            settings_focus = idx;
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          const bool effective_2x =
              pixel_supported && font_available && game_state.board_scale >= 2;
          int idx = settings_focus + 1;
          while (idx < TUI_SETTINGS_ITEM_COUNT - 1 && SETTINGS_2X_ONLY(idx) &&
                 !effective_2x) {
            idx++;
          }
          if (idx < TUI_SETTINGS_ITEM_COUNT) {
            settings_focus = idx;
          }
        } else if (key == NCKEY_LEFT || key == 'h' || key == 'H') {
          if (settings_focus == TUI_SETTINGS_SCALE && pixel_supported &&
              font_available) {
            // Scale is a 2-state toggle (1, 2). Both arrows flip it.
            pthread_mutex_lock(&game_state.mutex);
            game_state.board_scale = game_state.board_scale == 2 ? 1 : 2;
            const int v = game_state.board_scale;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.board_scale = v;
              to_save.board_scale_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_AA && pixel_supported &&
                     font_available && game_state.board_scale >= 2) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.antialias = !game_state.antialias;
            const bool v = game_state.antialias;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.antialias = v;
              to_save.antialias_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                     pixel_supported && font_available &&
                     game_state.board_scale >= 2) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.score_subscripts =
                (TuiScoreSubscripts)((game_state.score_subscripts +
                                      TUI_SCORE_SUBSCRIPTS_COUNT - 1) %
                                     TUI_SCORE_SUBSCRIPTS_COUNT);
            const TuiScoreSubscripts v = game_state.score_subscripts;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.score_subscripts = v;
              to_save.score_subscripts_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_BORDER && pixel_supported) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.border_thickness > 0) {
              game_state.border_thickness--;
            }
            const int v = game_state.border_thickness;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.border_thickness = v;
              to_save.border_thickness_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_PREMIUM) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.premium_labels =
                (TuiPremiumLabels)((game_state.premium_labels +
                                    TUI_PREMIUM_LABELS_COUNT - 1) %
                                   TUI_PREMIUM_LABELS_COUNT);
            const TuiPremiumLabels v = game_state.premium_labels;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.premium_labels = v;
              to_save.premium_labels_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_BLANKS) {
            // Blanks is a two-state toggle, so left and right both flip it.
            pthread_mutex_lock(&game_state.mutex);
            game_state.blank_uppercase = !game_state.blank_uppercase;
            const bool v = game_state.blank_uppercase;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.blank_uppercase = v;
              to_save.blank_uppercase_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_RACK_SORT) {
            pthread_mutex_lock(&game_state.mutex);
            int v = (int)game_state.rack_sort - 1;
            if (v < 0) {
              v = TUI_RACK_SORT_COUNT - 1;
            }
            game_state.rack_sort = (TuiRackSort)v;
            const TuiRackSort saved = game_state.rack_sort;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.rack_sort = saved;
              to_save.rack_sort_set = true;
              tui_config_save(&to_save);
            }
          }
        } else if (key == NCKEY_RIGHT || key == 'l' || key == 'L') {
          if (settings_focus == TUI_SETTINGS_SCALE && pixel_supported &&
              font_available) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.board_scale = game_state.board_scale == 2 ? 1 : 2;
            const int v = game_state.board_scale;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.board_scale = v;
              to_save.board_scale_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_AA && pixel_supported &&
                     font_available && game_state.board_scale >= 2) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.antialias = !game_state.antialias;
            const bool v = game_state.antialias;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.antialias = v;
              to_save.antialias_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_SUBSCRIPTS &&
                     pixel_supported && font_available &&
                     game_state.board_scale >= 2) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.score_subscripts =
                (TuiScoreSubscripts)((game_state.score_subscripts + 1) %
                                     TUI_SCORE_SUBSCRIPTS_COUNT);
            const TuiScoreSubscripts v = game_state.score_subscripts;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.score_subscripts = v;
              to_save.score_subscripts_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_BORDER && pixel_supported) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.border_thickness < 6) {
              game_state.border_thickness++;
            }
            const int v = game_state.border_thickness;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.border_thickness = v;
              to_save.border_thickness_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_PREMIUM) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.premium_labels =
                (TuiPremiumLabels)((game_state.premium_labels + 1) %
                                   TUI_PREMIUM_LABELS_COUNT);
            const TuiPremiumLabels v = game_state.premium_labels;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.premium_labels = v;
              to_save.premium_labels_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_BLANKS) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.blank_uppercase = !game_state.blank_uppercase;
            const bool v = game_state.blank_uppercase;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.blank_uppercase = v;
              to_save.blank_uppercase_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_RACK_SORT) {
            pthread_mutex_lock(&game_state.mutex);
            int v = (int)game_state.rack_sort + 1;
            if (v >= TUI_RACK_SORT_COUNT) {
              v = 0;
            }
            game_state.rack_sort = (TuiRackSort)v;
            const TuiRackSort saved = game_state.rack_sort;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              to_save.rack_sort = saved;
              to_save.rack_sort_set = true;
              tui_config_save(&to_save);
            }
          } else if (settings_focus == TUI_SETTINGS_RIT) {
            // RIT is a deferred toggle — write to config; the live
            // game keeps using whatever was loaded at game-state init.
            // The pending-change banner picks up the divergence and the
            // setting takes effect on the next New Game.
            const bool prev =
                to_save.load_rit_set ? to_save.load_rit : initial_load_rit;
            to_save.load_rit = !prev;
            to_save.load_rit_set = true;
            pthread_mutex_lock(&game_state.mutex);
            game_state.pending_load_rit = to_save.load_rit;
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              tui_config_save(&to_save);
            }
          }
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          if (settings_focus == TUI_SETTINGS_BACK) {
            modal = settings_return;
          }
        }
        continue;
      }

      if (modal == TUI_MODAL_TIME_PICKER) {
        const int preset_count = tui_time_picker_preset_count();
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < preset_count) {
            time_focus = hit;
            key = NCKEY_ENTER;
          } else {
            continue;
          }
        }
        if (key == NCKEY_ESC) {
          modal = time_picker_return;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          if (time_focus > 0) {
            time_focus--;
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          if (time_focus < preset_count - 1) {
            time_focus++;
          }
        } else if (key >= '1' && key <= (uint32_t)('0' + preset_count)) {
          time_focus = (int)(key - '1');
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          const int new_time = tui_time_picker_preset_seconds(time_focus);
          if (new_time > 0) {
            // Stop the bot only when one is actually running. At first
            // launch the bot is idle (waiting on the startup menu), so
            // the pthread_join would block forever on a never-started
            // thread.
            if (game_state.bot_started) {
              atomic_store(&game_state.bot_stop, true);
              pthread_join(game_state.bot_thread, NULL);
              game_state.bot_started = false;
              atomic_store(&game_state.bot_stop, false);
            }
            chosen_time = new_time;
            if (!args.no_config) {
              to_save.time_per_side_seconds = new_time;
              to_save.time_per_side_set = true;
              tui_config_save(&to_save);
            }
            // Pending lexicon / RIT changes that need a full re-init?
            // If so, tear down the state and re-init with the new
            // settings (loading fresh tables). Otherwise the fast in-
            // place reset is enough.
            const bool needs_reinit =
                strcmp(game_state.pending_lexicon, game_state.active_lexicon) !=
                    0 ||
                game_state.pending_load_rit != game_state.active_load_rit;
            if (needs_reinit) {
              char new_lexicon[TUI_LEXICON_NAME_MAX];
              snprintf(new_lexicon, sizeof(new_lexicon), "%s",
                       game_state.pending_lexicon);
              const bool new_load_rit = game_state.pending_load_rit;
              tui_game_state_destroy(&game_state);
              char reinit_error[256] = {0};
              if (!tui_game_state_init(new_lexicon, (uint64_t)time(NULL),
                                       new_load_rit, &game_state, reinit_error,
                                       sizeof(reinit_error))) {
                // Re-init failed; fall back to the previously active
                // settings so the user isn't left without a playable
                // game. We've already torn the state down, so we have
                // to retry with the old values.
                if (!tui_game_state_init(chosen_lexicon, (uint64_t)time(NULL),
                                         initial_load_rit, &game_state,
                                         reinit_error, sizeof(reinit_error))) {
                  running = false;
                  modal = TUI_MODAL_NONE;
                  continue;
                }
              } else {
                snprintf(chosen_lexicon, sizeof(chosen_lexicon), "%s",
                         new_lexicon);
              }
              tui_game_state_set_time_per_side(&game_state, new_time);
            } else {
              pthread_mutex_lock(&game_state.mutex);
              tui_game_state_set_time_per_side(&game_state, new_time);
              tui_game_state_reset_game(&game_state, (uint64_t)time(NULL));
              pthread_mutex_unlock(&game_state.mutex);
            }
            pthread_mutex_lock(&game_state.mutex);
            game_state.app_mode = TUI_APP_MODE_WATCH;
            pthread_mutex_unlock(&game_state.mutex);
            tui_bot_worker_start(&game_state);
          }
          modal = TUI_MODAL_NONE;
        }
        continue;
      }

      if (modal == TUI_MODAL_LEXICON_PICKER) {
        const int n = tui_lexicon_list_count(lexicon_list);
        if (key == NCKEY_ESC) {
          modal = TUI_MODAL_SETTINGS;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          if (lexicon_focus > 0) {
            lexicon_focus--;
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          if (lexicon_focus < n - 1) {
            lexicon_focus++;
          }
        } else if (key == NCKEY_HOME || key == 'g') {
          lexicon_focus = 0;
        } else if (key == NCKEY_END || key == 'G') {
          lexicon_focus = n - 1;
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          char picked[TUI_LEXICON_NAME_MAX] = {0};
          if (tui_lexicon_list_name(lexicon_list, lexicon_focus, picked,
                                    sizeof(picked))) {
            snprintf(to_save.lexicon, sizeof(to_save.lexicon), "%s", picked);
            to_save.lexicon_set = true;
            pthread_mutex_lock(&game_state.mutex);
            snprintf(game_state.pending_lexicon,
                     sizeof(game_state.pending_lexicon), "%s", picked);
            pthread_mutex_unlock(&game_state.mutex);
            if (!args.no_config) {
              tui_config_save(&to_save);
            }
          }
          modal = TUI_MODAL_SETTINGS;
        }
        continue;
      }

      if (modal == TUI_MODAL_QUIT_CONFIRM) {
        // Y / N shortcuts trigger their action regardless of focus.
        // Enter confirms whatever's focused (default No, the safer
        // option). Esc / N returns to whichever modal opened the
        // confirm (main menu when launched via Quit there, or NONE
        // when launched via the command-bar Q).
        if (key == NCKEY_BUTTON1 && input.evtype != NCTYPE_RELEASE) {
          const int hit = tui_modal_item_at(input.y, input.x);
          if (hit >= 0 && hit < 2) {
            quit_confirm_focus = hit;
            key = NCKEY_ENTER;
          } else {
            continue;
          }
        }
        if (key == NCKEY_ESC) {
          modal = quit_confirm_return;
        } else if (key == 'y' || key == 'Y') {
          running = false;
        } else if (key == 'n' || key == 'N') {
          modal = quit_confirm_return;
        } else if (key == NCKEY_UP || key == 'k' || key == 'K') {
          if (quit_confirm_focus > 0) {
            quit_confirm_focus--;
          }
        } else if (key == NCKEY_DOWN || key == 'j' || key == 'J') {
          if (quit_confirm_focus < 1) {
            quit_confirm_focus++;
          }
        } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
          if (quit_confirm_focus == 1) {
            running = false;
          } else {
            modal = quit_confirm_return;
          }
        }
        continue;
      }

      // When the History cursor sits on a pending entry and the
      // user hasn't opened the editor yet, Tab / Enter / → / ↓ all
      // drop into the move cell. Up / Left stay reserved for
      // entry-to-entry navigation. This is the keyboard mirror of
      // the click-to-edit path — annotation users shouldn't need
      // the mouse to start typing the next field.
      if (modal == TUI_MODAL_NONE && game_state.edit_history_idx < 0 &&
          game_state.focused_panel == TUI_FOCUS_HISTORY &&
          game_state.history_cursor >= 0 &&
          game_state.history_cursor < game_state.history_count &&
          game_state.history[game_state.history_cursor].pending &&
          (key == NCKEY_TAB || key == '\t' || key == NCKEY_ENTER ||
           key == '\r' || key == '\n' || key == NCKEY_RIGHT ||
           key == NCKEY_DOWN)) {
        pthread_mutex_lock(&game_state.mutex);
        const int target = game_state.history_cursor;
        const TuiHistoryEntry *e = &game_state.history[target];
        // Seed the buffers from the entry only when nothing has
        // been typed yet — preserves any in-flight text the user
        // had left in the buffer.
        if (game_state.edit_move_len == 0) {
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
        }
        if (game_state.edit_rack_len == 0) {
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          // Committed rack text on the entry is treated as
          // user-authored: don't snap it back to "match the
          // move's inferred letters" on the next keystroke.
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
        }
        game_state.edit_history_idx = target;
        // ↓ lands on the RACK row (mirrors the inside-edit
        // semantics where ↓ in MOVE switches to RACK). Everything
        // else opens the MOVE field. In play-vs-computer the rack
        // row is read-only (racks come from the bag), so ↓ opens
        // MOVE like everything else.
        game_state.edit_field =
            (key == NCKEY_DOWN &&
             game_state.app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER)
                ? TUI_EDIT_FIELD_RACK
                : TUI_EDIT_FIELD_MOVE;
        tui_game_state_parse_edit_buf(&game_state);
        pthread_mutex_unlock(&game_state.mutex);
        continue;
      }

      if (key == NCKEY_ESC) {
        modal = TUI_MODAL_MAIN_MENU;
        main_menu_focus = 0;
      } else if (key >= '0' && key <= '5') {
        // Direct panel focus hotkeys (no modal). '0' focuses the
        // command bar; '1'-'5' focus the corresponding panel. These
        // work globally regardless of which panel is currently
        // focused — typing digits never gets captured by a panel.
        pthread_mutex_lock(&game_state.mutex);
        const int new_focus = (int)(key - '0');
        if (new_focus != 0 && game_state.slash_active) {
          game_state.slash_active = false;
          game_state.slash_len = 0;
          game_state.slash_cursor = 0;
          game_state.slash_buf[0] = '\0';
        }
        game_state.focused_panel = new_focus;
        pthread_mutex_unlock(&game_state.mutex);
      } else if ((key == NCKEY_TAB || key == '\t') &&
                 !game_state.slash_active) {
        // Tab cycles forward through 0..5 (Command → Board → ... →
        // Analysis → Command); Shift-Tab cycles the other way.
        // Useful as a discoverability path — press Tab repeatedly to
        // walk every focus state. While slash mode is active Tab
        // means "autocomplete" instead, so it falls through to the
        // [0]-focused handler below.
        pthread_mutex_lock(&game_state.mutex);
        const int delta = ncinput_shift_p(&input) ? 5 : 1; // 5 = (-1 mod 6)
        const int new_focus = (game_state.focused_panel + delta) % 6;
        if (new_focus != 0 && game_state.slash_active) {
          game_state.slash_active = false;
          game_state.slash_len = 0;
          game_state.slash_cursor = 0;
          game_state.slash_buf[0] = '\0';
        }
        game_state.focused_panel = new_focus;
        pthread_mutex_unlock(&game_state.mutex);
      } else if (key == '/' && !game_state.slash_active) {
        // Global "/" — focuses [0] Command if it isn't already and
        // begins a slash-command. Lets the user kick off a command
        // from any panel without first pressing 0 / Tab to land on
        // the command bar.
        pthread_mutex_lock(&game_state.mutex);
        game_state.focused_panel = 0;
        game_state.slash_active = true;
        game_state.slash_len = 0;
        game_state.slash_cursor = 0;
        game_state.slash_buf[0] = '\0';
        pthread_mutex_unlock(&game_state.mutex);
      } else if ((key == 'c' || key == 'C') && !game_state.slash_active) {
        // Global copy hotkey: current position → clipboard as CGP.
        // Works from any panel focus; editing contexts (board entry,
        // history cells, slash input, modals) consume their keys
        // before this chain so a typed 'c' never lands here.
        pthread_mutex_lock(&game_state.mutex);
        tui_copy_position_cgp(&game_state);
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_ANALYSIS &&
                 (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
                  key == NCKEY_RIGHT || key == 'k' || key == 'K' ||
                  key == 'j' || key == 'J' || key == 'h' || key == 'H' ||
                  key == 'l' || key == 'L' || key == NCKEY_PGUP ||
                  key == NCKEY_PGDOWN || key == NCKEY_HOME ||
                  key == NCKEY_END)) {
        // Analysis panel nav. -1 = cursor on the [5>] label;
        // 0..N-1 = on a visible candidate row. Up/Down (and j/k)
        // moves the cursor row; Left/Right (and h/l) toggles the
        // column: LEFT/h → RANK (cursor pins to a row index),
        // RIGHT/l → MOVE (cursor pins to the move at the current
        // row and follows it as the sim reorders). PageUp/PageDown
        // jump by the visible window height; Home/End jump to the
        // first/last candidate.
        const bool key_up = key == NCKEY_UP || key == 'k' || key == 'K';
        const bool key_down = key == NCKEY_DOWN || key == 'j' || key == 'J';
        const bool key_left = key == NCKEY_LEFT || key == 'h' || key == 'H';
        const bool key_right = key == NCKEY_RIGHT || key == 'l' || key == 'L';
        const bool key_pgup = key == NCKEY_PGUP;
        const bool key_pgdn = key == NCKEY_PGDOWN;
        const bool key_home = key == NCKEY_HOME;
        const bool key_end = key == NCKEY_END;
        pthread_mutex_lock(&game_state.mutex);
        const int total = game_state.last_rendered_analysis_row_count;
        const int view_h = atomic_load(&game_state.analysis_visible_rows);
        if (key_home) {
          game_state.analysis_cursor = 0;
        } else if (key_end && total > 0) {
          game_state.analysis_cursor = total - 1;
        } else if ((key_pgup || key_pgdn) && view_h > 0) {
          const int step = view_h - 1 > 1 ? view_h - 1 : 1;
          int target =
              game_state.analysis_cursor < 0 ? 0 : game_state.analysis_cursor;
          target += key_pgdn ? step : -step;
          if (target < 0) {
            target = 0;
          }
          if (total > 0 && target >= total) {
            target = total - 1;
          }
          game_state.analysis_cursor = target;
        } else if (key_up || key_down) {
          const int last = total - 1;
          if (key_down) {
            if (game_state.analysis_cursor < last) {
              game_state.analysis_cursor++;
            }
          } else {
            if (game_state.analysis_cursor > -1) {
              game_state.analysis_cursor--;
            }
          }
          // Re-anchor when in MOVE column so the cursor follows the
          // new row's move from this point on.
          if (game_state.analysis_cursor_column == TUI_ANALYSIS_COLUMN_MOVE) {
            const int idx = game_state.analysis_cursor;
            if (idx >= 0 && idx < game_state.last_rendered_analysis_row_count) {
              snprintf(game_state.analysis_anchored_move,
                       sizeof(game_state.analysis_anchored_move), "%s",
                       game_state.last_rendered_analysis_rows[idx].move);
            } else {
              game_state.analysis_anchored_move[0] = '\0';
            }
          }
        } else if (key_left) {
          // Drop to RANK column. Discard anchor — the cursor sticks
          // to whatever row it's currently sitting at.
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        } else if (key_right) {
          // Switch to MOVE column. Capture the current row's move
          // text so the cursor will follow it as the sim reorders.
          const int idx = game_state.analysis_cursor;
          if (idx >= 0 && idx < game_state.last_rendered_analysis_row_count) {
            snprintf(game_state.analysis_anchored_move,
                     sizeof(game_state.analysis_anchored_move), "%s",
                     game_state.last_rendered_analysis_rows[idx].move);
            game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_MOVE;
          }
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_HISTORY &&
                 (key == NCKEY_HOME || key == NCKEY_END)) {
        // Home/End jump the cursor to the first / newest entry without
        // stepping through every turn — End is the quick "take me to
        // the live pending turn" gesture (and what a scripted driver
        // uses to reach the human's pending cell in play-vs-computer).
        pthread_mutex_lock(&game_state.mutex);
        const int prev_cursor = game_state.history_cursor;
        if (game_state.history_count > 0) {
          game_state.history_cursor =
              (key == NCKEY_HOME) ? 0 : game_state.history_count - 1;
        }
        if (game_state.history_cursor != prev_cursor) {
          game_state.analysis_cursor = 0;
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == TUI_FOCUS_HISTORY &&
                 (key == NCKEY_UP || key == NCKEY_DOWN || key == NCKEY_LEFT ||
                  key == NCKEY_RIGHT || key == 'k' || key == 'K' ||
                  key == 'j' || key == 'J' || key == 'h' || key == 'H' ||
                  key == 'l' || key == 'L')) {
        // History panel keyboard nav (cursor is on a label, not in
        // an entry's edit fields). The full step-through sequence is
        //   [4>] → 1> → 1.MOVE → 1.RACK → 2> → 2.MOVE → 2.RACK → 3> → ...
        // Plain Right from a "N>" label enters the editor on turn N's
        // MOVE field; plain Left from "N>" enters the editor on the
        // PREVIOUS turn's RACK (or stays at -1 if already there).
        // Shift+arrow keeps the original label-only behavior (jumps
        // 1> → 2> → 3>); h/j/k/l aliases follow the same rules.
        const bool forward = key == NCKEY_DOWN || key == NCKEY_RIGHT ||
                             key == 'j' || key == 'J' || key == 'l' ||
                             key == 'L';
        const bool is_horizontal = key == NCKEY_LEFT || key == NCKEY_RIGHT ||
                                   key == 'h' || key == 'H' || key == 'l' ||
                                   key == 'L';
        const bool shift = ncinput_shift_p(&input);
        pthread_mutex_lock(&game_state.mutex);
        const int last = game_state.history_count - 1;
        const int prev_cursor = game_state.history_cursor;
        const bool step_into = is_horizontal && !shift;
        if (step_into && forward && game_state.history_cursor >= 0 &&
            game_state.history_cursor <= last) {
          // N> → N.MOVE
          TuiHistoryEntry *e = &game_state.history[game_state.history_cursor];
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
          game_state.edit_rack_carryover[0] = '\0';
          game_state.edit_history_idx = game_state.history_cursor;
          game_state.edit_field = TUI_EDIT_FIELD_MOVE;
          tui_game_state_parse_edit_buf(&game_state);
        } else if (step_into && !forward && game_state.history_cursor > 0) {
          // N> → (N-1).RACK
          const int target = game_state.history_cursor - 1;
          TuiHistoryEntry *e = &game_state.history[target];
          snprintf(game_state.edit_move_buf, sizeof(game_state.edit_move_buf),
                   "%s", e->move_str);
          game_state.edit_move_len = (int)strlen(game_state.edit_move_buf);
          game_state.edit_move_cursor = game_state.edit_move_len;
          snprintf(game_state.edit_rack_buf, sizeof(game_state.edit_rack_buf),
                   "%s", e->rack_str);
          game_state.edit_rack_len = (int)strlen(game_state.edit_rack_buf);
          game_state.edit_rack_cursor = game_state.edit_rack_len;
          game_state.edit_rack_user_modified = e->rack_str[0] != '\0';
          game_state.edit_rack_carryover[0] = '\0';
          game_state.edit_history_idx = target;
          game_state.history_cursor = target;
          game_state.edit_field = TUI_EDIT_FIELD_RACK;
          tui_game_state_parse_edit_buf(&game_state);
        } else if (forward) {
          if (game_state.history_cursor < last) {
            game_state.history_cursor++;
          }
        } else {
          if (game_state.history_cursor > -1) {
            game_state.history_cursor--;
          }
        }
        // Whenever the History cursor lands on a new turn, snap the
        // Analysis cursor back to row 0 so the panel highlights the
        // play actually made for that turn (or the top play of the
        // live in-progress analysis when on the label / a pending
        // entry). Keeps the board preview consistent with the row
        // the user is looking at.
        if (game_state.history_cursor != prev_cursor) {
          game_state.analysis_cursor = 0;
          game_state.analysis_cursor_column = TUI_ANALYSIS_COLUMN_RANK;
          game_state.analysis_anchored_move[0] = '\0';
        }
        pthread_mutex_unlock(&game_state.mutex);
      } else if (game_state.focused_panel == 0) {
        // Slash-mode input loop: typing /, letters, Tab, Backspace,
        // Enter, Esc all map to slash buffer behavior rather than
        // global hotkeys. Falls through to the alphabetical hotkeys
        // (N/S/Q) only when slash mode is NOT active.
        if (game_state.slash_active) {
          if (key == NCKEY_ESC) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_active = false;
            game_state.slash_len = 0;
            game_state.slash_cursor = 0;
            game_state.slash_buf[0] = '\0';
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_LEFT) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor > 0) {
              game_state.slash_cursor--;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_RIGHT) {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor < game_state.slash_len) {
              game_state.slash_cursor++;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_HOME) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_cursor = 0;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_END) {
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_cursor = game_state.slash_len;
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_DEL) {
            // Forward-delete: remove char at cursor.
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor < game_state.slash_len) {
              memmove(game_state.slash_buf + game_state.slash_cursor,
                      game_state.slash_buf + game_state.slash_cursor + 1,
                      (size_t)(game_state.slash_len - game_state.slash_cursor));
              game_state.slash_len--;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_BACKSPACE || key == 0x7F || key == '\b') {
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_cursor > 0) {
              // Remove the char before the cursor.
              memmove(
                  game_state.slash_buf + game_state.slash_cursor - 1,
                  game_state.slash_buf + game_state.slash_cursor,
                  (size_t)(game_state.slash_len - game_state.slash_cursor + 1));
              game_state.slash_len--;
              game_state.slash_cursor--;
            } else if (game_state.slash_len == 0) {
              // Backspace at the very start of an empty buffer exits.
              game_state.slash_active = false;
            }
            pthread_mutex_unlock(&game_state.mutex);
          } else if (key == NCKEY_TAB || key == '\t') {
            // Tab completes against the unique prefix match.
            static const char *cmd_names[] = {"copy", "exit", "new", "quit",
                                              "settings"};
            static const int n_cmds =
                (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
            const char *match = NULL;
            int n_match = 0;
            for (int i = 0; i < n_cmds; i++) {
              if ((int)strlen(cmd_names[i]) >= game_state.slash_len &&
                  strncmp(cmd_names[i], game_state.slash_buf,
                          (size_t)game_state.slash_len) == 0) {
                match = cmd_names[i];
                n_match++;
              }
            }
            if (n_match == 1 && match != NULL) {
              pthread_mutex_lock(&game_state.mutex);
              snprintf(game_state.slash_buf, sizeof(game_state.slash_buf), "%s",
                       match);
              game_state.slash_len = (int)strlen(match);
              game_state.slash_cursor = game_state.slash_len;
              pthread_mutex_unlock(&game_state.mutex);
            }
          } else if (key == NCKEY_ENTER || key == '\r' || key == '\n') {
            // Execute the typed-or-completed command. Fall back to a
            // unique prefix match if user pressed Enter without
            // completing first.
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "%s", game_state.slash_buf);
            if (strcmp(cmd, "new") == 0 || strcmp(cmd, "n") == 0) {
              modal = TUI_MODAL_TIME_PICKER;
              time_focus = tui_time_picker_closest_index(chosen_time);
              time_picker_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "settings") == 0) {
              modal = TUI_MODAL_SETTINGS;
              settings_focus = 0;
              settings_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
              modal = TUI_MODAL_QUIT_CONFIRM;
              quit_confirm_focus = 0;
              quit_confirm_return = TUI_MODAL_NONE;
            } else if (strcmp(cmd, "copy") == 0) {
              pthread_mutex_lock(&game_state.mutex);
              tui_copy_position_cgp(&game_state);
              pthread_mutex_unlock(&game_state.mutex);
            } else {
              // Try a unique prefix match.
              static const char *cmd_names[] = {"copy", "exit", "new", "quit",
                                                "settings"};
              static const int n_cmds =
                  (int)(sizeof(cmd_names) / sizeof(cmd_names[0]));
              const char *match = NULL;
              int n_match = 0;
              for (int i = 0; i < n_cmds; i++) {
                if ((int)strlen(cmd_names[i]) >= game_state.slash_len &&
                    strncmp(cmd_names[i], game_state.slash_buf,
                            (size_t)game_state.slash_len) == 0) {
                  match = cmd_names[i];
                  n_match++;
                }
              }
              if (n_match == 1 && match != NULL) {
                if (strcmp(match, "new") == 0) {
                  modal = TUI_MODAL_TIME_PICKER;
                  time_focus = tui_time_picker_closest_index(chosen_time);
                  time_picker_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "settings") == 0) {
                  modal = TUI_MODAL_SETTINGS;
                  settings_focus = 0;
                  settings_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "quit") == 0 ||
                           strcmp(match, "exit") == 0) {
                  modal = TUI_MODAL_QUIT_CONFIRM;
                  quit_confirm_focus = 0;
                  quit_confirm_return = TUI_MODAL_NONE;
                } else if (strcmp(match, "copy") == 0) {
                  pthread_mutex_lock(&game_state.mutex);
                  tui_copy_position_cgp(&game_state);
                  pthread_mutex_unlock(&game_state.mutex);
                }
              }
            }
            pthread_mutex_lock(&game_state.mutex);
            game_state.slash_active = false;
            game_state.slash_len = 0;
            game_state.slash_cursor = 0;
            game_state.slash_buf[0] = '\0';
            pthread_mutex_unlock(&game_state.mutex);
          } else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
            // Insert (lowercased) at the cursor position rather than
            // always appending. Shifts the buffer tail right.
            const char ch = (key >= 'A' && key <= 'Z')
                                ? (char)(key + ('a' - 'A'))
                                : (char)key;
            pthread_mutex_lock(&game_state.mutex);
            if (game_state.slash_len < (int)sizeof(game_state.slash_buf) - 1) {
              memmove(
                  game_state.slash_buf + game_state.slash_cursor + 1,
                  game_state.slash_buf + game_state.slash_cursor,
                  (size_t)(game_state.slash_len - game_state.slash_cursor + 1));
              game_state.slash_buf[game_state.slash_cursor] = ch;
              game_state.slash_len++;
              game_state.slash_cursor++;
            }
            pthread_mutex_unlock(&game_state.mutex);
          }
        } else if (key == 'q' || key == 'Q') {
          modal = TUI_MODAL_QUIT_CONFIRM;
          quit_confirm_focus = 0;
          quit_confirm_return = TUI_MODAL_NONE;
        } else if (key == 's' || key == 'S') {
          modal = TUI_MODAL_SETTINGS;
          settings_focus = 0;
          settings_return = TUI_MODAL_NONE;
        } else if (key == 'n' || key == 'N') {
          modal = TUI_MODAL_TIME_PICKER;
          time_focus = tui_time_picker_closest_index(chosen_time);
          time_picker_return = TUI_MODAL_NONE;
        }
      }
    } while (running);

    // If this frame's input drain dirtied the frame, render it ASAP instead
    // of sleeping a full pacing interval first. The top-of-loop sleep would
    // otherwise add ~1 frame (~16ms) of keypress-to-pixels latency on top of
    // the (sub-millisecond) render — measured as the dominant input lag at
    // 1x, where the render itself is negligible. Collapsing the deadline to
    // "now" makes the next iteration skip the sleep and render immediately;
    // frame_dirty is always cleared by the render block, so steady-state
    // frames with no input still pace normally via the deadline advance.
    if (frame_dirty) {
      clock_gettime(CLOCK_MONOTONIC, &next_frame_deadline);
    }
  }

  tui_game_state_destroy(&game_state);
  if (lexicon_list != NULL) {
    tui_lexicon_list_destroy(lexicon_list);
  }
  // Symmetric with the startup enable — politely turn off focus
  // reporting so the terminal isn't left in an unexpected mode
  // after we exit.
  (void)!write(STDOUT_FILENO, "\x1b[?1004l", 8);
  notcurses_stop(nc);
  return 0;
}

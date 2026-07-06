#include "game_state.h"

#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg.h"
#include "../src/util/io_util.h"
#include "glyph_cache.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  DATA_PATH_CANDIDATES = 3,
};

static const char *find_data_paths(const char *lexicon) {
  static const char *candidates[DATA_PATH_CANDIDATES] = {
      "data",
      "../data",
      "./data",
  };
  for (int idx = 0; idx < DATA_PATH_CANDIDATES; idx++) {
    char path[512];
    const int written = snprintf(path, sizeof(path), "%s/lexica/%s.kwg",
                                 candidates[idx], lexicon);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
      continue;
    }
    if (access(path, R_OK) == 0) {
      return candidates[idx];
    }
  }
  return NULL;
}

static void copy_error(ErrorStack *err, char *buf, size_t buf_size) {
  if (buf_size == 0) {
    return;
  }
  buf[0] = '\0';
  char *msg = error_stack_get_string_and_reset(err);
  if (msg != NULL) {
    snprintf(buf, buf_size, "%s", msg);
    free(msg);
  }
}

bool tui_game_state_init(const char *lexicon, uint64_t seed, bool load_rit,
                         TuiGameState *out_state, char *error_message,
                         size_t error_message_size) {
  memset(out_state, 0, sizeof(*out_state));
  // Annotation edit state defaults to "not editing." All other
  // edit_* fields are already zeroed by the memset above; the
  // -1 sentinel just disambiguates "idle" from "editing entry 0."
  out_state->edit_history_idx = -1;
  // -2 = engine not yet positioned for any edit turn; forces the
  // first parse to seek.
  out_state->engine_positioned_for_turn = -2;
  // Snapshot of the settings we're locking in for this session. The
  // pending-change line above the status bar compares these to the
  // live config to decide what "(restart to apply)" should surface.
  snprintf(out_state->active_lexicon, sizeof(out_state->active_lexicon), "%s",
           lexicon);
  out_state->active_load_rit = load_rit;
  snprintf(out_state->pending_lexicon, sizeof(out_state->pending_lexicon), "%s",
           lexicon);
  out_state->pending_load_rit = load_rit;
  if (error_message != NULL && error_message_size > 0) {
    error_message[0] = '\0';
  }

  const char *data_paths = find_data_paths(lexicon);
  if (data_paths == NULL) {
    snprintf(error_message, error_message_size,
             "lexicon '%s' not found under data/lexica (run "
             "./download_data.sh from the repo root)",
             lexicon);
    return false;
  }
  out_state->data_paths = data_paths;

  ErrorStack *err = error_stack_create();

  char *ld_name = ld_get_default_name_from_lexicon_name(lexicon, err);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }
  out_state->ld = ld_create(data_paths, ld_name, err);
  free(ld_name);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }

  out_state->players_data = players_data_create(false);
  players_data_set(out_state->players_data, PLAYERS_DATA_TYPE_KWG, data_paths,
                   lexicon, lexicon, false, err);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }
  // Load KLV for static-eval leave valuation. Same name as the lexicon.
  players_data_set(out_state->players_data, PLAYERS_DATA_TYPE_KLV, data_paths,
                   lexicon, lexicon, false, err);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }
  // Load WMP for the faster wordmap-backed movegen. Best-effort: if
  // the bundled lexicon doesn't ship a .wmp the engine still runs on
  // the KWG alone, so we reset the error stack and continue.
  players_data_set(out_state->players_data, PLAYERS_DATA_TYPE_WMP, data_paths,
                   lexicon, lexicon, false, err);
  if (!error_stack_is_empty(err)) {
    error_stack_reset(err);
  }
  // RIT (rack info table) is large and only useful for inference /
  // advanced rack-aware analysis. Opt-in via load_rit; best-effort
  // even when requested so a missing file doesn't kill startup.
  if (load_rit) {
    players_data_set(out_state->players_data, PLAYERS_DATA_TYPE_RIT, data_paths,
                     lexicon, lexicon, false, err);
    if (!error_stack_is_empty(err)) {
      error_stack_reset(err);
    }
  }
  // Both players: pick the best move by static-eval equity.
  players_data_set_move_sort_type(out_state->players_data, 0, MOVE_SORT_EQUITY);
  players_data_set_move_sort_type(out_state->players_data, 1, MOVE_SORT_EQUITY);
  players_data_set_move_record_type(out_state->players_data, 0,
                                    MOVE_RECORD_BEST);
  players_data_set_move_record_type(out_state->players_data, 1,
                                    MOVE_RECORD_BEST);

  out_state->board_layout = board_layout_create_default(data_paths, err);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }

  // Win-percentage file is needed by the simulator. Failure to load is
  // non-fatal: the bot worker falls back to equity-best moves in that
  // case (no sim, no endgame). "winpct" is MAGPIE's bundled default.
  out_state->win_pcts = win_pct_create(data_paths, "winpct", err);
  if (!error_stack_is_empty(err)) {
    error_stack_reset(err);
    out_state->win_pcts = NULL;
  }

  // SimResults is allocated once and reused for every sim turn so the
  // analysis panel can read it. cutoff matches the bot worker's BAI
  // options (sim_results_set_cutoff inside the sim overrides this
  // when the sim starts).
  out_state->sim_results = sim_results_create(0.005);
  atomic_store(&out_state->sim_results_active, false);
  atomic_store(&out_state->sim_results_turn_idx, -1);

  out_state->endgame_results = endgame_results_create();
  atomic_store(&out_state->endgame_results_active, false);
  atomic_store(&out_state->endgame_results_turn_idx, -1);
  atomic_store(&out_state->endgame_initial_spread, 0);

  // The PEG poll is created lazily by the bot worker on the first
  // pre-endgame turn (many games never enter PEG range).
  out_state->peg_poll = NULL;
  atomic_store(&out_state->peg_results_active, false);
  atomic_store(&out_state->peg_results_turn_idx, -1);

  const GameArgs args = {
      .players_data = out_state->players_data,
      .board_layout = out_state->board_layout,
      .ld = out_state->ld,
      .game_variant = GAME_VARIANT_CLASSIC,
      .bingo_bonus = 50,
      .seed = seed,
  };
  out_state->game = game_create(&args);
  if (out_state->game == NULL) {
    snprintf(error_message, error_message_size, "game_create returned NULL");
    goto fail;
  }

  // Racks intentionally start empty so the startup menu sees the
  // widgets in an "idle" state (Bag full, Racks empty, Board empty).
  // The first game starts via tui_game_state_reset_game, which
  // draws fresh starting racks at that point.

  strncpy(out_state->lexicon, lexicon, sizeof(out_state->lexicon) - 1);
  out_state->lexicon[sizeof(out_state->lexicon) - 1] = '\0';

  pthread_mutex_init(&out_state->mutex, NULL);
  atomic_store(&out_state->bot_stop, false);
  out_state->bot_started = false;
  // Pixel worker primitives. The thread itself is started later by
  // tui_pixel_worker_start (called from main.c alongside the bot
  // worker start). Initializing the mutex/cond unconditionally is
  // safe — they're cheap when never signaled.
  pthread_mutex_init(&out_state->pixel_mutex, NULL);
  pthread_cond_init(&out_state->pixel_cond, NULL);
  atomic_store(&out_state->pixel_stop, false);
  out_state->pixel_started = false;
  out_state->history_count = 0;
  // -1 = cursor sits on the [4>] label. The cursor persists across
  // panel-focus changes (so the user keeps their place when they
  // click away and come back), so it's important to start at the
  // label rather than at memset's 0 (entry 0).
  out_state->history_cursor = -1;
  out_state->analysis_cursor = -1;
  out_state->analysis_cursor_column = 0; // TUI_ANALYSIS_COLUMN_RANK
  out_state->analysis_anchored_move[0] = '\0';
  out_state->last_rendered_analysis_row_count = 0;
  out_state->analysis_scroll_offset = 0;
  out_state->analysis_scrollbar_dragging = false;
  atomic_store(&out_state->analysis_scrollbar_top, 0);
  atomic_store(&out_state->analysis_scrollbar_bottom, 0);
  atomic_store(&out_state->analysis_scrollbar_col, 0);
  atomic_store(&out_state->analysis_scrollbar_total, 0);
  atomic_store(&out_state->analysis_scrollbar_view, 0);
  out_state->sim_plies = 4;
  out_state->sim_candidates = 100;
  atomic_store(&out_state->analysis_visible_rows, 0);
  out_state->time_per_side_seconds = 0;
  out_state->seconds_used[0] = 0.0;
  out_state->seconds_used[1] = 0.0;
  out_state->overtime_rule = UI_OVERTIME_MAX;
  out_state->overtime_cap_minutes = 5;
  out_state->time_penalty_rate = UI_TIME_PENALTY_10_PER_MIN;
  out_state->time_forfeit_player_idx = -1;
  out_state->time_penalties_applied = false;
  out_state->challenge_rule = UI_CHALLENGE_VOID;
  out_state->challenge_penalty = UI_CHALLENGE_PENALTY_5_PER_PLAY;
  out_state->analysis_started = false;
  atomic_store(&out_state->analysis_stop, false);
  atomic_store(&out_state->analysis_running, false);
  out_state->analysis_resume_turn_idx = -1;
  out_state->analysis_game = NULL;
  out_state->border_thickness = 2; // default; overridden by config
  out_state->blank_uppercase = true;
  out_state->premium_labels = TUI_PREMIUM_LABELS_UPPERCASE;
  out_state->board_scale = 1;
  out_state->antialias = true;
  out_state->score_subscripts = TUI_SCORE_SUBSCRIPTS_OFF;
  out_state->rack_sort = TUI_RACK_SORT_ALPHA;
  atomic_store(&out_state->render_version, (uint64_t)1);
  // Load the bundled TTF for 2x mode. Failure here just leaves
  // glyph_cache NULL — the renderer treats that as "scale=2 unavailable"
  // and silently falls back to 1x. The secondary cache holds digit
  // glyphs at the smaller pixel size used for score subscripts.
  char font_path[512];
  if (tui_glyph_cache_resolve_font_path(font_path, sizeof(font_path))) {
    out_state->glyph_cache = tui_glyph_cache_create(font_path);
    out_state->glyph_cache_sub = tui_glyph_cache_create(font_path);
    // Dedicated glyph caches owned exclusively by the pixel-worker
    // thread, so the worker's FT_Set_Pixel_Sizes / FT_Load_Glyph
    // calls never race with the UI thread rendering the rack or
    // label planes off the primary caches.
    out_state->pixel_glyph_cache = tui_glyph_cache_create(font_path);
    out_state->pixel_glyph_cache_sub = tui_glyph_cache_create(font_path);
  } else {
    out_state->glyph_cache = NULL;
    out_state->glyph_cache_sub = NULL;
    out_state->pixel_glyph_cache = NULL;
    out_state->pixel_glyph_cache_sub = NULL;
  }
  clock_gettime(CLOCK_MONOTONIC, &out_state->turn_started);

  // Preview Move for the annotation editor — caller allocates
  // once; the parser fills it in on each valid keystroke and
  // the board renderer ghosts it as the user types.
  out_state->edit_preview_move = move_create();
  out_state->edit_preview_move_valid = false;

  error_stack_destroy(err);
  return true;

fail:
  error_stack_destroy(err);
  tui_game_state_destroy(out_state);
  return false;
}

void tui_game_state_set_time_per_side(TuiGameState *state, int seconds) {
  if (state == NULL) {
    return;
  }
  state->time_per_side_seconds = seconds;
  state->seconds_used[0] = 0.0;
  state->seconds_used[1] = 0.0;
  clock_gettime(CLOCK_MONOTONIC, &state->turn_started);
}

bool tui_game_state_play_over(const TuiGameState *state) {
  if (state == NULL) {
    return false;
  }
  if (state->time_forfeit_player_idx >= 0) {
    return true;
  }
  return state->game != NULL && game_over(state->game);
}

static bool tok_is_rack_char(char c) {
  return (c >= 'A' && c <= 'Z') || c == '?';
}
static bool tok_is_word_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.';
}

// True if `s` (length `n`) is well-formed coord notation and the
// row/column are inside BOARD_DIM. Accepts either order:
//   <digits><letter>  e.g. "8H"   (row 8, col H)
//   <letter><digits>  e.g. "H8"   (col H, row 8)
// Reject "AA9" (two letters), "17B" (row 17 > BOARD_DIM), "0H"
// (rows are 1-indexed), and any 1- or 4+-char string.
static bool parse_coord_token(const char *s, int n) {
  if (n < 2 || n > 3) {
    return false;
  }
  const int max_col_letter = 'A' + BOARD_DIM - 1;
  // Walk forward separating letters from digits exactly once.
  // The token is valid only as <digits><letter> or <letter><digits>.
  int digit_value = 0;
  int digit_count = 0;
  char letter = '\0';
  int letter_count = 0;
  // Track ordering so we reject interleaved patterns like "8H8".
  bool seen_letter_after_digit = false;
  bool seen_digit_after_letter = false;
  for (int i = 0; i < n; i++) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      if (letter_count > 0) {
        seen_digit_after_letter = true;
      }
      digit_value = digit_value * 10 + (c - '0');
      digit_count++;
    } else if (c >= 'A' && c <= 'Z') {
      if (digit_count > 0) {
        seen_letter_after_digit = true;
      }
      letter = c;
      letter_count++;
    } else {
      return false;
    }
  }
  if (letter_count != 1 || digit_count < 1 || digit_count > 2) {
    return false;
  }
  if (seen_letter_after_digit && seen_digit_after_letter) {
    return false; // both transitions present → 3 segments → invalid
  }
  if (digit_value < 1 || digit_value > BOARD_DIM) {
    return false;
  }
  if (letter < 'A' || letter > max_col_letter) {
    return false;
  }
  return true;
}

// Returns true if `s` is a non-empty case-insensitive prefix of
// "exchange" — "e", "ex", "exc", ..., "exchange". Used to detect
// the user's exchange shortcut so the canonical "ex <tiles>" form
// can be forwarded to the engine validator.
static bool is_prefix_of_exchange(const char *s, int n) {
  static const char *full = "exchange";
  const int full_len = 8;
  if (n <= 0 || n > full_len) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    const char c = s[i];
    const char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (lower != full[i]) {
      return false;
    }
  }
  return true;
}

// "pass" prefix detector — same shape as exchange's but
// short-circuits when the buffer is exactly "pass".
static bool is_iequal(const char *s, int n, const char *target) {
  const int t = (int)strlen(target);
  if (n != t) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    const char c = s[i];
    const char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (lower != target[i]) {
      return false;
    }
  }
  return true;
}

// Walks the played-letters portion of a move's word token,
// extracting just the tiles that come off the rack: uppercase →
// the letter itself, lowercase → '?' (a played blank), '.' →
// skipped (playthrough). The result is sorted ascending (so '?'
// comes first, then A..Z) — this matches the order
// sort_rack_for_display would produce from a Rack count-table,
// and format_alphagram_for_sort can rebucket from there into
// whichever rack-sort the user picked without scrambling the
// input.
static int char_cmp(const void *a, const void *b) {
  return *(const unsigned char *)a - *(const unsigned char *)b;
}
// Derive the rack-tile string a validated Move would consume,
// EXCLUDING played-through tiles (which come from the board, not
// the rack). Blank-designated tiles map back to '?'. Sorted so '?'
// leads. This is more accurate than inferring from the raw word
// string, which can't tell a played-through letter from a freshly
// laid one — e.g. "1H QUIPPIER" played through PERSON's P needs
// only one P from the rack, not two.
static void infer_rack_from_move(const Move *m, const LetterDistribution *ld,
                                 char *out, size_t out_cap) {
  size_t oi = 0;
  const int n = move_get_tiles_length(m);
  for (int i = 0; i < n && oi + 1 < out_cap; i++) {
    const MachineLetter ml = move_get_tile(m, i);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (get_is_blanked(ml)) {
      out[oi++] = '?';
      continue;
    }
    const char *hl = ld->ld_ml_to_hl[ml];
    if (hl != NULL && hl[0] != '\0' && (unsigned char)hl[0] < 0x80) {
      out[oi++] = hl[0];
    }
  }
  out[oi] = '\0';
  if (oi > 1) {
    qsort(out, oi, 1, char_cmp);
  }
}

static void infer_rack_from_word(const char *word, int word_len, char *out,
                                 size_t out_cap) {
  size_t oi = 0;
  for (int i = 0; i < word_len && oi + 1 < out_cap; i++) {
    const char c = word[i];
    if (c == '.') {
      continue;
    }
    if (c >= 'a' && c <= 'z') {
      out[oi++] = '?';
    } else if (c >= 'A' && c <= 'Z') {
      out[oi++] = c;
    }
  }
  out[oi] = '\0';
  if (oi > 1) {
    qsort(out, oi, 1, char_cmp);
  }
}

// Multiset subtraction of `played` from `full_rack`, producing
// the leave (whatever tiles are left over) sorted ascending so
// '?' comes first, then A..Z. Returns false (leaves out[0] = '\0')
// when `played` contains a letter that isn't in `full_rack` —
// i.e., the user typed a move whose tiles aren't a subset of
// the typed rack. Callers gate leave display on a true return.
static bool compute_leave(const char *full_rack, const char *played, char *out,
                          size_t out_cap) {
  if (out == NULL || out_cap == 0) {
    return false;
  }
  out[0] = '\0';
  int counts[256] = {0};
  for (const char *p = full_rack; *p != '\0'; p++) {
    counts[(unsigned char)*p]++;
  }
  for (const char *p = played; *p != '\0'; p++) {
    counts[(unsigned char)*p]--;
    if (counts[(unsigned char)*p] < 0) {
      return false;
    }
  }
  size_t oi = 0;
  for (int c = 0; c < 256 && oi + 1 < out_cap; c++) {
    while (counts[c] > 0 && oi + 1 < out_cap) {
      out[oi++] = (char)c;
      counts[c]--;
    }
  }
  out[oi] = '\0';
  return true;
}

// True if every char in [s, s+n) is A-Z (no digits, no dots,
// no blanks). Used to detect a bare-word entry like "JUNKY"
// that should trigger the placement enumeration flow.
static bool tok_is_all_uppercase(const char *s, int n) {
  if (n <= 0) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    if (s[i] < 'A' || s[i] > 'Z') {
      return false;
    }
  }
  return true;
}

size_t tui_game_state_effective_editor_rack(const TuiGameState *state,
                                            char *out, size_t out_size,
                                            bool *out_from_buffer) {
  if (out == NULL || out_size == 0) {
    if (out_from_buffer != NULL) {
      *out_from_buffer = false;
    }
    return 0;
  }
  out[0] = '\0';
  bool from_buffer = false;
  // Selection order (must match the engine-rack assignment in
  // sync_player_rack_to_editor):
  //   1. carryover-leave + the move's inferred played tiles (freshly-seeded
  //      turn, user hasn't typed into RACK), alphagrammed;
  //   2. the rack buffer, but ONLY when user-typed AND currently valid;
  //   3. the move's inferred rack;
  //   4. the rack buffer as a last resort, again only when valid.
  if (!state->edit_rack_user_modified &&
      state->edit_rack_carryover[0] != '\0') {
    char combined[32];
    int oi = 0;
    for (const char *p = state->edit_rack_carryover;
         *p != '\0' && oi < (int)sizeof(combined) - 1; p++) {
      combined[oi++] = *p;
    }
    for (const char *p = state->edit_move_inferred_rack;
         *p != '\0' && oi < (int)sizeof(combined) - 1; p++) {
      combined[oi++] = *p;
    }
    combined[oi] = '\0';
    if (oi > 1) {
      qsort(combined, (size_t)oi, 1, char_cmp);
    }
    snprintf(out, out_size, "%s", combined);
  } else if (state->edit_rack_user_modified && state->edit_rack_len > 0 &&
             state->edit_rack_valid) {
    snprintf(out, out_size, "%.*s", state->edit_rack_len, state->edit_rack_buf);
    from_buffer = true;
  } else if (state->edit_move_inferred_rack[0] != '\0') {
    snprintf(out, out_size, "%s", state->edit_move_inferred_rack);
  } else if (state->edit_rack_len > 0 && state->edit_rack_valid) {
    snprintf(out, out_size, "%.*s", state->edit_rack_len, state->edit_rack_buf);
    from_buffer = true;
  }
  if (out_from_buffer != NULL) {
    *out_from_buffer = from_buffer;
  }
  return strlen(out);
}

// Set the on-turn player's rack to mirror the live editor, using the shared
// tui_game_state_effective_editor_rack selection so the pill and the history
// cell's rack row never disagree.
// Caller must hold state->mutex.
static void sync_player_rack_to_editor(TuiGameState *state) {
  // Play-vs-computer: the human's rack is the real bag-drawn rack, not
  // something inferred from the typed move. Mirroring the editor rack into
  // it here would erase the actual tiles (and empty it entirely once the
  // typed word uses them all, surfacing the "(no rack)" placeholder). The
  // board-entry preview never needs to touch the engine rack.
  if (state->app_mode == TUI_APP_MODE_PLAY_VS_COMPUTER) {
    return;
  }
  if (state->game == NULL || state->edit_history_idx < 0 ||
      state->edit_history_idx >= state->history_count) {
    return;
  }
  const int player_idx = state->history[state->edit_history_idx].player_idx;
  Player *player = game_get_player(state->game, player_idx);
  if (player == NULL) {
    return;
  }
  Rack *rack = player_get_rack(player);
  if (rack == NULL) {
    return;
  }
  // On a freshly-seeded turn (carryover-leave set, user hasn't typed into
  // RACK), mirror the carryover leave PLUS the move's played tiles into
  // edit_rack_buf so the RACK field shows the combined rack live (typing
  // "KAM" on a turn carrying "ERST" forward gives "AEKMRST"; the leave then
  // recomputes back to "ERST"). The effective-rack helper below computes the
  // identical combination for the engine rack.
  char carryover_combined[32];
  if (!state->edit_rack_user_modified &&
      state->edit_rack_carryover[0] != '\0') {
    int oi = 0;
    for (const char *p = state->edit_rack_carryover;
         *p != '\0' && oi < (int)sizeof(carryover_combined) - 1; p++) {
      carryover_combined[oi++] = *p;
    }
    for (const char *p = state->edit_move_inferred_rack;
         *p != '\0' && oi < (int)sizeof(carryover_combined) - 1; p++) {
      carryover_combined[oi++] = *p;
    }
    carryover_combined[oi] = '\0';
    if (oi > 1) {
      qsort(carryover_combined, (size_t)oi, 1, char_cmp);
    }
    snprintf(state->edit_rack_buf, sizeof(state->edit_rack_buf), "%s",
             carryover_combined);
    state->edit_rack_len = (int)strlen(state->edit_rack_buf);
    state->edit_rack_cursor = state->edit_rack_len;
  }
  // Single source of truth for which rack the pill shows — shared with the
  // renderer's history-cell rack row so the two can't drift.
  char source[32];
  const size_t source_len =
      tui_game_state_effective_editor_rack(state, source, sizeof(source), NULL);
  if (source_len > 0) {
    rack_set_to_string(state->ld, rack, source);
  } else {
    // Empty editor buffer + no inferred rack: only clobber the
    // engine rack on a *fresh* pending entry whose previous-turn
    // commit hasn't drawn tiles into it yet. For entries that
    // followed a play_move (where draw_to_full_rack just filled
    // the rack from the bag), wiping it would erase those drawn
    // tiles and leave the pill empty until the user retypes the
    // rack by hand. Heuristic: leave a non-empty live rack alone;
    // only reset when the rack was already empty.
    if (rack_get_total_letters(rack) == 0) {
      rack->dist_size = ld_get_size(state->ld);
      rack_reset(rack);
    }
  }
}

// Try to score `canonical` (a notation string in engine form, e.g.
// "8H JUNKY" or "ex AB") against the live board with the player's
// rack already set. Returns score (>=0) on success, -1 if the
// engine rejected the move. We pass allow_phonies=true so words
// not in the lexicon still produce a score — the renderer keeps
// the in-flight buffer green and lets the user commit; phony
// gating is a future refinement.
// Manually build a TILE_PLACEMENT Move from the parsed coord +
// word tokens. Used when the engine rejects the move (off-board,
// doesn't cross center, etc.) but the user still wants to see
// the in-flight tiles ghosted on the board.
//
// `coord_token` is a 2- or 3-char coord like "8H" or "H8";
// `word_token` is the played word with `word_len` bytes (may
// contain lowercase letters for played blanks). Returns true if
// the build succeeded.
static bool build_partial_preview_move(const TuiGameState *state,
                                       const char *coord_token, int coord_len,
                                       const char *word_token, int word_len,
                                       Move *out) {
  if (out == NULL || state->ld == NULL || coord_len < 2 || coord_len > 3 ||
      word_len <= 0) {
    return false;
  }
  // First char digit → digits-then-letter → HORIZONTAL.
  // First char letter → letter-then-digits → VERTICAL.
  int dir = -1;
  int row = -1;
  int col = -1;
  const char c0 = coord_token[0];
  if (c0 >= '0' && c0 <= '9') {
    dir = BOARD_HORIZONTAL_DIRECTION;
    int r = 0;
    int i = 0;
    while (i < coord_len && coord_token[i] >= '0' && coord_token[i] <= '9') {
      r = r * 10 + (coord_token[i] - '0');
      i++;
    }
    if (i >= coord_len || coord_token[i] < 'A' || coord_token[i] > 'Z') {
      return false;
    }
    col = coord_token[i] - 'A';
    row = r - 1;
  } else if (c0 >= 'A' && c0 <= 'Z') {
    dir = BOARD_VERTICAL_DIRECTION;
    col = c0 - 'A';
    int r = 0;
    int i = 1;
    while (i < coord_len && coord_token[i] >= '0' && coord_token[i] <= '9') {
      r = r * 10 + (coord_token[i] - '0');
      i++;
    }
    row = r - 1;
  } else {
    return false;
  }
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM) {
    return false;
  }
  // Convert the word to MachineLetters. `.` (playthrough) is
  // accepted so partial mid-play strings still render.
  char word_buf[80];
  if (word_len >= (int)sizeof(word_buf)) {
    word_len = (int)sizeof(word_buf) - 1;
  }
  memcpy(word_buf, word_token, (size_t)word_len);
  word_buf[word_len] = '\0';
  MachineLetter mls[80];
  const int n_mls =
      ld_str_to_mls(state->ld, word_buf, /*allow_played_through_marker=*/true,
                    mls, sizeof(mls) / sizeof(mls[0]));
  if (n_mls <= 0) {
    return false;
  }
  move_set_type(out, GAME_EVENT_TILE_PLACEMENT_MOVE);
  move_set_row_start(out, row);
  move_set_col_start(out, col);
  move_set_dir(out, dir);
  move_set_score(out, int_to_equity(0));
  move_set_tiles_played(out, n_mls);
  move_set_tiles_length(out, n_mls);
  for (int i = 0; i < n_mls; i++) {
    move_set_tile(out, mls[i], i);
  }
  return true;
}

// Score `canonical` against the live board. When `capture_move` is
// non-NULL and validation succeeds, ALSO deep-copy the resulting
// Move into `*capture_move` so the renderer can ghost it on the
// board as the user types. Returns score on success, -1 if the
// engine rejected the move (off-board, doesn't connect, etc.).
static int score_canonical_move(TuiGameState *state, const char *canonical,
                                int player_idx, Move *capture_move) {
  if (state->game == NULL || canonical[0] == '\0') {
    return -1;
  }
  ErrorStack *err = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(
      state->game, player_idx, canonical, /*allow_phonies=*/true,
      /*allow_playthrough=*/true, err);
  int score = -1;
  if (error_stack_is_empty(err) && vms != NULL &&
      validated_moves_get_number_of_moves(vms) > 0) {
    const Move *m = validated_moves_get_move(vms, 0);
    if (m != NULL) {
      score = equity_to_int(move_get_score(m));
      if (capture_move != NULL) {
        move_copy(capture_move, m);
      }
    }
  }
  if (vms != NULL) {
    validated_moves_destroy(vms);
  }
  error_stack_destroy(err);
  return score;
}

void tui_game_state_parse_edit_buf(TuiGameState *state) {
  if (state == NULL) {
    return;
  }
  // Position the engine board at the START of the turn being
  // edited, so score_canonical_move validates against the board
  // the player faced — NOT the post-game board (which already has
  // this turn's tiles, causing self-collisions and red text on a
  // perfectly legal committed move). Cached by turn index so we
  // only replay when switching turns, not on every keystroke.
  // Play-vs-computer edits the live current turn only, and the engine
  // is already positioned there with the real bag-drawn racks. Seeking
  // (which replays committed history and rebuilds racks from text, with
  // no bag draws) would desync the human's rack and the bag — so skip
  // it entirely in that mode. Annotation still seeks so validation sees
  // the board the edited player faced rather than the post-game board.
  if (state->app_mode != TUI_APP_MODE_PLAY_VS_COMPUTER && state->game != NULL &&
      state->edit_history_idx >= 0 &&
      state->engine_positioned_for_turn != state->edit_history_idx) {
    tui_game_state_seek_engine_to_turn(state, state->edit_history_idx);
  }
  // ── Move buffer ──────────────────────────────────────────────
  // Reset derived fields up front so an early-return leaves
  // consistent state.
  state->edit_move_kind = TUI_EDIT_MOVE_KIND_EMPTY;
  state->edit_move_score = -1;
  state->edit_move_canonical[0] = '\0';
  state->edit_move_inferred_rack[0] = '\0';
  state->edit_move_leave[0] = '\0';
  state->edit_move_valid = true;
  // Drop any stale preview from the prior parse. We only re-enable
  // it below when the engine accepts a TILE_PLACEMENT move — bare
  // words / partial / invalid / exchange / pass don't get ghosted.
  state->edit_preview_move_valid = false;

  const char *buf = state->edit_move_buf;
  const int n = state->edit_move_len;
  // Tokenize into up to two whitespace-delimited fields.
  int i = 0;
  while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
    i++;
  }
  const int t1_s = i;
  while (i < n && buf[i] != ' ' && buf[i] != '\t') {
    i++;
  }
  const int t1_len = i - t1_s;
  while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
    i++;
  }
  const int t2_s = i;
  while (i < n && buf[i] != ' ' && buf[i] != '\t') {
    i++;
  }
  const int t2_len = i - t2_s;
  const bool has_trailing_space =
      t1_len > 0 && t2_len == 0 && i > t1_s + t1_len;

  if (t1_len == 0) {
    // Empty buffer or pure whitespace.
    sync_player_rack_to_editor(state);
    goto parse_rack;
  }

  const char *t1 = buf + t1_s;
  const char *t2 = buf + t2_s;

  // ── Exchange shortcut: leading '-' ────────────────────────
  if (t1[0] == '-') {
    // "-" alone is PARTIAL; "-X..." is EXCHANGE if the tiles
    // are well-formed rack chars (A-Z or '?'), capped at 7.
    if (t1_len == 1 && t2_len == 0) {
      state->edit_move_kind = TUI_EDIT_MOVE_KIND_PARTIAL;
      goto parse_rack;
    }
    const char *tiles = t1 + 1;
    int tiles_len = t1_len - 1;
    // Allow "- ABC" form too: if user typed a space after -,
    // pick up the second token.
    if (t1_len == 1 && t2_len > 0) {
      tiles = t2;
      tiles_len = t2_len;
    }
    if (tiles_len > 7) {
      state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
      state->edit_move_valid = false;
      goto parse_rack;
    }
    for (int j = 0; j < tiles_len; j++) {
      if (!tok_is_rack_char(tiles[j])) {
        state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
        state->edit_move_valid = false;
        goto parse_rack;
      }
    }
    state->edit_move_kind = TUI_EDIT_MOVE_KIND_EXCHANGE;
    // Canonical form for the engine: "ex <tiles>".
    snprintf(state->edit_move_canonical, sizeof(state->edit_move_canonical),
             "ex %.*s", tiles_len, tiles);
    snprintf(state->edit_move_inferred_rack,
             sizeof(state->edit_move_inferred_rack), "%.*s", tiles_len, tiles);
    sync_player_rack_to_editor(state);
    state->edit_move_score =
        score_canonical_move(state, state->edit_move_canonical,
                             state->history[state->edit_history_idx].player_idx,
                             /*capture_move=*/NULL);
    if (state->edit_move_score < 0) {
      // Engine rejected (e.g., bag too small for an exchange).
      // Treat that as invalid so the row colors red.
      state->edit_move_valid = false;
    }
    goto parse_rack;
  }

  // ── Pass ──────────────────────────────────────────────────
  if (is_iequal(t1, t1_len, "pass") && t2_len == 0) {
    state->edit_move_kind = TUI_EDIT_MOVE_KIND_PASS;
    snprintf(state->edit_move_canonical, sizeof(state->edit_move_canonical),
             "pass");
    state->edit_move_score = 0;
    sync_player_rack_to_editor(state);
    goto parse_rack;
  }

  // ── Exchange word: any prefix of "exchange" ──────────────
  // Examples: "e Q", "ex AB", "exch ABC", "exchange QU".
  // The space between the keyword and the tiles is required
  // so a bare "e" doesn't get hijacked from word-only mode.
  if (is_prefix_of_exchange(t1, t1_len)) {
    if (t2_len == 0) {
      // "e", "ex" — the user might mean the word "EX" or might
      // be typing toward "ex ABC". Wait for the space.
      // Without a trailing space this is WORD_ONLY (the letters
      // might be a real word). With a trailing space it's a
      // partial exchange.
      if (has_trailing_space) {
        state->edit_move_kind = TUI_EDIT_MOVE_KIND_PARTIAL;
        goto parse_rack;
      }
      // Fall through to the word-only / coord branches below.
    } else {
      if (t2_len > 7) {
        state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
        state->edit_move_valid = false;
        goto parse_rack;
      }
      for (int j = 0; j < t2_len; j++) {
        if (!tok_is_rack_char(t2[j])) {
          state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
          state->edit_move_valid = false;
          goto parse_rack;
        }
      }
      state->edit_move_kind = TUI_EDIT_MOVE_KIND_EXCHANGE;
      snprintf(state->edit_move_canonical, sizeof(state->edit_move_canonical),
               "ex %.*s", t2_len, t2);
      snprintf(state->edit_move_inferred_rack,
               sizeof(state->edit_move_inferred_rack), "%.*s", t2_len, t2);
      sync_player_rack_to_editor(state);
      state->edit_move_score = score_canonical_move(
          state, state->edit_move_canonical,
          state->history[state->edit_history_idx].player_idx,
          /*capture_move=*/NULL);
      if (state->edit_move_score < 0) {
        state->edit_move_valid = false;
      }
      goto parse_rack;
    }
  }

  // ── Coord-led placement: "8H JUNKY" or "8H" (partial) ────
  if (parse_coord_token(t1, t1_len)) {
    if (t2_len == 0) {
      state->edit_move_kind = TUI_EDIT_MOVE_KIND_PARTIAL;
      // Even with no word yet, build a zero-tile preview Move so
      // the directional cursor can anchor at the typed coord.
      // The arrow's "next square" position is start_row/start_col
      // when tiles_length == 0 — exactly the cell the user is
      // about to type into. build_partial_preview_move requires
      // a word so we do the coord parse inline here.
      {
        Move *m = state->edit_preview_move;
        int pdir = -1;
        int prow = -1;
        int pcol = -1;
        const char c0 = t1[0];
        if (c0 >= '0' && c0 <= '9') {
          pdir = BOARD_HORIZONTAL_DIRECTION;
          int r = 0;
          int k = 0;
          while (k < t1_len && t1[k] >= '0' && t1[k] <= '9') {
            r = r * 10 + (t1[k] - '0');
            k++;
          }
          if (k < t1_len && t1[k] >= 'A' && t1[k] <= 'Z') {
            pcol = t1[k] - 'A';
            prow = r - 1;
          }
        } else if (c0 >= 'A' && c0 <= 'Z') {
          pdir = BOARD_VERTICAL_DIRECTION;
          pcol = c0 - 'A';
          int r = 0;
          int k = 1;
          while (k < t1_len && t1[k] >= '0' && t1[k] <= '9') {
            r = r * 10 + (t1[k] - '0');
            k++;
          }
          prow = r - 1;
        }
        if (pdir >= 0 && prow >= 0 && prow < BOARD_DIM && pcol >= 0 &&
            pcol < BOARD_DIM) {
          move_set_type(m, GAME_EVENT_TILE_PLACEMENT_MOVE);
          move_set_row_start(m, prow);
          move_set_col_start(m, pcol);
          move_set_dir(m, pdir);
          move_set_score(m, int_to_equity(0));
          move_set_tiles_played(m, 0);
          move_set_tiles_length(m, 0);
          state->edit_preview_move_valid = true;
        }
      }
      goto parse_rack;
    }
    // Word token must be all word-chars.
    for (int j = 0; j < t2_len; j++) {
      if (!tok_is_word_char(t2[j])) {
        state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
        state->edit_move_valid = false;
        goto parse_rack;
      }
    }
    state->edit_move_kind = TUI_EDIT_MOVE_KIND_PLACEMENT;
    snprintf(state->edit_move_canonical, sizeof(state->edit_move_canonical),
             "%.*s %.*s", t1_len, t1, t2_len, t2);
    infer_rack_from_word(t2, t2_len, state->edit_move_inferred_rack,
                         sizeof(state->edit_move_inferred_rack));
    sync_player_rack_to_editor(state);
    // Capture the validated Move into edit_preview_move so the
    // board can ghost it as the user types. Only mark
    // edit_preview_move_valid true when the engine actually
    // accepted the play (score >= 0) — partial / off-board /
    // doesn't-cross-center cases still get scored as -1 and
    // shouldn't ghost.
    state->edit_move_score =
        score_canonical_move(state, state->edit_move_canonical,
                             state->history[state->edit_history_idx].player_idx,
                             state->edit_preview_move);
    // Validity now reflects engine acceptance: an opening play
    // that doesn't cross the center, a placement that hangs off
    // the board, or a play that doesn't touch any existing tile
    // all come back as score < 0. Render red until the user
    // grows the play into something legal — but still ghost
    // the typed tiles on the board so the user can see what
    // they've shaped so far.
    if (state->edit_move_score < 0) {
      state->edit_move_valid = false;
      if (build_partial_preview_move(state, t1, t1_len, t2, t2_len,
                                     state->edit_preview_move)) {
        state->edit_preview_move_valid = true;
      }
    } else {
      state->edit_preview_move_valid = true;
      // Re-derive the inferred rack from the VALIDATED move so
      // played-through tiles (already on the board) aren't counted
      // as rack tiles. infer_rack_from_word above over-counts them;
      // the engine's Move marks them PLAYED_THROUGH_MARKER. Re-sync
      // so the rack panel and bag accounting see the correct tiles.
      infer_rack_from_move(state->edit_preview_move, state->ld,
                           state->edit_move_inferred_rack,
                           sizeof(state->edit_move_inferred_rack));
      sync_player_rack_to_editor(state);
    }
    goto parse_rack;
  }

  // ── Bare word (no coord): "JUNKY" ────────────────────────
  // No mixed digits, no dots, no blanks — case-sensitive
  // uppercase only. The placement-enumeration flow will fan
  // this out across the board on focus-away.
  if (t2_len == 0 && tok_is_all_uppercase(t1, t1_len)) {
    state->edit_move_kind = TUI_EDIT_MOVE_KIND_WORD_ONLY;
    infer_rack_from_word(t1, t1_len, state->edit_move_inferred_rack,
                         sizeof(state->edit_move_inferred_rack));
    sync_player_rack_to_editor(state);
    goto parse_rack;
  }

  // Anything else is malformed.
  state->edit_move_kind = TUI_EDIT_MOVE_KIND_INVALID;
  state->edit_move_valid = false;
  sync_player_rack_to_editor(state);

parse_rack:
  // ── Rack buffer: 1..7 chars of A-Z or ? ──────────────────
  {
    bool ok = state->edit_rack_len <= 7;
    if (state->edit_rack_len > 0) {
      for (int j = 0; j < state->edit_rack_len && ok; j++) {
        if (!tok_is_rack_char(state->edit_rack_buf[j])) {
          ok = false;
        }
      }
    }
    state->edit_rack_valid = ok;
  }
  // Live leave preview. The leave is the rack buffer minus the
  // played tiles. We only compute it when both sides are valid
  // and the rack is a superset of the played letters — otherwise
  // the row 1 leave zone keeps its "·" placeholder.
  //
  // When the user hasn't authored the rack (edit_rack_user_modified
  // is false), the rack is logically just the move's inferred
  // letters — so leave is empty by definition. Skip compute_leave
  // entirely; otherwise it'd diff the move against a stale
  // edit_rack_buf (still holding the prior move's auto-seed) and
  // surface phantom leave letters in row 1.
  if (!state->edit_rack_user_modified) {
    state->edit_move_leave[0] = '\0';
  } else if ((state->edit_move_kind == TUI_EDIT_MOVE_KIND_PLACEMENT ||
              state->edit_move_kind == TUI_EDIT_MOVE_KIND_EXCHANGE) &&
             state->edit_rack_valid && state->edit_rack_len > 0 &&
             state->edit_move_inferred_rack[0] != '\0') {
    char rack_buf[24];
    snprintf(rack_buf, sizeof(rack_buf), "%.*s", state->edit_rack_len,
             state->edit_rack_buf);
    char leave_buf[16];
    if (compute_leave(rack_buf, state->edit_move_inferred_rack, leave_buf,
                      sizeof(leave_buf))) {
      snprintf(state->edit_move_leave, sizeof(state->edit_move_leave), "%s",
               leave_buf);
    }
  }
  // Re-sync the rack panel: the rack buffer might override the
  // move buffer's inferred letters, so the final state of the
  // player's rack only settles after both have parsed.
  sync_player_rack_to_editor(state);
}

void tui_game_state_reset_game_for_annotation(TuiGameState *state) {
  if (state == NULL || state->game == NULL) {
    return;
  }
  // No seed, no rack draw — the engine lands at the "before
  // move 1" position with an empty board, racks empty, and the
  // full bag intact. The annotator fills each turn's rack in
  // by hand as the live game plays.
  game_reset(state->game);
  // Same per-entry / clock / atomic teardown the normal
  // reset_game does; duplicated here rather than refactored to
  // keep this change scoped.
  for (int idx = 0; idx < state->history_count; idx++) {
    TuiHistoryEntry *entry = &state->history[idx];
    if (entry->board_before != NULL) {
      board_destroy(entry->board_before);
      entry->board_before = NULL;
    }
    if (entry->rack_before != NULL) {
      rack_destroy(entry->rack_before);
      entry->rack_before = NULL;
    }
    if (entry->opp_rack_before != NULL) {
      rack_destroy(entry->opp_rack_before);
      entry->opp_rack_before = NULL;
    }
    if (entry->sim_results_saved != NULL) {
      sim_results_destroy(entry->sim_results_saved);
      entry->sim_results_saved = NULL;
    }
    if (entry->loaded_move != NULL) {
      free(entry->loaded_move);
      entry->loaded_move = NULL;
    }
  }
  state->history_count = 0;
  state->history_cursor = -1;
  state->seconds_used[0] = 0.0;
  state->seconds_used[1] = 0.0;
  state->time_forfeit_player_idx = -1;
  state->time_penalties_applied = false;
  clock_gettime(CLOCK_MONOTONIC, &state->turn_started);
  atomic_store(&state->sim_results_active, false);
  atomic_store(&state->sim_results_turn_idx, -1);
  atomic_store(&state->endgame_results_active, false);
  atomic_store(&state->endgame_results_turn_idx, -1);
  atomic_store(&state->endgame_initial_spread, 0);
  atomic_store(&state->peg_results_active, false);
  atomic_store(&state->peg_results_turn_idx, -1);
  state->peg_live_meta.valid = false;
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  if (state->endgame_ctx != NULL) {
    endgame_ctx_clear_transposition_table(state->endgame_ctx);
  }
  // Drop any in-progress board move-entry so a stale anchor doesn't
  // leak into the next game.
  state->board_entry_active = false;
  state->edit_history_idx = -1;
  atomic_fetch_add(&state->render_version, 1);
}

// Build an engine-format move notation from an entry's display
// move_str. Handles:
//   "8H POND"      → "8H POND"          (placement, pass-through)
//   "-ABCD"        → "ex ABCD"          (TUI compact exchange)
//   "ex ABCD"      → "ex ABCD"          (already canonical)
//   "(exch ABCD)"  → "ex ABCD"          (bot-finalized exchange)
//   "pass"         → "pass"
// `out` must be at least `out_cap` bytes; `out_cap` ≥ 8 for "ex"
// to fit even with empty tiles.
static void canonicalize_history_move(const char *display, char *out,
                                      size_t out_cap) {
  if (display == NULL || out_cap == 0) {
    return;
  }
  out[0] = '\0';
  if (display[0] == '\0') {
    return;
  }
  if (strcmp(display, "pass") == 0) {
    snprintf(out, out_cap, "pass");
    return;
  }
  if (display[0] == '-') {
    snprintf(out, out_cap, "ex %s", display + 1);
    return;
  }
  if (strncmp(display, "ex ", 3) == 0) {
    snprintf(out, out_cap, "%s", display);
    return;
  }
  if (strncmp(display, "(exch ", 6) == 0) {
    snprintf(out, out_cap, "ex %s", display + 6);
    char *paren = strchr(out, ')');
    if (paren != NULL) {
      *paren = '\0';
    }
    return;
  }
  // Default: treat as a placement notation and hand it to the
  // engine as-is.
  snprintf(out, out_cap, "%s", display);
}

// Shared replay core. Resets the engine and applies committed
// turns in [0, up_to) (up_to < 0 means "all of history"). When
// record_errors is true, clears every entry's error_str first and
// writes a message onto the first turn that fails to validate,
// then stops; total_after is refreshed on each applied turn. When
// false (the editor "seek" path), no error_str is touched and the
// replay simply stops at the first failure, leaving the engine at
// the position after the last successfully-applied turn — i.e. the
// pre-move board for turn `up_to`.
static void replay_history_prefix(TuiGameState *state, int up_to,
                                  bool record_errors) {
  const int limit = (up_to < 0 || up_to > state->history_count)
                        ? state->history_count
                        : up_to;
  if (record_errors) {
    for (int idx = 0; idx < state->history_count; idx++) {
      state->history[idx].error_str[0] = '\0';
    }
  }
  game_reset(state->game);
  // Cumulative tiles "drawn from the bag" per letter across all
  // turns. Each turn introduces (this turn's rack − the player's
  // previous leave) new tiles; summed, no letter may exceed the
  // distribution count. Catches impossible racks like "ZZZ" when
  // the bag holds a single Z — something validated_moves_create
  // can't see, since we set the rack artificially.
  const int ld_size = ld_get_size(state->ld);
  int drawn[MACHINE_LETTER_MAX_VALUE];
  memset(drawn, 0, sizeof(drawn));
  for (int idx = 0; idx < limit; idx++) {
    TuiHistoryEntry *e = &state->history[idx];
    if (e->pending) {
      continue;
    }
    // Seed the on-turn player's rack from the entry's rack_str.
    // Empty rack_str leaves the rack empty; the engine will then
    // reject moves whose tiles aren't held, surfacing as
    // "rack missing X" — exactly the validation the annotator
    // wants.
    Player *player = game_get_player(state->game, e->player_idx);
    if (player == NULL) {
      if (record_errors) {
        snprintf(e->error_str, sizeof(e->error_str),
                 "internal: player_idx %d out of range", e->player_idx);
      }
      break;
    }
    Rack *rack = player_get_rack(player);
    if (rack == NULL) {
      if (record_errors) {
        snprintf(e->error_str, sizeof(e->error_str),
                 "internal: player has no rack");
      }
      break;
    }
    // Snapshot the player's leave (engine rack carried over from
    // their previous turn) BEFORE overwriting with this turn's
    // rack. The newly-drawn tiles this turn = this rack − that
    // leave, per letter.
    int prev_leave[MACHINE_LETTER_MAX_VALUE];
    memset(prev_leave, 0, sizeof(prev_leave));
    for (int ml = 0; ml < ld_size && ml < MACHINE_LETTER_MAX_VALUE; ml++) {
      prev_leave[ml] = (int)rack_get_letter(rack, (MachineLetter)ml);
    }
    if (e->rack_str[0] != '\0') {
      rack_set_to_string(state->ld, rack, e->rack_str);
    } else {
      rack->dist_size = ld_get_size(state->ld);
      rack_reset(rack);
    }
    // Bag-availability check: add (this rack − prev leave) to the
    // cumulative draw counts and reject if any letter overruns
    // its distribution. Runs before move validation so an
    // impossible rack is reported even if the move notation is
    // otherwise well-formed.
    bool bag_overrun = false;
    char overrun_msg[192];
    int omi = 0;
    for (int ml = 0; ml < ld_size && ml < MACHINE_LETTER_MAX_VALUE; ml++) {
      const int have = (int)rack_get_letter(rack, (MachineLetter)ml);
      int newly = have - prev_leave[ml];
      if (newly < 0) {
        newly = 0;
      }
      drawn[ml] += newly;
      if (drawn[ml] > ld_get_dist(state->ld, (MachineLetter)ml)) {
        bag_overrun = true;
        // Accumulate EVERY overrun letter into one message rather
        // than stopping at the first — a single play can exhaust
        // more than one tile (e.g. needs a 2nd Q and a 3rd P).
        if (record_errors && omi < (int)sizeof(overrun_msg) - 1) {
          const char *hl = state->ld->ld_ml_to_hl[ml];
          const char *label =
              (ml == 0 || hl == NULL || hl[0] == '\0') ? "?" : hl;
          const int dist = ld_get_dist(state->ld, (MachineLetter)ml);
          const int w =
              snprintf(overrun_msg + omi, sizeof(overrun_msg) - (size_t)omi,
                       omi == 0 ? "not enough %s's (need %d, have %d)"
                                : "; %s's (need %d, have %d)",
                       label, drawn[ml], dist);
          if (w > 0) {
            omi += w;
            if (omi >= (int)sizeof(overrun_msg)) {
              omi = (int)sizeof(overrun_msg) - 1;
            }
          }
        }
      }
    }
    if (bag_overrun) {
      if (record_errors) {
        snprintf(e->error_str, sizeof(e->error_str), "%s", overrun_msg);
      }
      break;
    }
    // Normalize display notation → engine canonical, validate.
    char canonical[64];
    canonicalize_history_move(e->move_str, canonical, sizeof(canonical));
    if (canonical[0] == '\0') {
      if (record_errors) {
        snprintf(e->error_str, sizeof(e->error_str), "no move specified");
      }
      break;
    }
    ErrorStack *err = error_stack_create();
    ValidatedMoves *vms = validated_moves_create(
        state->game, e->player_idx, canonical,
        /*allow_phonies=*/true, /*allow_playthrough=*/true, err);
    bool ok = false;
    const Move *applied_move = NULL;
    if (error_stack_is_empty(err) && vms != NULL &&
        validated_moves_get_number_of_moves(vms) > 0) {
      const Move *mv = validated_moves_get_move(vms, 0);
      if (mv != NULL && equity_to_int(move_get_score(mv)) >= 0) {
        applied_move = mv;
        ok = true;
      }
    }
    if (!ok) {
      if (record_errors) {
        char *err_msg = error_stack_get_string_and_reset(err);
        if (err_msg != NULL && err_msg[0] != '\0') {
          // Trim trailing newline if the engine appended one.
          const size_t L = strlen(err_msg);
          if (L > 0 && err_msg[L - 1] == '\n') {
            err_msg[L - 1] = '\0';
          }
          snprintf(e->error_str, sizeof(e->error_str), "%s", err_msg);
        } else {
          snprintf(e->error_str, sizeof(e->error_str), "invalid move: %s",
                   canonical);
        }
        if (err_msg != NULL) {
          free(err_msg);
        }
      }
      if (vms != NULL) {
        validated_moves_destroy(vms);
      }
      error_stack_destroy(err);
      // Leave the engine state at the position BEFORE this turn —
      // that's the "we got this far successfully" snapshot the
      // annotator wants to see.
      break;
    }
    // Move validated successfully. Apply on the engine and tag
    // tile owners for placement moves so the board renderer paints
    // each placed tile in its player's color.
    play_move_without_drawing_tiles(applied_move, state->game);
    if (move_get_type(applied_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const int dir = move_get_dir(applied_move);
      int r = move_get_row_start(applied_move);
      int c = move_get_col_start(applied_move);
      const int n_tiles = move_get_tiles_length(applied_move);
      for (int t = 0; t < n_tiles; t++) {
        if (move_get_tile(applied_move, t) != PLAYED_THROUGH_MARKER) {
          board_set_square_owner(game_get_board(state->game), r, c,
                                 e->player_idx);
        }
        if (board_is_dir_vertical(dir)) {
          r++;
        } else {
          c++;
        }
      }
    }
    // Refresh total_after with the engine's running score.
    e->total_after = equity_to_int(
        player_get_score(game_get_player(state->game, e->player_idx)));
    if (vms != NULL) {
      validated_moves_destroy(vms);
    }
    error_stack_destroy(err);
  }
}

void tui_game_state_revalidate_history(TuiGameState *state) {
  if (state == NULL || state->game == NULL) {
    return;
  }
  replay_history_prefix(state, -1, /*record_errors=*/true);
  // Full replay leaves the engine at the post-last-turn position;
  // mark the editor-seek cache stale so the next parse re-seeks to
  // whatever turn is being edited.
  state->engine_positioned_for_turn = -2;
  atomic_fetch_add(&state->render_version, 1);
}

void tui_game_state_seek_engine_to_turn(TuiGameState *state, int idx) {
  if (state == NULL || state->game == NULL) {
    return;
  }
  // Replay [0, idx) so the engine board reflects the position the
  // player faced at the START of turn idx. Used by the editor so
  // re-validating a committed turn's move doesn't collide with the
  // turn's own tiles (which are otherwise still on the board from
  // the full replay).
  replay_history_prefix(state, idx, /*record_errors=*/false);
  state->engine_positioned_for_turn = idx;
}

void tui_game_state_reset_game(TuiGameState *state, uint64_t seed) {
  if (state == NULL || state->game == NULL) {
    return;
  }
  game_reset(state->game);
  game_seed(state->game, seed);
  draw_starting_racks(state->game);
  // Free per-entry owned state before zeroing the count.
  for (int idx = 0; idx < state->history_count; idx++) {
    TuiHistoryEntry *entry = &state->history[idx];
    if (entry->board_before != NULL) {
      board_destroy(entry->board_before);
      entry->board_before = NULL;
    }
    if (entry->rack_before != NULL) {
      rack_destroy(entry->rack_before);
      entry->rack_before = NULL;
    }
    if (entry->opp_rack_before != NULL) {
      rack_destroy(entry->opp_rack_before);
      entry->opp_rack_before = NULL;
    }
    if (entry->sim_results_saved != NULL) {
      sim_results_destroy(entry->sim_results_saved);
      entry->sim_results_saved = NULL;
    }
    if (entry->loaded_move != NULL) {
      free(entry->loaded_move);
      entry->loaded_move = NULL;
    }
  }
  state->history_count = 0;
  state->history_cursor = -1;
  state->seconds_used[0] = 0.0;
  state->seconds_used[1] = 0.0;
  state->time_forfeit_player_idx = -1;
  state->time_penalties_applied = false;
  clock_gettime(CLOCK_MONOTONIC, &state->turn_started);
  atomic_store(&state->sim_results_active, false);
  atomic_store(&state->sim_results_turn_idx, -1);
  atomic_store(&state->endgame_results_active, false);
  atomic_store(&state->endgame_results_turn_idx, -1);
  atomic_store(&state->endgame_initial_spread, 0);
  atomic_store(&state->peg_results_active, false);
  atomic_store(&state->peg_results_turn_idx, -1);
  state->peg_live_meta.valid = false;
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  if (state->endgame_ctx != NULL) {
    endgame_ctx_clear_transposition_table(state->endgame_ctx);
  }
  // Drop any in-progress board move-entry so a stale anchor doesn't
  // leak into the next game.
  state->board_entry_active = false;
  state->edit_history_idx = -1;
  atomic_fetch_add(&state->render_version, 1);
}

void tui_game_state_destroy(TuiGameState *state) {
  if (state == NULL) {
    return;
  }
  if (state->edit_preview_move != NULL) {
    move_destroy(state->edit_preview_move);
    state->edit_preview_move = NULL;
    state->edit_preview_move_valid = false;
  }
  if (state->bot_started) {
    atomic_store(&state->bot_stop, true);
    pthread_join(state->bot_thread, NULL);
    state->bot_started = false;
  }
  // Tear down the analysis-resume worker before its entry-owned
  // sim results / the endgame ctx are freed below.
  if (state->analysis_started) {
    atomic_store(&state->analysis_stop, true);
    pthread_join(state->analysis_thread, NULL);
    state->analysis_started = false;
  }
  // Tear down the pixel worker: set the stop flag, wake the thread
  // (it might be blocked in cond_wait), join.
  if (state->pixel_started) {
    pthread_mutex_lock(&state->pixel_mutex);
    atomic_store(&state->pixel_stop, true);
    pthread_cond_signal(&state->pixel_cond);
    pthread_mutex_unlock(&state->pixel_mutex);
    pthread_join(state->pixel_thread, NULL);
    state->pixel_started = false;
  }
  // Free any owned request/result residue before the glyph caches
  // go away.
  if (state->pixel_request.board != NULL) {
    board_destroy(state->pixel_request.board);
    state->pixel_request.board = NULL;
  }
  if (state->pixel_result.buf != NULL) {
    free(state->pixel_result.buf);
    state->pixel_result.buf = NULL;
  }
  if (state->pixel_glyph_cache != NULL) {
    tui_glyph_cache_destroy(state->pixel_glyph_cache);
    state->pixel_glyph_cache = NULL;
  }
  if (state->pixel_glyph_cache_sub != NULL) {
    tui_glyph_cache_destroy(state->pixel_glyph_cache_sub);
    state->pixel_glyph_cache_sub = NULL;
  }
  // Free per-entry owned state stashed during gameplay.
  for (int idx = 0; idx < state->history_count; idx++) {
    TuiHistoryEntry *entry = &state->history[idx];
    if (entry->board_before != NULL) {
      board_destroy(entry->board_before);
      entry->board_before = NULL;
    }
    if (entry->rack_before != NULL) {
      rack_destroy(entry->rack_before);
      entry->rack_before = NULL;
    }
    if (entry->opp_rack_before != NULL) {
      rack_destroy(entry->opp_rack_before);
      entry->opp_rack_before = NULL;
    }
    if (entry->sim_results_saved != NULL) {
      sim_results_destroy(entry->sim_results_saved);
      entry->sim_results_saved = NULL;
    }
    if (entry->loaded_move != NULL) {
      free(entry->loaded_move);
      entry->loaded_move = NULL;
    }
  }
  state->history_count = 0;
  if (state->game != NULL) {
    game_destroy(state->game);
  }
  if (state->board_layout != NULL) {
    board_layout_destroy(state->board_layout);
  }
  if (state->players_data != NULL) {
    players_data_destroy(state->players_data);
  }
  if (state->ld != NULL) {
    ld_destroy(state->ld);
  }
  if (state->glyph_cache != NULL) {
    tui_glyph_cache_destroy(state->glyph_cache);
  }
  if (state->glyph_cache_sub != NULL) {
    tui_glyph_cache_destroy(state->glyph_cache_sub);
  }
  if (state->win_pcts != NULL) {
    win_pct_destroy(state->win_pcts);
  }
  if (state->sim_results != NULL) {
    sim_results_destroy(state->sim_results);
  }
  if (state->endgame_results != NULL) {
    endgame_results_destroy(state->endgame_results);
  }
  if (state->endgame_ctx != NULL) {
    endgame_ctx_destroy(state->endgame_ctx);
  }
  if (state->peg_poll != NULL) {
    peg_poll_destroy(state->peg_poll);
  }
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  memset(state, 0, sizeof(*state));
}

bool tui_position_in_peg_range(const struct Game *game) {
  if (game == NULL) {
    return false;
  }
  // Mirror peg_solve's own range check: the raw bag holds the real
  // remaining tiles plus the opponent's unknown holdings, so subtract
  // (RACK_SIZE - opp rack tiles) to get the effective bag size.
  const int raw_bag = bag_get_letters(game_get_bag(game));
  const int mover_idx = game_get_player_on_turn_index(game);
  const Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  const int opp_unknown = RACK_SIZE - (int)rack_get_total_letters(opp_rack);
  const int effective_bag = raw_bag - opp_unknown;
  return effective_bag >= PEG_MIN_BAG && effective_bag <= PEG_MAX_BAG;
}

void tui_endgame_snapshot_clear(TuiEndgameSnapshot *snap) {
  if (snap == NULL) {
    return;
  }
  if (snap->board != NULL) {
    board_destroy(snap->board);
    snap->board = NULL;
  }
  if (snap->solve_rack != NULL) {
    rack_destroy(snap->solve_rack);
    snap->solve_rack = NULL;
  }
  if (snap->moves != NULL) {
    for (int i = 0; i < snap->num_entries; i++) {
      if (snap->moves[i] != NULL) {
        move_destroy(snap->moves[i]);
      }
    }
    free(snap->moves);
    snap->moves = NULL;
  }
  if (snap->values != NULL) {
    free(snap->values);
    snap->values = NULL;
  }
  snap->num_entries = 0;
  snap->initial_spread = 0;
  snap->depth = 0;
  snap->solving_player = 0;
  snap->valid = false;
}

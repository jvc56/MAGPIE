#include "game_state.h"

#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
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

  draw_starting_racks(out_state->game);

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
  atomic_store(&out_state->analysis_visible_rows, 0);
  out_state->time_per_side_seconds = 0;
  out_state->seconds_used[0] = 0.0;
  out_state->seconds_used[1] = 0.0;
  out_state->border_thickness = 2; // default; overridden by config
  out_state->blank_uppercase = true;
  out_state->premium_labels = TUI_PREMIUM_LABELS_UPPERCASE;
  out_state->board_scale = 1;
  out_state->antialias = true;
  out_state->score_subscripts = TUI_SCORE_SUBSCRIPTS_OFF;
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
  }
  state->history_count = 0;
  state->history_cursor = -1;
  state->seconds_used[0] = 0.0;
  state->seconds_used[1] = 0.0;
  clock_gettime(CLOCK_MONOTONIC, &state->turn_started);
  atomic_store(&state->sim_results_active, false);
  atomic_store(&state->sim_results_turn_idx, -1);
  atomic_store(&state->endgame_results_active, false);
  atomic_store(&state->endgame_results_turn_idx, -1);
  atomic_store(&state->endgame_initial_spread, 0);
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  if (state->endgame_ctx != NULL) {
    endgame_ctx_clear_transposition_table(state->endgame_ctx);
  }
  atomic_fetch_add(&state->render_version, 1);
}

void tui_game_state_destroy(TuiGameState *state) {
  if (state == NULL) {
    return;
  }
  if (state->bot_started) {
    atomic_store(&state->bot_stop, true);
    pthread_join(state->bot_thread, NULL);
    state->bot_started = false;
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
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  memset(state, 0, sizeof(*state));
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

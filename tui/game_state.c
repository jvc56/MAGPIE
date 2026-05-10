#include "game_state.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"

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

bool tui_game_state_init(const char *lexicon, uint64_t seed,
                         TuiGameState *out_state, char *error_message,
                         size_t error_message_size) {
  memset(out_state, 0, sizeof(*out_state));
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
  // Both players: pick the best move by static-eval equity.
  players_data_set_move_sort_type(out_state->players_data, 0,
                                  MOVE_SORT_EQUITY);
  players_data_set_move_sort_type(out_state->players_data, 1,
                                  MOVE_SORT_EQUITY);
  players_data_set_move_record_type(out_state->players_data, 0,
                                    MOVE_RECORD_BEST);
  players_data_set_move_record_type(out_state->players_data, 1,
                                    MOVE_RECORD_BEST);

  out_state->board_layout = board_layout_create_default(data_paths, err);
  if (!error_stack_is_empty(err)) {
    copy_error(err, error_message, error_message_size);
    goto fail;
  }

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
  out_state->history_count = 0;
  out_state->time_per_side_seconds = 0;
  out_state->seconds_used[0] = 0.0;
  out_state->seconds_used[1] = 0.0;
  out_state->border_thickness = 2;  // default; overridden by config
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

void tui_game_state_destroy(TuiGameState *state) {
  if (state == NULL) {
    return;
  }
  if (state->bot_started) {
    atomic_store(&state->bot_stop, true);
    pthread_join(state->bot_thread, NULL);
    state->bot_started = false;
  }
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
  memset(state, 0, sizeof(*state));
}

#include "value_net_player.h"

#include "../compat/cpthread.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/value_net_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/value_net.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include "value_net_coreml.h"
#include "value_net_features.h"
#include "value_net_metal.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  VALUE_NET_PLAYER_DEFAULT_CANDIDATES = 50,
  // Metal compiles its graph once per batch size, so rows are padded to
  // the next of these.
  VALUE_NET_PLAYER_MIN_BATCH = 8,
};

struct ValueNetPlayer {
  ValueNet *net;
  ValueNetMetal *metal;
  ValueNetCoreML *coreml;
  // Whether a call holds the GPU (with both engines; see
  // value_net_player_evaluate_rows).
  atomic_bool metal_busy;
  int max_candidates;
  int batch_capacity;
  MoveList *list;
  Game *scratch;
  float *board_rows;
  float *scalar_rows;
  float *values;
  // Staging for value_net_player_evaluate_rows, under staging_mutex.
  cpthread_mutex_t staging_mutex;
  int staging_capacity;
  float *staging_board;
  float *staging_scalars;
  float *staging_values;
};

ValueNetPlayer *value_net_player_create(const char *model_dir,
                                        value_net_backend_t backend,
                                        int max_candidates,
                                        ErrorStack *error_stack) {
  ValueNet *net = value_net_create(model_dir, error_stack);
  if (net == NULL) {
    return NULL;
  }
  ValueNetMetal *metal = NULL;
  ValueNetCoreML *coreml = NULL;
  if (backend == VALUE_NET_BACKEND_COREML ||
      backend == VALUE_NET_BACKEND_COREML_AND_METAL) {
    char *path = get_formatted_string("%s/ane.mlpackage", model_dir);
    coreml = value_net_coreml_create(path, 0, error_stack);
    free(path);
    if (coreml == NULL) {
      value_net_destroy(net);
      return NULL;
    }
  }
  if (backend == VALUE_NET_BACKEND_METAL_FP32 ||
      backend == VALUE_NET_BACKEND_METAL_FP16 ||
      backend == VALUE_NET_BACKEND_COREML_AND_METAL) {
    metal = value_net_metal_create(net, backend != VALUE_NET_BACKEND_METAL_FP32,
                                   error_stack);
    if (metal == NULL) {
      value_net_coreml_destroy(coreml);
      value_net_destroy(net);
      return NULL;
    }
  }
  ValueNetPlayer *player = calloc_or_die(1, sizeof(ValueNetPlayer));
  player->net = net;
  player->metal = metal;
  player->coreml = coreml;
  player->max_candidates =
      max_candidates > 0 ? max_candidates : VALUE_NET_PLAYER_DEFAULT_CANDIDATES;
  player->batch_capacity = VALUE_NET_PLAYER_MIN_BATCH;
  while (player->batch_capacity < player->max_candidates) {
    player->batch_capacity *= 2;
  }
  player->list = move_list_create(player->max_candidates);
  player->board_rows = calloc_or_die(
      (size_t)player->batch_capacity * VALUE_NET_BOARD_FLOATS, sizeof(float));
  player->scalar_rows = calloc_or_die(
      (size_t)player->batch_capacity * VALUE_NET_SCALARS, sizeof(float));
  player->values = calloc_or_die((size_t)player->batch_capacity, sizeof(float));
  cpthread_mutex_init(&player->staging_mutex);
  return player;
}

void value_net_player_destroy(ValueNetPlayer *player) {
  if (player == NULL) {
    return;
  }
  value_net_metal_destroy(player->metal);
  value_net_coreml_destroy(player->coreml);
  value_net_destroy(player->net);
  move_list_destroy(player->list);
  game_destroy(player->scratch);
  free(player->board_rows);
  free(player->scalar_rows);
  free(player->values);
  free(player->staging_board);
  free(player->staging_scalars);
  free(player->staging_values);
  free(player);
}

// The next power of two from VALUE_NET_PLAYER_MIN_BATCH holding rows rows,
// so Metal compiles few batch sizes.
static int value_net_padded_rows(int rows) {
  if (rows > VALUE_NET_MAX_GPU_ROWS) {
    // Metal runs these in chunks of VALUE_NET_MAX_GPU_ROWS.
    return ((rows + VALUE_NET_MAX_GPU_ROWS - 1) / VALUE_NET_MAX_GPU_ROWS) *
           VALUE_NET_MAX_GPU_ROWS;
  }
  int batch = VALUE_NET_PLAYER_MIN_BATCH;
  while (batch < rows) {
    batch *= 2;
  }
  return batch;
}

// Evaluates rows on Metal through the staging buffers (padded to a batch
// size Metal has compiled).
static void value_net_player_evaluate_metal(ValueNetPlayer *player, int rows,
                                            const float *board,
                                            const float *scalars,
                                            float *values) {
  const int batch = value_net_padded_rows(rows);
  cpthread_mutex_lock(&player->staging_mutex);
  if (batch > player->staging_capacity) {
    player->staging_capacity = batch;
    player->staging_board =
        realloc_or_die(player->staging_board,
                       sizeof(float) * (size_t)batch * VALUE_NET_BOARD_FLOATS);
    player->staging_scalars =
        realloc_or_die(player->staging_scalars,
                       sizeof(float) * (size_t)batch * VALUE_NET_SCALARS);
    player->staging_values =
        realloc_or_die(player->staging_values, sizeof(float) * (size_t)batch);
  }
  memcpy(player->staging_board, board,
         sizeof(float) * (size_t)rows * VALUE_NET_BOARD_FLOATS);
  memcpy(player->staging_scalars, scalars,
         sizeof(float) * (size_t)rows * VALUE_NET_SCALARS);
  // Padding rows repeat the first row; their values are ignored.
  for (int pad_idx = rows; pad_idx < batch; pad_idx++) {
    memcpy(player->staging_board + ((size_t)pad_idx * VALUE_NET_BOARD_FLOATS),
           board, sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(player->staging_scalars + ((size_t)pad_idx * VALUE_NET_SCALARS),
           scalars, sizeof(float) * VALUE_NET_SCALARS);
  }
  value_net_metal_evaluate(player->metal, batch, player->staging_board,
                           player->staging_scalars, player->staging_values,
                           NULL);
  memcpy(values, player->staging_values, sizeof(float) * (size_t)rows);
  cpthread_mutex_unlock(&player->staging_mutex);
}

void value_net_player_evaluate_rows(void *context, int rows, const float *board,
                                    const float *scalars, float *values) {
  ValueNetPlayer *player = context;
  if (rows <= 0) {
    return;
  }
  if (player->coreml != NULL && player->metal != NULL) {
    // Whole calls go to the GPU when it is idle and to the Neural Engine
    // otherwise: splitting one call between them leaves the faster engine
    // waiting, and the Neural Engine takes concurrent callers while the GPU
    // runs one evaluation at a time.
    bool idle = false;
    if (atomic_compare_exchange_strong(&player->metal_busy, &idle, true)) {
      value_net_player_evaluate_metal(player, rows, board, scalars, values);
      atomic_store(&player->metal_busy, false);
    } else {
      value_net_coreml_evaluate(player->coreml, rows, board, scalars, values);
    }
    return;
  }
  if (player->coreml != NULL) {
    value_net_coreml_evaluate(player->coreml, rows, board, scalars, values);
  } else if (player->metal != NULL) {
    value_net_player_evaluate_metal(player, rows, board, scalars, values);
  } else {
    value_net_evaluate_cpu(player->net, rows, board, scalars, values, NULL);
  }
}

const Move *value_net_player_choose(ValueNetPlayer *player, const Game *game,
                                    const ValueNetHistory *history) {
  MoveList *list = player->list;
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  move_list_sort_moves(list);
  const int count = move_list_get_count(list);
  if (count <= 1 || bag_get_letters(game_get_bag(game)) == 0) {
    return move_list_get_move(list, 0);
  }
  if (player->scratch == NULL) {
    player->scratch = game_duplicate(game);
  }
  for (int move_idx = 0; move_idx < count; move_idx++) {
    value_net_features_for_move(
        game, move_list_get_move(list, move_idx), history, player->scratch,
        player->board_rows + ((size_t)move_idx * VALUE_NET_BOARD_FLOATS),
        player->scalar_rows + ((size_t)move_idx * VALUE_NET_SCALARS));
  }
  value_net_player_evaluate_rows(player, count, player->board_rows,
                                 player->scalar_rows, player->values);
  int best = 0;
  for (int move_idx = 1; move_idx < count; move_idx++) {
    const float value = player->values[move_idx];
    const float best_value = player->values[best];
    if (value > best_value ||
        (value == best_value &&
         move_get_tiles_played(move_list_get_move(list, move_idx)) >
             move_get_tiles_played(move_list_get_move(list, best)))) {
      best = move_idx;
    }
  }
  return move_list_get_move(list, best);
}

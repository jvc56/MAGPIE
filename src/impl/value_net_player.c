#include "value_net_player.h"

#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/value_net_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/value_net.h"
#include "../util/io_util.h"
#include "move_gen.h"
#include "value_net_features.h"
#include "value_net_metal.h"
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
  int max_candidates;
  int batch_capacity;
  MoveList *list;
  Game *scratch;
  float *board_rows;
  float *scalar_rows;
  float *values;
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
  if (backend != VALUE_NET_BACKEND_CPU) {
    metal = value_net_metal_create(net, backend == VALUE_NET_BACKEND_METAL_FP16,
                                   error_stack);
    if (metal == NULL) {
      value_net_destroy(net);
      return NULL;
    }
  }
  ValueNetPlayer *player = calloc_or_die(1, sizeof(ValueNetPlayer));
  player->net = net;
  player->metal = metal;
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
  return player;
}

void value_net_player_destroy(ValueNetPlayer *player) {
  if (player == NULL) {
    return;
  }
  value_net_metal_destroy(player->metal);
  value_net_destroy(player->net);
  move_list_destroy(player->list);
  game_destroy(player->scratch);
  free(player->board_rows);
  free(player->scalar_rows);
  free(player->values);
  free(player);
}

// The smallest padded batch size holding rows rows.
static int value_net_player_batch(const ValueNetPlayer *player, int rows) {
  int batch = VALUE_NET_PLAYER_MIN_BATCH;
  while (batch < rows && batch < player->batch_capacity) {
    batch *= 2;
  }
  return batch;
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
  if (player->metal != NULL) {
    // Padding rows repeat the first row; their values are ignored.
    const int batch = value_net_player_batch(player, count);
    for (int pad_idx = count; pad_idx < batch; pad_idx++) {
      memcpy(player->board_rows + ((size_t)pad_idx * VALUE_NET_BOARD_FLOATS),
             player->board_rows, sizeof(float) * VALUE_NET_BOARD_FLOATS);
      memcpy(player->scalar_rows + ((size_t)pad_idx * VALUE_NET_SCALARS),
             player->scalar_rows, sizeof(float) * VALUE_NET_SCALARS);
    }
    value_net_metal_evaluate(player->metal, batch, player->board_rows,
                             player->scalar_rows, player->values, NULL);
  } else {
    value_net_evaluate_cpu(player->net, count, player->board_rows,
                           player->scalar_rows, player->values, NULL);
  }
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

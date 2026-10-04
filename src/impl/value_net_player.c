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
  // With both engines, the Neural Engine calls in flight at most (see
  // value_net_player_evaluate_rows).
  VALUE_NET_PLAYER_COREML_SLOTS = 2,
};

struct ValueNetPlayer {
  ValueNet *net;
  ValueNetMetal *metal;
  ValueNetCoreML *coreml;
  // With both engines, calls in flight (or waiting) on the GPU, its
  // concurrency, and calls on the Neural Engine (see
  // value_net_player_evaluate_rows).
  atomic_int metal_in_flight;
  // Rows and calls evaluated through value_net_player_evaluate_rows.
  atomic_llong rows_evaluated;
  atomic_llong calls_evaluated;
  int metal_slots;
  atomic_int coreml_in_flight;
  int max_candidates;
  // The utility value_net_player_choose ranks candidates by.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  MoveList *list;
  Game *scratch;
  float *board_rows;
  float *scalar_rows;
  float *values;
  float *spreads;
  // The cascade (value_net_player_set_rescorer); NULL when off.
  ValueNetPlayer *rescorer;
  int rescore_top;
  double *utilities;
  int *ranked;
};

ValueNetPlayer *
value_net_player_create(const char *model_dir, value_net_backend_t backend,
                        int max_candidates, double utility_w_winpct,
                        double utility_w_spread, double utility_spread_scale,
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
                                   0, error_stack);
    if (metal == NULL) {
      value_net_coreml_destroy(coreml);
      value_net_destroy(net);
      return NULL;
    }
  }
  ValueNetPlayer *player = calloc_or_die(1, sizeof(ValueNetPlayer));
  player->net = net;
  player->metal = metal;
  player->metal_slots =
      metal != NULL ? value_net_metal_get_concurrency(metal) : 0;
  player->coreml = coreml;
  player->max_candidates =
      max_candidates > 0 ? max_candidates : VALUE_NET_PLAYER_DEFAULT_CANDIDATES;
  player->utility_w_winpct = utility_w_winpct;
  player->utility_w_spread = utility_w_spread;
  player->utility_spread_scale = utility_spread_scale;
  player->list = move_list_create(player->max_candidates);
  player->board_rows = calloc_or_die(
      (size_t)player->max_candidates * VALUE_NET_BOARD_FLOATS, sizeof(float));
  player->scalar_rows = calloc_or_die(
      (size_t)player->max_candidates * VALUE_NET_SCALARS, sizeof(float));
  player->values = calloc_or_die((size_t)player->max_candidates, sizeof(float));
  player->spreads =
      calloc_or_die((size_t)player->max_candidates, sizeof(float));
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
  free(player->spreads);
  free(player->utilities);
  free(player->ranked);
  free(player);
}

void value_net_player_set_rescorer(ValueNetPlayer *player,
                                   ValueNetPlayer *rescorer, int rescore_top) {
  player->rescorer = rescorer;
  player->rescore_top = rescore_top < 1 ? 1 : rescore_top;
  if (rescorer != NULL && player->rescore_top > rescorer->max_candidates) {
    player->rescore_top = rescorer->max_candidates;
  }
  if (rescorer != NULL && player->utilities == NULL) {
    player->utilities =
        calloc_or_die((size_t)player->max_candidates, sizeof(double));
    player->ranked = calloc_or_die((size_t)player->max_candidates, sizeof(int));
  }
}

// Takes one of slots places in in_flight if one is free.
static bool value_net_player_try_take(atomic_int *in_flight, int slots) {
  int current = atomic_load(in_flight);
  while (current < slots) {
    if (atomic_compare_exchange_weak(in_flight, &current, current + 1)) {
      return true;
    }
  }
  return false;
}

void value_net_player_evaluate_rows(void *context, int rows, const float *board,
                                    const float *scalars, float *values,
                                    float *spreads) {
  ValueNetPlayer *player = context;
  if (rows <= 0) {
    return;
  }
  atomic_fetch_add_explicit(&player->rows_evaluated, rows,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&player->calls_evaluated, 1, memory_order_relaxed);
  if (player->coreml != NULL && player->metal != NULL) {
    // Whole calls go to the GPU while one of its slots is free, then to the
    // Neural Engine while it runs fewer than VALUE_NET_PLAYER_COREML_SLOTS
    // calls, then to the GPU, which queues them. Splitting one call between
    // the engines would leave the faster one waiting, and the Neural Engine,
    // several times slower per row, would hold its callers up if it took
    // every call the GPU had no slot for.
    const bool on_gpu = value_net_player_try_take(&player->metal_in_flight,
                                                  player->metal_slots);
    if (!on_gpu && value_net_player_try_take(&player->coreml_in_flight,
                                             VALUE_NET_PLAYER_COREML_SLOTS)) {
      value_net_coreml_evaluate(player->coreml, rows, board, scalars, values,
                                spreads);
      atomic_fetch_sub(&player->coreml_in_flight, 1);
      return;
    }
    if (!on_gpu) {
      atomic_fetch_add(&player->metal_in_flight, 1);
    }
    value_net_metal_evaluate(player->metal, rows, board, scalars, values,
                             spreads);
    atomic_fetch_sub(&player->metal_in_flight, 1);
    return;
  }
  if (player->coreml != NULL) {
    value_net_coreml_evaluate(player->coreml, rows, board, scalars, values,
                              spreads);
  } else if (player->metal != NULL) {
    value_net_metal_evaluate(player->metal, rows, board, scalars, values,
                             spreads);
  } else {
    value_net_evaluate_cpu(player->net, rows, board, scalars, values, spreads);
  }
}

// Whether candidate first_idx ranks above candidate second_idx: higher
// utility, then more tiles played.
static bool value_net_player_ranks_above(const MoveList *list,
                                         const double *utilities, int first_idx,
                                         int second_idx) {
  if (utilities[first_idx] != utilities[second_idx]) {
    return utilities[first_idx] > utilities[second_idx];
  }
  return move_get_tiles_played(move_list_get_move(list, first_idx)) >
         move_get_tiles_played(move_list_get_move(list, second_idx));
}

// The cascade's choice among the count candidates player's net has
// scored: the best by player's utility from the rescorer's outputs of the
// top rescore_top by player's own.
static int value_net_player_rescore(ValueNetPlayer *player, const Game *game,
                                    int count) {
  const MoveList *list = player->list;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    player->utilities[move_idx] = value_net_utility(
        player->values[move_idx], player->spreads[move_idx],
        value_net_spread_after_move(game, move_list_get_move(list, move_idx)),
        player->utility_w_winpct, player->utility_w_spread,
        player->utility_spread_scale);
  }
  // ranked[0..top): the top candidates, best first (among equals, the
  // earlier).
  const int top = count < player->rescore_top ? count : player->rescore_top;
  int ranked_count = 0;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    int slot = ranked_count;
    if (ranked_count < top) {
      ranked_count++;
    } else if (value_net_player_ranks_above(list, player->utilities, move_idx,
                                            player->ranked[top - 1])) {
      slot = top - 1;
    } else {
      continue;
    }
    while (slot > 0 &&
           value_net_player_ranks_above(list, player->utilities, move_idx,
                                        player->ranked[slot - 1])) {
      player->ranked[slot] = player->ranked[slot - 1];
      slot--;
    }
    player->ranked[slot] = move_idx;
  }
  ValueNetPlayer *rescorer = player->rescorer;
  for (int slot = 0; slot < top; slot++) {
    const size_t move_idx = (size_t)player->ranked[slot];
    memcpy(rescorer->board_rows + ((size_t)slot * VALUE_NET_BOARD_FLOATS),
           player->board_rows + (move_idx * VALUE_NET_BOARD_FLOATS),
           sizeof(float) * VALUE_NET_BOARD_FLOATS);
    memcpy(rescorer->scalar_rows + ((size_t)slot * VALUE_NET_SCALARS),
           player->scalar_rows + (move_idx * VALUE_NET_SCALARS),
           sizeof(float) * VALUE_NET_SCALARS);
  }
  value_net_player_evaluate_rows(
      rescorer, top, rescorer->board_rows, rescorer->scalar_rows,
      rescorer->values,
      player->utility_w_spread > 0.0 ? rescorer->spreads : NULL);
  int best = player->ranked[0];
  double best_utility = 0.0;
  for (int slot = 0; slot < top; slot++) {
    const int move_idx = player->ranked[slot];
    const Move *move = move_list_get_move(list, move_idx);
    const double utility = value_net_utility(
        rescorer->values[slot], rescorer->spreads[slot],
        value_net_spread_after_move(game, move), player->utility_w_winpct,
        player->utility_w_spread, player->utility_spread_scale);
    if (slot == 0 || utility > best_utility ||
        (utility == best_utility &&
         move_get_tiles_played(move) >
             move_get_tiles_played(move_list_get_move(list, best)))) {
      best = move_idx;
      best_utility = utility;
    }
  }
  return best;
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
  value_net_player_evaluate_rows(
      player, count, player->board_rows, player->scalar_rows, player->values,
      player->utility_w_spread > 0.0 ? player->spreads : NULL);
  if (player->rescorer != NULL) {
    return move_list_get_move(list,
                              value_net_player_rescore(player, game, count));
  }
  int best = 0;
  double best_utility = 0.0;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    const double utility = value_net_utility(
        player->values[move_idx], player->spreads[move_idx],
        value_net_spread_after_move(game, move), player->utility_w_winpct,
        player->utility_w_spread, player->utility_spread_scale);
    if (move_idx == 0 || utility > best_utility ||
        (utility == best_utility &&
         move_get_tiles_played(move) >
             move_get_tiles_played(move_list_get_move(list, best)))) {
      best = move_idx;
      best_utility = utility;
    }
  }
  return move_list_get_move(list, best);
}

void value_net_player_get_evaluated(ValueNetPlayer *player, int64_t *rows,
                                    int64_t *calls) {
  *rows = atomic_load_explicit(&player->rows_evaluated, memory_order_relaxed);
  *calls = atomic_load_explicit(&player->calls_evaluated, memory_order_relaxed);
}

bool value_net_player_has_rack_head(const ValueNetPlayer *player) {
  return value_net_has_rack_head(player->net);
}

void value_net_player_rack_odds(void *context, int rows, const float *board,
                                const float *scalars, const float *side,
                                float *theta) {
  ValueNetPlayer *player = context;
  if (!value_net_has_rack_head(player->net)) {
    log_fatal("the value net has no opponent-leave head");
  }
  const int hidden_dim = value_net_get_shape(player->net)->head_hidden;
  float *hidden = malloc_or_die(sizeof(float) * (size_t)rows * hidden_dim);
  if (player->metal != NULL) {
    value_net_metal_hidden(player->metal, rows, board, scalars, hidden);
  } else {
    value_net_hidden_cpu(player->net, rows, board, scalars, hidden);
  }
  for (int row = 0; row < rows; row++) {
    value_net_rack_head(player->net, hidden + ((size_t)row * hidden_dim),
                        side + ((size_t)row * VALUE_NET_RACK_SIDE),
                        theta + ((size_t)row * VALUE_NET_RACK_LETTERS));
  }
  free(hidden);
}

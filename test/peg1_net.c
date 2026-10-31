#include "peg1_net.h"

#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/value_net_history.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/value_net_features.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "peg_net_util.h"
#include "peg_world.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void peg1_reply_features(const Game *scenario, int mover, Game *work,
                         MoveList *small, Move *move, float *feature,
                         double *timing) {
  bool out, pass;
  int tiles;
  // Mover's immediate greedy value on the post-candidate board.
  int64_t start = ctimer_monotonic_ns();
  game_copy(work, scenario);
  game_set_player_on_turn_index(work, mover);
  const int now = greedy_endgame_move(work, small, move, &out, &tiles, &pass);
  feature[3] = (float)now;
  feature[4] = out;
  timing[1] += (double)(ctimer_monotonic_ns() - start) / 1e6;
  // Greedy playout from the opponent's reply to game over.
  start = ctimer_monotonic_ns();
  game_copy(work, scenario);
  int moves = 0;
  while (!game_over(work) && moves < PEG1_PLAYOUT_CAP) {
    const int value =
        greedy_endgame_move(work, small, move, &out, &tiles, &pass);
    if (moves == 0) {
      feature[0] = (float)value;
      feature[1] = out;
      feature[2] = (float)tiles;
      feature[9] = pass;
      timing[2] += (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else if (moves == 1) {
      feature[5] = (float)value;
      feature[6] = out;
    }
    play_move(move, work, NULL);
    moves++;
  }
  if (!game_over(work)) {
    log_fatal("1PEG greedy playout did not end");
  }
  feature[7] = (float)spread_for_player(work, mover);
  feature[8] = (float)moves;
  timing[3] += (double)(ctimer_monotonic_ns() - start) / 1e6;
}

// 1PEG network limits and the reply-feature groups a network reads (bits in
// its header): the opponent's reply, and the mover's immediate greedy value
// on the post-candidate board.
enum {
  PEG1_NET_MAX_INPUT = 160,
  PEG1_NET_MAX_WIDTH = 512,
  PEG1_NET_GROUP_OPP = 1,
  PEG1_NET_GROUP_MOVER_NOW = 2,
};

Peg1Scratch peg1_scratch_create(const Game *game) {
  Peg1Scratch work = {.scenario = game_duplicate(game),
                      .opp_root = game_duplicate(game),
                      .best_list = move_list_create(1),
                      .small = move_list_create_small(16),
                      .move = move_create(),
                      .found = move_create()};
  return work;
}

void peg1_scratch_destroy(Peg1Scratch *work) {
  game_destroy(work->scenario);
  game_destroy(work->opp_root);
  move_list_destroy(work->best_list);
  small_move_list_destroy(work->small);
  move_destroy(work->move);
  move_destroy(work->found);
}

void peg1_root_context_fill(const Game *root, Peg1Scratch *work,
                            Peg1RootContext *prep, bool opp) {
  prep->total = peg_world_public_unseen(root, prep->unseen);
  if (prep->total != RACK_SIZE + 1) {
    log_fatal("1PEG v2 needs eight unseen tiles, found %d", prep->total);
  }
  if (!opp) {
    return;
  }
  const int mover = game_get_player_on_turn_index(root);
  for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
    if (prep->unseen[tile] == 0) {
      continue;
    }
    peg_world_from_public_counts(root, prep->unseen, tile, work->opp_root);
    game_set_player_on_turn_index(work->opp_root, 1 - mover);
    uint64_t lanes = ~UINT64_C(0) >> (64 - 2 * BOARD_DIM);
    for (int idx = 0; idx < PEG_WORLD_ROOT_LANES; idx++) {
      Move *best = &prep->lane_best[tile][idx];
      peg_world_best_scoring_in_lanes(work->opp_root, work->best_list, lanes,
                                      NULL, best);
      if (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        lanes &= ~(UINT64_C(1) << peg_world_move_lane(best));
      }
    }
  }
}

void peg1_reply_features_v2(const Game *root, const Move *cand,
                            uint64_t touched, int tile,
                            const Peg1RootContext *prep, Peg1Scratch *work,
                            bool opp, bool mover_now, float *feature,
                            Peg1NetTiming *timing) {
  const int mover = game_get_player_on_turn_index(root);
  int64_t start = ctimer_monotonic_ns();
  peg_world_from_public_counts(root, prep->unseen, tile, work->scenario);
  play_move(cand, work->scenario, NULL);
  timing->world_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  const LetterDistribution *ld = game_get_ld(root);
  if (opp) {
    start = ctimer_monotonic_ns();
    const Move *base = NULL;
    for (int idx = 0; idx < PEG_WORLD_ROOT_LANES; idx++) {
      const Move *lb = &prep->lane_best[tile][idx];
      if (move_get_type(lb) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        break;
      }
      if (!((touched >> peg_world_move_lane(lb)) & 1)) {
        base = lb;
        break;
      }
    }
    peg_world_best_scoring_in_lanes(work->scenario, work->best_list,
                                    base == NULL ? 0 : touched, base,
                                    work->found);
    const bool placement =
        move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE;
    const int tiles = placement ? move_get_tiles_played(work->found) : 0;
    const bool out =
        placement && tiles == rack_get_total_letters(player_get_rack(
                                  game_get_player(work->scenario, 1 - mover)));
    feature[0] = (float)(equity_to_int(move_get_score(work->found)) +
                         (out ? equity_to_int(calculate_end_rack_points(
                                    player_get_rack(
                                        game_get_player(work->scenario, mover)),
                                    ld))
                              : 0));
    feature[1] = out;
    feature[2] = (float)tiles;
    feature[9] = !placement;
    timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
  if (mover_now) {
    start = ctimer_monotonic_ns();
    bool out, pass;
    int tiles;
    game_set_player_on_turn_index(work->scenario, mover);
    feature[3] = (float)greedy_endgame_move(work->scenario, work->small,
                                            work->move, &out, &tiles, &pass);
    feature[4] = out;
    game_set_player_on_turn_index(work->scenario, 1 - mover);
    timing->mover_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
}

static const double peg1_thresholds[PEG1_THRESHOLDS] = {-40, -20, -10, -5, 0,
                                                        5,   10,  20,  40};

Peg1UtilityNet *peg1_utility_net_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[7];
  if (!fread_le_uint32s(shape, 7, file) || shape[0] != PEG1_UTILITY_NET_MAGIC ||
      shape[1] < 1 || shape[1] > PEG1_NET_MAX_INPUT || shape[2] < 1 ||
      shape[2] > PEG1_NET_MAX_WIDTH || shape[3] < 1 ||
      shape[3] > PEG_STATIC_POOL_SIZE || (shape[4] & ~UINT32_C(3)) != 0 ||
      (shape[4] & PEG1_NET_GROUP_OPP) == 0 || shape[5] != RACK_SIZE ||
      shape[6] != PEG1_UTILITY_NET_OUTPUTS) {
    log_fatal("invalid 1PEG v3 network header");
  }
  Peg1UtilityNet *net = calloc_or_die(1, sizeof(Peg1UtilityNet));
  net->inputs = (int)shape[1];
  net->width = (int)shape[2];
  net->top_k = (int)shape[3];
  net->groups = shape[4];
  const size_t count = (size_t)net->width * (net->inputs + 1) +
                       (size_t)PEG1_UTILITY_NET_OUTPUTS * (net->width + 1);
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  net->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 1PEG v3 network weights");
  }
  memcpy(net->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  net->first = net->blob;
  net->bias = net->first + (size_t)net->width * net->inputs;
  net->out = net->bias + net->width;
  for (int out = 0; out < PEG1_UTILITY_NET_OUTPUTS; out++) {
    net->out_bias[out] =
        net->out[(size_t)PEG1_UTILITY_NET_OUTPUTS * net->width + out];
  }
  const bool mover_now = net->groups & PEG1_NET_GROUP_MOVER_NOW;
  const bool use[PEG1_REPLY_FEATURES] = {
      true, true, true, mover_now, mover_now, false, false, false, false, true};
  for (int idx = 0; idx < PEG1_REPLY_FEATURES; idx++) {
    if (use[idx]) {
      net->reply_index[net->reply_count++] = idx;
    }
  }
  net->estimates = mover_now ? 2 : 1;
  net->v2_inputs = (VALUE_NET_SCALARS - 4) + 2 + 4 * net->reply_count + 1;
  if (net->v2_inputs + net->estimates * PEG1_THRESHOLD_BLOCK != net->inputs) {
    log_fatal("1PEG v3 network input width %d, expected %d", net->inputs,
              net->v2_inputs + net->estimates * PEG1_THRESHOLD_BLOCK);
  }
  return net;
}

void peg1_utility_net_destroy(Peg1UtilityNet *net) {
  free(net->blob);
  free(net);
}

void peg1_utility_net_inputs(const Peg1UtilityNet *net, const Game *root,
                             const Move *candidates, int count,
                             Peg1Scratch *work, const Peg1RootContext *prep,
                             float *inputs, Peg1NetTiming *timing) {
  const bool mover_now = net->groups & PEG1_NET_GROUP_MOVER_NOW;
  ValueNetHistory history;
  value_net_history_reset(&history);
  for (int candidate = 0; candidate < count; candidate++) {
    const Move *cand = &candidates[candidate];
    float *row = &inputs[candidate * net->inputs];
    int64_t start = ctimer_monotonic_ns();
    float scalars[VALUE_NET_SCALARS];
    value_net_scalars_for_move(root, cand, &history, scalars);
    int col = 0;
    for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
      if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
        row[col++] = scalars[idx];
      }
    }
    const double after = value_net_spread_after_move(root, cand);
    row[col++] = (float)equity_to_double(move_get_equity(cand));
    row[col++] = (float)after;
    const uint64_t touched =
        peg_world_lane_influence(game_get_board(root), cand);
    timing->scalar_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
    double sums[PEG1_REPLY_FEATURES] = {0}, squares[PEG1_REPLY_FEATURES] = {0};
    double low[PEG1_REPLY_FEATURES], high[PEG1_REPLY_FEATURES];
    for (int idx = 0; idx < PEG1_REPLY_FEATURES; idx++) {
      low[idx] = INFINITY;
      high[idx] = -INFINITY;
    }
    double est_value[2][VALUE_NET_TILE_TYPES];
    double est_prob[VALUE_NET_TILE_TYPES];
    int worlds = 0;
    for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
      if (prep->unseen[tile] == 0) {
        continue;
      }
      float feature[PEG1_REPLY_FEATURES] = {0};
      peg1_reply_features_v2(root, cand, touched, tile, prep, work, true,
                             mover_now, feature, timing);
      const double probability = (double)prep->unseen[tile] / prep->total;
      for (int idx = 0; idx < net->reply_count; idx++) {
        const double value = feature[net->reply_index[idx]];
        sums[idx] += probability * value;
        squares[idx] += probability * value * value;
        low[idx] = fmin(low[idx], value);
        high[idx] = fmax(high[idx], value);
      }
      const double one_ply = after - (double)feature[0];
      est_value[0][worlds] = one_ply;
      est_value[1][worlds] =
          one_ply + (1.0 - (double)feature[1]) * (double)feature[3];
      est_prob[worlds++] = probability;
    }
    for (int idx = 0; idx < net->reply_count; idx++) {
      row[col + idx] = (float)sums[idx];
      row[col + net->reply_count + idx] =
          (float)sqrt(fmax(0, squares[idx] - sums[idx] * sums[idx]));
      row[col + 2 * net->reply_count + idx] = (float)low[idx];
      row[col + 3 * net->reply_count + idx] = (float)high[idx];
    }
    col += 4 * net->reply_count;
    row[col++] = (float)(after - sums[0]);
    for (int est = 0; est < net->estimates; est++) {
      double mean = 0, square = 0, lo = INFINITY, hi = -INFINITY;
      for (int threshold = 0; threshold < PEG1_THRESHOLDS; threshold++) {
        const double k = peg1_thresholds[threshold];
        double mass = 0;
        for (int world = 0; world < worlds; world++) {
          const double value = est_value[est][world];
          if (k == 0) {
            mass += est_prob[world] * (value > 0 ? 1.0 : value == 0 ? 0.5 : 0);
          } else if (value > k) {
            mass += est_prob[world];
          }
        }
        row[col++] = (float)mass;
      }
      for (int world = 0; world < worlds; world++) {
        const double value = est_value[est][world];
        mean += est_prob[world] * value;
        square += est_prob[world] * value * value;
        lo = fmin(lo, value);
        hi = fmax(hi, value);
      }
      row[col++] = (float)mean;
      row[col++] = (float)sqrt(fmax(0, square - mean * mean));
      row[col++] = (float)lo;
      row[col++] = (float)hi;
    }
    if (col != net->inputs) {
      log_fatal("1PEG v3 input assembly mismatch");
    }
  }
}

double peg1_utility_net_value(const Peg1UtilityNet *net, const float *input,
                              const Peg1UtilityWeights *utility, double *win,
                              double *spread) {
  float hidden[PEG1_NET_MAX_WIDTH];
  for (int unit = 0; unit < net->width; unit++) {
    const float *weights = &net->first[(size_t)unit * net->inputs];
    float sum = net->bias[unit];
    for (int col = 0; col < net->inputs; col++) {
      sum += weights[col] * input[col];
    }
    hidden[unit] = fmaxf(sum, 0);
  }
  float raw[PEG1_UTILITY_NET_OUTPUTS];
  for (int out = 0; out < PEG1_UTILITY_NET_OUTPUTS; out++) {
    float sum = net->out_bias[out];
    const float *weights = &net->out[(size_t)out * net->width];
    for (int unit = 0; unit < net->width; unit++) {
      sum += weights[unit] * hidden[unit];
    }
    raw[out] = sum;
  }
  *win = 1.0 / (1.0 + exp(-(double)raw[0]));
  *spread = (double)input[net->v2_inputs - 1] + 20.0 * (double)raw[1];
  if (utility->w_spread < 0) {
    return 10.0 * (double)raw[2];
  }
  return sim_utility_blend(*win, double_to_equity(*spread), utility->w_win,
                           utility->w_spread, utility->scale);
}

int peg1_utility_net_choose(const Peg1UtilityNet *first,
                            const Peg1UtilityNet *second, int shortlist,
                            const Peg1UtilityWeights *utility, const Game *root,
                            MoveList *pool, Move *candidates, int *count,
                            Peg1Scratch *work, float *inputs, float *inputs2,
                            double *values, double *wins, double *spreads,
                            Peg1NetTiming *timing, Peg1NetTiming *timing2) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = peg_world_static_pool(root, pool, candidates, &pass_rank);
  timing->movegen_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (*count == 0) {
    return -1;
  }
  const int scored = *count < first->top_k ? *count : first->top_k;
  start = ctimer_monotonic_ns();
  Peg1RootContext prep;
  peg1_root_context_fill(root, work, &prep, true);
  timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  peg1_utility_net_inputs(first, root, candidates, scored, work, &prep, inputs,
                          timing);
  start = ctimer_monotonic_ns();
  for (int candidate = 0; candidate < scored; candidate++) {
    values[candidate] =
        peg1_utility_net_value(first, &inputs[candidate * first->inputs],
                               utility, &wins[candidate], &spreads[candidate]);
  }
  timing->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (second == NULL) {
    int choice = 0;
    for (int candidate = 1; candidate < scored; candidate++) {
      if (values[candidate] > values[choice]) {
        choice = candidate;
      }
    }
    return choice;
  }
  const int listed = scored < shortlist ? scored : shortlist;
  bool taken[PEG_STATIC_POOL_SIZE] = {false};
  for (int slot = 0; slot < listed; slot++) {
    int best = -1;
    for (int candidate = 0; candidate < scored; candidate++) {
      if (!taken[candidate] && (best < 0 || values[candidate] > values[best])) {
        best = candidate;
      }
    }
    taken[best] = true;
  }
  int ranks[PEG_STATIC_POOL_SIZE];
  Move short_moves[PEG_STATIC_POOL_SIZE];
  int filled = 0;
  for (int candidate = 0; candidate < scored; candidate++) {
    if (taken[candidate]) {
      ranks[filled] = candidate;
      move_copy(&short_moves[filled++], &candidates[candidate]);
    }
  }
  peg1_utility_net_inputs(second, root, short_moves, filled, work, &prep,
                          inputs2, timing2);
  start = ctimer_monotonic_ns();
  int choice = 0;
  double best_value = -INFINITY;
  for (int slot = 0; slot < filled; slot++) {
    double win, spread;
    const double value = peg1_utility_net_value(
        second, &inputs2[slot * second->inputs], utility, &win, &spread);
    if (value > best_value) {
      best_value = value;
      choice = slot;
    }
  }
  timing2->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  return ranks[choice];
}

Peg1NetChooser *peg1_net_chooser_create(const char *first, const char *second,
                                        int shortlist, double w_spread,
                                        double scale, const Game *game,
                                        int slots) {
  Peg1NetChooser *net = calloc_or_die(1, sizeof(Peg1NetChooser));
  net->first = peg1_utility_net_load(first);
  net->second =
      strings_equal(second, "-") ? NULL : peg1_utility_net_load(second);
  net->shortlist = shortlist;
  net->utility =
      (Peg1UtilityWeights){.w_win = 1.0, .w_spread = w_spread, .scale = scale};
  net->slots = slots;
  net->slot = calloc_or_die((size_t)slots, sizeof(Peg1NetChooserSlot));
  for (int slot_idx = 0; slot_idx < slots; slot_idx++) {
    Peg1NetChooserSlot *slot = &net->slot[slot_idx];
    slot->work = peg1_scratch_create(game);
    slot->pool = move_list_create(PEG_STATIC_POOL_SIZE + 1);
    slot->inputs = malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE *
                                 net->first->inputs);
    slot->inputs2 =
        malloc_or_die(sizeof(float) * PEG_STATIC_POOL_SIZE *
                      (net->second == NULL ? 1 : net->second->inputs));
    slot->swap = game_duplicate(game);
  }
  net->pick_cache = peg_net_pick_cache_create();
  return net;
}

void peg1_net_chooser_reset(Peg1NetChooser *net) {
  peg_net_pick_cache_reset(net->pick_cache);
}

void peg1_net_chooser_destroy(Peg1NetChooser *net) {
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    Peg1NetChooserSlot *slot = &net->slot[slot_idx];
    peg1_scratch_destroy(&slot->work);
    move_list_destroy(slot->pool);
    free(slot->inputs);
    free(slot->inputs2);
    game_destroy(slot->swap);
  }
  free(net->slot);
  peg_net_pick_cache_destroy(net->pick_cache);
  peg1_utility_net_destroy(net->first);
  if (net->second != NULL) {
    peg1_utility_net_destroy(net->second);
  }
  free(net);
}

static int peg1_net_chooser_choose(Peg1NetChooser *net,
                                   Peg1NetChooserSlot *slot, const Game *game,
                                   Move *move) {
  Move candidates[PEG_STATIC_POOL_SIZE];
  double values[PEG_STATIC_POOL_SIZE], wins[PEG_STATIC_POOL_SIZE],
      spreads[PEG_STATIC_POOL_SIZE];
  Peg1NetTiming timing = {0}, timing2 = {0};
  int count;
  const int choice = peg1_utility_net_choose(
      net->first, net->second, net->shortlist, &net->utility, game, slot->pool,
      candidates, &count, &slot->work, slot->inputs, slot->inputs2, values,
      wins, spreads, &timing, &timing2);
  if (choice < 0) {
    move_set_as_pass(move);
  } else {
    move_copy(move, &candidates[choice]);
  }
  return choice;
}

bool peg1_net_chooser_pick(Peg1NetChooser *net, int slot_idx, const Game *game,
                           Move *move) {
  int unseen[VALUE_NET_TILE_TYPES];
  if (game_over(game) || bag_get_letters(game_get_bag(game)) != 1 ||
      peg_world_public_unseen(game, unseen) != RACK_SIZE + 1) {
    return false;
  }
  Peg1NetChooserSlot *slot = &net->slot[slot_idx];
  const int64_t start = ctimer_monotonic_ns();
  uint8_t key[PEG_NET_CACHE_KEY_BYTES];
  const uint64_t hash = net->cache ? peg_net_cache_key(game, key) : 0;
  const bool hit =
      net->cache && peg_net_pick_cache_get(net->pick_cache, key, hash, move);
  if (hit) {
    slot->hits++;
  } else {
    peg1_net_chooser_choose(net, slot, game, move);
    if (net->cache) {
      peg_net_pick_cache_put(net->pick_cache, key, hash, move);
    }
  }
  slot->ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  slot->calls++;
  if (net->check && hit) {
    Move fresh;
    peg1_net_chooser_choose(net, slot, game, &fresh);
    if (compare_moves(move, &fresh, true) != -1) {
      log_fatal("1PEG network pick cache returned a different move");
    }
  }
  if (net->check) {
    int actual = -1, other = -1;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      if (bag_get_letter(game_get_bag(game), (MachineLetter)ml) > 0) {
        actual = ml;
      }
    }
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
      if (unseen[ml] > 0 && ml != actual) {
        other = ml;
      }
    }
    if (other >= 0) {
      peg_world_from_public_counts(game, unseen, other, slot->swap);
      Move swapped;
      peg1_net_chooser_choose(net, slot, slot->swap, &swapped);
      if (compare_moves(move, &swapped, true) != -1) {
        log_fatal("1PEG network pick depends on the hidden tiles");
      }
      slot->checks++;
    }
  }
  return true;
}

bool peg1_net_chooser_callback(const Game *game, int worker_idx, void *ctx,
                               Move *move) {
  Peg1NetChooser *net = ctx;
  if (worker_idx < 0 || worker_idx >= net->slots) {
    log_fatal("1PEG network chooser has no slot for PEG worker %d", worker_idx);
  }
  return peg1_net_chooser_pick(net, worker_idx, game, move);
}

void peg1_net_chooser_totals(const Peg1NetChooser *net, int64_t *calls,
                             int64_t *checks, double *ms) {
  *calls = 0;
  *checks = 0;
  *ms = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    *calls += net->slot[slot_idx].calls;
    *checks += net->slot[slot_idx].checks;
    *ms += net->slot[slot_idx].ms;
  }
}

int64_t peg1_net_chooser_cache_hits(const Peg1NetChooser *net) {
  int64_t hits = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    hits += net->slot[slot_idx].hits;
  }
  return hits;
}

#include "peg2_net.h"

#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/value_net_history.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/value_net_features.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "peg1_net.h"
#include "peg_net_util.h"
#include "peg_world.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void peg2_root_context_fill(const Game *root, Peg1Scratch *work,
                            Peg2RootContext *prep) {
  if (peg_world_public_unseen(root, prep->unseen) != PEG2_UNSEEN) {
    log_fatal("peg2feat root does not have nine unseen tiles");
  }
  const int mover = game_get_player_on_turn_index(root);
  Move pass;
  move_set_as_pass(&pass);
  prep->pairs = 0;
  for (int tile_a = 0; tile_a < VALUE_NET_TILE_TYPES; tile_a++) {
    for (int tile_b = tile_a; tile_b < VALUE_NET_TILE_TYPES; tile_b++) {
      const int pairs =
          tile_a == tile_b
              ? prep->unseen[tile_a] * (prep->unseen[tile_a] - 1) / 2
              : prep->unseen[tile_a] * prep->unseen[tile_b];
      if (pairs == 0) {
        continue;
      }
      const int pair = prep->pairs++;
      prep->tile_a[pair] = (MachineLetter)tile_a;
      prep->tile_b[pair] = (MachineLetter)tile_b;
      prep->count[pair] = pairs;
      // The opponent's rack in this world, on the pre-move board.
      const MachineLetter bag2[2] = {(MachineLetter)tile_a,
                                     (MachineLetter)tile_b};
      peg_world_after_candidate(root, prep->unseen, &pass, bag2, 0,
                                work->opp_root);
      game_set_player_on_turn_index(work->opp_root, 1 - mover);
      uint64_t lanes = ~UINT64_C(0) >> (64 - 2 * BOARD_DIM);
      for (int idx = 0; idx < PEG_WORLD_ROOT_LANES; idx++) {
        Move *best = &prep->lane_best[pair][idx];
        peg_world_best_scoring_in_lanes(work->opp_root, work->best_list, lanes,
                                        NULL, best);
        if (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
          lanes &= ~(UINT64_C(1) << peg_world_move_lane(best));
        }
      }
    }
  }
}

void peg2_opponent_top_reply(const Peg2RootContext *prep, int pair,
                             uint64_t touched, Game *world, int mover,
                             Peg1Scratch *work, float *feature) {
  const Move *base = NULL;
  for (int idx = 0; idx < PEG_WORLD_ROOT_LANES; idx++) {
    const Move *lb = &prep->lane_best[pair][idx];
    if (move_get_type(lb) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      break;
    }
    if (!((touched >> peg_world_move_lane(lb)) & 1)) {
      base = lb;
      break;
    }
  }
  peg_world_best_scoring_in_lanes(
      world, work->best_list, base == NULL ? 0 : touched, base, work->found);
  const bool placement =
      move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE;
  const int tiles = placement ? move_get_tiles_played(work->found) : 0;
  const bool out = placement && bag_get_letters(game_get_bag(world)) == 0 &&
                   tiles == rack_get_total_letters(player_get_rack(
                                game_get_player(world, 1 - mover)));
  feature[1] =
      (float)(equity_to_int(move_get_score(work->found)) +
              (out ? equity_to_int(calculate_end_rack_points(
                         player_get_rack(game_get_player(world, mover)),
                         game_get_ld(world)))
                   : 0));
  feature[2] = out;
  feature[3] = (float)tiles;
  feature[4] = !placement;
}

void peg2_reply_net_value(Peg1NetChooser *net, const Game *game, Move *pick,
                          double *win, double *spread, double *value) {
  Peg1NetChooserSlot *slot = &net->slot[0];
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
    move_set_as_pass(pick);
    *win = NAN;
    *spread = NAN;
    *value = NAN;
    return;
  }
  move_copy(pick, &candidates[choice]);
  if (net->second == NULL) {
    *win = wins[choice];
    *spread = spreads[choice];
    *value = values[choice];
    return;
  }
  Peg1RootContext prep;
  peg1_root_context_fill(game, &slot->work, &prep, true);
  peg1_utility_net_inputs(net->second, game, pick, 1, &slot->work, &prep,
                          slot->inputs2, &timing2);
  *value = peg1_utility_net_value(net->second, slot->inputs2, &net->utility,
                                  win, spread);
}

static const double peg2_thresholds[PEG2_THRESHOLDS] = {-40, -20, -10, -5, 0,
                                                        5,   10,  20,  40};

Peg2CandidateNet *peg2_candidate_net_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[4];
  if (!fread_le_uint32s(shape, 4, file) ||
      shape[0] != PEG2_CANDIDATE_NET_MAGIC ||
      (shape[1] != PEG2_NET_INPUT && shape[1] != PEG2_NET_FIRST_STAGE_INPUT) ||
      shape[2] < 1 || shape[2] > 1024 || shape[3] < 1 ||
      shape[3] > PEG_STATIC_POOL_SIZE) {
    log_fatal("invalid 2PEG chooser model");
  }
  Peg2CandidateNet *model = calloc_or_die(1, sizeof(Peg2CandidateNet));
  model->inputs = (int)shape[1];
  model->width = (int)shape[2];
  model->top_k = (int)shape[3];
  const size_t count = (size_t)model->width * (model->inputs + 2) + 1;
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  model->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 2PEG chooser weights");
  }
  memcpy(model->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  model->first = model->blob;
  model->bias = model->first + (size_t)model->width * model->inputs;
  model->out = model->bias + model->width;
  model->out_bias = model->out[model->width];
  return model;
}

void peg2_candidate_net_destroy(Peg2CandidateNet *model) {
  free(model->blob);
  free(model);
}

static float peg2_candidate_net_score(const Peg2CandidateNet *model,
                                      const float *input) {
  float total = model->out_bias;
  for (int unit = 0; unit < model->width; unit++) {
    const float *weights = &model->first[(size_t)unit * model->inputs];
    float sum = model->bias[unit];
    for (int col = 0; col < model->inputs; col++) {
      sum += weights[col] * input[col];
    }
    total += model->out[unit] * (sum > 0 ? sum : 0);
  }
  return total;
}

// The cheap-tier world rows of one candidate, in peg2feat's layout (network
// columns zero; without greedy, columns 5 and 6 zero too). Returns the number
// of rows. The candidate's post-move world is built once (base) and reset per
// bag pair; after a one-tile play both draw orders leave the opponent the
// same rack on the same board (and the bag not empty), so the second order
// reuses the first's reply.
static int peg2_net_world_rows(const Game *root, const Peg2RootContext *prep,
                               const Move *cand, bool greedy, Peg1Scratch *work,
                               Game *world, Game *base, float *rows,
                               Peg2PickTiming *timing) {
  const int mover = game_get_player_on_turn_index(root);
  const int tiles = move_get_tiles_played(cand);
  const int n_drawn = tiles < 2 ? tiles : 2;
  const uint64_t touched = peg_world_lane_influence(game_get_board(root), cand);
  const bool cache = peg_world_cache_enabled() && n_drawn > 0;
  bool base_ready = false;
  Rack own_after;
  int on_turn = 0;
  double weight_sum = 0;
  int filled = 0;
  for (int pair = 0; pair < prep->pairs; pair++) {
    const MachineLetter tile_a = prep->tile_a[pair];
    const MachineLetter tile_b = prep->tile_b[pair];
    const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
    for (int order = 0; order < orders; order++) {
      const MachineLetter bag2[2] = {order == 0 ? tile_a : tile_b,
                                     order == 0 ? tile_b : tile_a};
      float *feature = &rows[(size_t)filled * PEG2_FEATURE_WIDTH];
      memset(feature, 0, sizeof(float) * PEG2_FEATURE_WIDTH);
      const double weight =
          n_drawn == 1
              ? (tile_a == tile_b ? 2.0 * prep->count[pair] : prep->count[pair])
              : 2.0 * prep->count[pair];
      feature[0] = (float)(weight / PEG2_ORDERED_PAIRS);
      weight_sum += weight;
      feature[14] = n_drawn == 1 ? bag2[0] : -1;
      feature[15] = n_drawn == 1 ? bag2[1] : -1;
      filled++;
      int64_t start = ctimer_monotonic_ns();
      Game *target = world;
      if (!cache) {
        peg_world_after_candidate(root, prep->unseen, cand, bag2, n_drawn,
                                  world);
      } else {
        if (!base_ready) {
          peg_world_after_candidate(root, prep->unseen, cand, bag2, n_drawn,
                                    base);
          rack_copy(&own_after, player_get_rack(game_get_player(base, mover)));
          for (int idx = 0; idx < n_drawn; idx++) {
            rack_take_letter(&own_after, bag2[idx]);
          }
          on_turn = game_get_player_on_turn_index(base);
          base_ready = true;
        } else {
          peg_world_set_bag_pair(prep->unseen, &own_after, bag2, n_drawn, mover,
                                 on_turn, base);
        }
        target = base;
        // Greedy plays the reply after a one-tile play: work on a copy.
        if (greedy && n_drawn == 1) {
          game_copy(world, base);
          target = world;
        }
      }
      if (game_over(target)) {
        timing->opp_ns += ctimer_monotonic_ns() - start;
        continue;
      }
      if (cache && order == 1) {
        memcpy(&feature[1], &feature[1 - PEG2_FEATURE_WIDTH],
               sizeof(float) * 4);
      } else {
        peg2_opponent_top_reply(prep, pair, touched, target, mover, work,
                                feature);
      }
      if (!greedy) {
        timing->opp_ns += ctimer_monotonic_ns() - start;
        continue;
      }
      if (n_drawn == 1) {
        if (move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
          play_move(work->found, target, NULL);
        } else {
          Move pass_move;
          move_set_as_pass(&pass_move);
          play_move(&pass_move, target, NULL);
        }
      } else {
        game_set_player_on_turn_index(target, mover);
      }
      const int64_t mid = ctimer_monotonic_ns();
      timing->opp_ns += mid - start;
      bool out = false, pass;
      int greedy_tiles;
      if (!game_over(target)) {
        feature[5] = (float)greedy_endgame_move(target, work->small, work->move,
                                                &out, &greedy_tiles, &pass);
      }
      feature[6] = out;
      timing->greedy_ns += ctimer_monotonic_ns() - mid;
    }
  }
  if (weight_sum != PEG2_ORDERED_PAIRS) {
    log_fatal("peg2pick world weights do not cover the unseen pool");
  }
  return filled;
}

// The model input of one candidate (scripts/peg2_cv.py cand_features, public
// and cheap groups).
static void peg2_net_input_from_rows(const float *public_row, int tiles,
                                     const float *rows, int count, double after,
                                     double *columns, double *prob,
                                     float *input) {
  for (int idx = 0; idx < PEG2_NET_PUBLIC; idx++) {
    input[idx] = public_row[idx];
  }
  const bool one = tiles == 1;
  input[PEG2_NET_PUBLIC] = (float)tiles;
  input[PEG2_NET_PUBLIC + 1] = one;
  for (int row = 0; row < count; row++) {
    const float *feature = &rows[(size_t)row * PEG2_FEATURE_WIDTH];
    prob[row] = feature[0];
    for (int col = 0; col < 6; col++) {
      columns[(size_t)row * 8 + col] = feature[col + 1];
    }
    const double est1 = after - (double)feature[1];
    columns[(size_t)row * 8 + 6] = est1;
    columns[(size_t)row * 8 + 7] =
        est1 + (one ? (double)feature[5]
                    : (1 - (double)feature[2]) * (double)feature[5]);
  }
  double cheap[PEG2_NET_WORLD_SUMMARY];
  for (int col = 0; col < 6; col++) {
    weighted_stats(&columns[col], 8, prob, count, &cheap[col], 6);
  }
  for (int est = 0; est < 2; est++) {
    double *block = &cheap[24 + est * (PEG2_THRESHOLDS + 4)];
    for (int idx = 0; idx < PEG2_THRESHOLDS; idx++) {
      const double threshold = peg2_thresholds[idx];
      double mass = 0;
      for (int row = 0; row < count; row++) {
        const double value = columns[(size_t)row * 8 + 6 + est];
        if (threshold == 0) {
          mass += ((value > 0) + .5 * (value == 0)) * prob[row];
        } else {
          mass += (value > threshold) * prob[row];
        }
      }
      block[idx] = mass;
    }
    weighted_stats(&columns[6 + est], 8, prob, count, &block[PEG2_THRESHOLDS],
                   1);
  }
  for (int idx = 0; idx < PEG2_NET_WORLD_SUMMARY; idx++) {
    input[PEG2_NET_PUBLIC + 2 + idx] = (float)cheap[idx];
  }
}

// The first-stage input: public columns and the opponent-reply columns of a
// full input.
static void peg2_net_first_stage_input(const float *input, float *opp) {
  int col = 0;
  for (int idx = 0; idx < PEG2_NET_PUBLIC + 2; idx++) {
    opp[col++] = input[idx];
  }
  const float *cheap = &input[PEG2_NET_PUBLIC + 2];
  for (int block = 0; block < 4; block++) {
    for (int idx = 0; idx < 4; idx++) {
      opp[col++] = cheap[block * 6 + idx];
    }
  }
  for (int idx = 24; idx < 24 + PEG2_THRESHOLDS + 4; idx++) {
    opp[col++] = cheap[idx];
  }
}

Peg2PickScratch peg2_pick_scratch_create(const Game *game) {
  Peg2PickScratch work = {
      .peg1 = peg1_scratch_create(game),
      .world = game_duplicate(game),
      .base = game_duplicate(game),
      .pool = move_list_create(PEG_STATIC_POOL_SIZE + 1),
      .rows = malloc_or_die(sizeof(float) * PEG2_ORDERED_PAIRS *
                            PEG2_FEATURE_WIDTH),
      .columns = malloc_or_die(sizeof(double) * PEG2_ORDERED_PAIRS * 8),
      .prob = malloc_or_die(sizeof(double) * PEG2_ORDERED_PAIRS)};
  return work;
}

void peg2_pick_scratch_destroy(Peg2PickScratch *work) {
  peg1_scratch_destroy(&work->peg1);
  game_destroy(work->world);
  game_destroy(work->base);
  move_list_destroy(work->pool);
  free(work->rows);
  free(work->columns);
  free(work->prob);
}

// The model input of rank's candidate from its world rows.
static void peg2_net_candidate_input(const Game *root, const Move *cand,
                                     Peg2PickScratch *work, int filled,
                                     float *input) {
  ValueNetHistory history;
  value_net_history_reset(&history);
  float scalars[VALUE_NET_SCALARS];
  value_net_scalars_for_move(root, cand, &history, scalars);
  float public_row[PEG2_NET_PUBLIC];
  int col = 0;
  for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
    if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
      public_row[col++] = scalars[idx];
    }
  }
  const double after = value_net_spread_after_move(root, cand);
  public_row[col++] = (float)equity_to_double(move_get_equity(cand));
  public_row[col++] = (float)after;
  peg2_net_input_from_rows(public_row, move_get_tiles_played(cand), work->rows,
                           filled, after, work->columns, work->prob, input);
}

int peg2_net_staged_pick(const Game *root, const Peg2CandidateNet *model,
                         const Peg2CandidateNet *first, int stage_n, int top_k,
                         Peg2PickScratch *work, Move *candidates, int *count,
                         int *scored, float *scores, float *first_scores,
                         Peg2PickTiming *timing, FILE *dump) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = peg_world_static_pool(root, work->pool, candidates, &pass_rank);
  timing->pool_ns += ctimer_monotonic_ns() - start;
  *scored = *count < top_k ? *count : top_k;
  if (*scored == 0) {
    return -1;
  }
  start = ctimer_monotonic_ns();
  Peg2RootContext prep;
  peg2_root_context_fill(root, &work->peg1, &prep);
  timing->prep_ns += ctimer_monotonic_ns() - start;
  float input[PEG2_NET_INPUT];
  bool full[PEG_STATIC_POOL_SIZE];
  for (int rank = 0; rank < *scored; rank++) {
    full[rank] = first == NULL;
    scores[rank] = NAN;
  }
  if (first != NULL) {
    for (int rank = 0; rank < *scored; rank++) {
      const int filled = peg2_net_world_rows(root, &prep, &candidates[rank],
                                             false, &work->peg1, work->world,
                                             work->base, work->rows, timing);
      if (dump != NULL) {
        fwrite(work->rows, sizeof(float), (size_t)filled * PEG2_FEATURE_WIDTH,
               dump);
      }
      start = ctimer_monotonic_ns();
      peg2_net_candidate_input(root, &candidates[rank], work, filled, input);
      float opp[PEG2_NET_FIRST_STAGE_INPUT];
      peg2_net_first_stage_input(input, opp);
      first_scores[rank] = peg2_candidate_net_score(first, opp);
      timing->model_ns += ctimer_monotonic_ns() - start;
    }
    // The first model's best N (ties to the better static rank).
    for (int pick = 0; pick < stage_n && pick < *scored; pick++) {
      int best = -1;
      for (int rank = 0; rank < *scored; rank++) {
        if (!full[rank] &&
            (best < 0 || first_scores[rank] > first_scores[best])) {
          best = rank;
        }
      }
      full[best] = true;
    }
  }
  int best = -1;
  for (int rank = 0; rank < *scored; rank++) {
    if (!full[rank]) {
      continue;
    }
    const int filled =
        peg2_net_world_rows(root, &prep, &candidates[rank], true, &work->peg1,
                            work->world, work->base, work->rows, timing);
    if (dump != NULL && first == NULL) {
      fwrite(work->rows, sizeof(float), (size_t)filled * PEG2_FEATURE_WIDTH,
             dump);
    }
    start = ctimer_monotonic_ns();
    peg2_net_candidate_input(root, &candidates[rank], work, filled, input);
    scores[rank] = peg2_candidate_net_score(model, input);
    if (best < 0 || scores[rank] > scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  return best;
}

int peg2_net_wide_pick(const Game *root, const Peg2CandidateNet *first,
                       const Peg2CandidateNet *model, int stage_n,
                       Peg2PickScratch *work, MoveList *wide, Move *moves,
                       bool *taken, int *count, float *first_scores,
                       float *scores, Peg2PickTiming *timing) {
  int64_t start = ctimer_monotonic_ns();
  *count = peg_world_all_placements(root, wide, moves);
  timing->pool_ns += ctimer_monotonic_ns() - start;
  if (*count == 0) {
    return -1;
  }
  start = ctimer_monotonic_ns();
  Peg2RootContext prep;
  peg2_root_context_fill(root, &work->peg1, &prep);
  timing->prep_ns += ctimer_monotonic_ns() - start;
  float input[PEG2_NET_INPUT];
  float opp[PEG2_NET_FIRST_STAGE_INPUT];
  int best = -1;
  for (int rank = 0; rank < *count; rank++) {
    const int filled =
        peg2_net_world_rows(root, &prep, &moves[rank], false, &work->peg1,
                            work->world, work->base, work->rows, timing);
    start = ctimer_monotonic_ns();
    peg2_net_candidate_input(root, &moves[rank], work, filled, input);
    peg2_net_first_stage_input(input, opp);
    first_scores[rank] = peg2_candidate_net_score(first, opp);
    scores[rank] = NAN;
    taken[rank] = false;
    if (best < 0 || first_scores[rank] > first_scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  if (model == NULL) {
    return best;
  }
  for (int pick = 0; pick < stage_n && pick < *count; pick++) {
    int top = -1;
    for (int rank = 0; rank < *count; rank++) {
      if (!taken[rank] && (top < 0 || first_scores[rank] > first_scores[top])) {
        top = rank;
      }
    }
    taken[top] = true;
  }
  best = -1;
  for (int rank = 0; rank < *count; rank++) {
    if (!taken[rank]) {
      continue;
    }
    const int filled =
        peg2_net_world_rows(root, &prep, &moves[rank], true, &work->peg1,
                            work->world, work->base, work->rows, timing);
    start = ctimer_monotonic_ns();
    peg2_net_candidate_input(root, &moves[rank], work, filled, input);
    scores[rank] = peg2_candidate_net_score(model, input);
    if (best < 0 || scores[rank] > scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  return best;
}

Peg2NetChooser *peg2_net_chooser_create(const char *model_path,
                                        const char *first_path, int stage_n,
                                        int top_k, const Game *game,
                                        int slots) {
  Peg2NetChooser *chooser = calloc_or_die(1, sizeof(Peg2NetChooser));
  chooser->model = peg2_candidate_net_load(model_path);
  if (chooser->model->inputs != PEG2_NET_INPUT) {
    log_fatal("2PEG chooser model must use the full cheap input");
  }
  chooser->first = strings_equal(first_path, "-")
                       ? NULL
                       : peg2_candidate_net_load(first_path);
  if (chooser->first != NULL &&
      chooser->first->inputs != PEG2_NET_FIRST_STAGE_INPUT) {
    log_fatal("2PEG chooser first model must use the opponent-reply input");
  }
  chooser->stage_n = stage_n;
  chooser->top_k = top_k > 0 ? top_k : chooser->model->top_k;
  if (chooser->top_k > PEG_STATIC_POOL_SIZE ||
      (chooser->first != NULL && stage_n < 1)) {
    log_fatal("2PEG chooser invalid top K or stage N");
  }
  chooser->slots = slots;
  chooser->slot = calloc_or_die((size_t)slots, sizeof(Peg2NetChooserSlot));
  for (int slot_idx = 0; slot_idx < slots; slot_idx++) {
    chooser->slot[slot_idx].work = peg2_pick_scratch_create(game);
    chooser->slot[slot_idx].swap = game_duplicate(game);
  }
  chooser->pick_cache = peg_net_pick_cache_create();
  return chooser;
}

void peg2_net_chooser_reset(Peg2NetChooser *chooser) {
  peg_net_pick_cache_reset(chooser->pick_cache);
}

void peg2_net_chooser_destroy(Peg2NetChooser *chooser) {
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    peg2_pick_scratch_destroy(&chooser->slot[slot_idx].work);
    game_destroy(chooser->slot[slot_idx].swap);
  }
  free(chooser->slot);
  peg_net_pick_cache_destroy(chooser->pick_cache);
  peg2_candidate_net_destroy(chooser->model);
  if (chooser->first != NULL) {
    peg2_candidate_net_destroy(chooser->first);
  }
  free(chooser);
}

// The chooser's full pick for game (the static rank, -1 = pass) into move.
static int peg2_net_chooser_choose(Peg2NetChooser *chooser,
                                   Peg2NetChooserSlot *slot, const Game *game,
                                   Move *candidates, Move *move) {
  Peg2PickTiming timing = {0};
  int count, scored;
  float scores[PEG_STATIC_POOL_SIZE], first_scores[PEG_STATIC_POOL_SIZE];
  const int pick = peg2_net_staged_pick(
      game, chooser->model, chooser->first, chooser->stage_n, chooser->top_k,
      &slot->work, candidates, &count, &scored, scores, first_scores, &timing,
      NULL);
  if (pick < 0) {
    move_set_as_pass(move);
  } else {
    move_copy(move, &candidates[pick]);
  }
  return pick;
}

bool peg2_net_chooser_pick(Peg2NetChooser *chooser, int slot_idx,
                           const Game *game, Move *move) {
  int unseen[VALUE_NET_TILE_TYPES];
  if (game_over(game) || bag_get_letters(game_get_bag(game)) != 2 ||
      peg_world_public_unseen(game, unseen) != PEG2_UNSEEN) {
    return false;
  }
  Peg2NetChooserSlot *slot = &chooser->slot[slot_idx];
  const int64_t start = ctimer_monotonic_ns();
  uint8_t key[PEG_NET_CACHE_KEY_BYTES];
  const uint64_t hash = chooser->cache ? peg_net_cache_key(game, key) : 0;
  const bool hit = chooser->cache &&
                   peg_net_pick_cache_get(chooser->pick_cache, key, hash, move);
  if (hit) {
    slot->hits++;
  } else {
    peg2_net_chooser_choose(chooser, slot, game, slot->candidates, move);
    if (chooser->cache) {
      peg_net_pick_cache_put(chooser->pick_cache, key, hash, move);
    }
  }
  slot->ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  slot->calls++;
  if (chooser->check) {
    Move fresh;
    if (hit) {
      peg2_net_chooser_choose(chooser, slot, game, slot->candidates, &fresh);
      if (compare_moves(move, &fresh, true) != -1) {
        log_fatal("2PEG chooser pick cache returned a different move");
      }
    }
    if (peg_world_swap_hidden_tiles(game, slot->swap)) {
      peg2_net_chooser_choose(chooser, slot, slot->swap, slot->candidates2,
                              &fresh);
      if (compare_moves(move, &fresh, true) != -1) {
        log_fatal("2PEG chooser pick depends on the hidden tiles");
      }
      slot->checks++;
    }
  }
  return true;
}

bool peg2_net_chooser_callback(const Game *game, int worker_idx, void *ctx,
                               Move *move) {
  Peg2NetChooser *chooser = ctx;
  if (worker_idx < 0 || worker_idx >= chooser->slots) {
    log_fatal("2PEG chooser has no slot for PEG worker %d", worker_idx);
  }
  return peg2_net_chooser_pick(chooser, worker_idx, game, move);
}

void peg2_net_chooser_totals(const Peg2NetChooser *chooser, int64_t *calls,
                             int64_t *checks, double *ms, int64_t *hits) {
  *calls = 0;
  *checks = 0;
  *ms = 0;
  *hits = 0;
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    *calls += chooser->slot[slot_idx].calls;
    *checks += chooser->slot[slot_idx].checks;
    *ms += chooser->slot[slot_idx].ms;
    *hits += chooser->slot[slot_idx].hits;
  }
}

int peg2_net_chooser_rank(Peg2NetChooser *chooser, const Game *root,
                          Move *candidates, const Move **order, int max,
                          int *pick_rank) {
  Peg2NetChooserSlot *slot = &chooser->slot[0];
  Peg2PickTiming timing = {0};
  int count, scored;
  float scores[PEG_STATIC_POOL_SIZE], first_scores[PEG_STATIC_POOL_SIZE];
  *pick_rank = peg2_net_staged_pick(root, chooser->model, chooser->first,
                                    chooser->stage_n, chooser->top_k,
                                    &slot->work, candidates, &count, &scored,
                                    scores, first_scores, &timing, NULL);
  int idx[PEG_STATIC_POOL_SIZE];
  double key[PEG_STATIC_POOL_SIZE];
  for (int rank = 0; rank < count; rank++) {
    idx[rank] = rank;
    // Fully scored ranks first (by the full model), then by the first model.
    key[rank] = !isnan(scores[rank])     ? 1e9 + scores[rank]
                : chooser->first != NULL ? (double)first_scores[rank]
                                         : -1e18;
  }
  for (int left = 1; left < count; left++) {
    const int cur = idx[left];
    int pos = left;
    while (pos > 0 && (key[idx[pos - 1]] < key[cur] ||
                       (key[idx[pos - 1]] == key[cur] && idx[pos - 1] > cur))) {
      idx[pos] = idx[pos - 1];
      pos--;
    }
    idx[pos] = cur;
  }
  const int n = count < max ? count : max;
  for (int pos = 0; pos < n; pos++) {
    order[pos] = &candidates[idx[pos]];
  }
  if (*pick_rank >= 0 && n > 0 && order[0] != &candidates[*pick_rank]) {
    log_fatal("pegbudget ranking disagrees with the chooser's pick");
  }
  return n;
}

int peg2_net_root_ranker(const Game *game, void *ctx, Move *moves,
                         int max_moves) {
  Peg2NetRanker *ranker = ctx;
  const int64_t start = ctimer_monotonic_ns();
  Move candidates[PEG_STATIC_POOL_SIZE];
  const Move *order[PEG_STATIC_POOL_SIZE];
  int pass_rank, pick_rank;
  (void)peg_world_static_pool(game, ranker->pool, candidates, &pass_rank);
  const int n = peg2_net_chooser_rank(
      ranker->chooser, game, candidates, order,
      max_moves < PEG_STATIC_POOL_SIZE ? max_moves : PEG_STATIC_POOL_SIZE,
      &pick_rank);
  for (int rank = 0; rank < n; rank++) {
    move_copy(&moves[rank], order[rank]);
  }
  ranker->ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
  return n;
}

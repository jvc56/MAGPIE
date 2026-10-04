#include "value_net_inference.h"

#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/value_net_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include "value_net_features.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Racks whose candidates go to the net in one call.
  VALUE_NET_INFERENCE_RACKS_PER_CALL = 8,
  // Scale of the alias method's integer counts.
  VALUE_NET_INFERENCE_ALIAS_SCALE = 1000000,
};

struct ValueNetLeaveInference {
  int count;
  int capacity;
  Rack *leaves;
  double *log_prior;
  double *log_likelihood;
  double *probability;
  // Each leave's candidates' utilities, how many, and which is the move.
  double *utilities;
  int *utility_count;
  int *utility_move;
  int rows_evaluated;
  // Per call: each rack's candidates and where the target move is among
  // them, and the input rows.
  Game *scratch;
  Game *features_scratch;
  MoveList *list;
  Move *candidates;
  int *candidate_count;
  int *move_index;
  float *board;
  float *scalars;
  float *values;
  float *spreads;
  double *spread_after;
};

ValueNetLeaveInference *value_net_leave_inference_create(void) {
  ValueNetLeaveInference *inference =
      calloc_or_die(1, sizeof(ValueNetLeaveInference));
  const size_t rows = (size_t)VALUE_NET_INFERENCE_RACKS_PER_CALL *
                      (VALUE_NET_INFERENCE_MAX_CANDIDATES + 1);
  inference->list = move_list_create(VALUE_NET_INFERENCE_MAX_CANDIDATES);
  inference->candidates = malloc_or_die(sizeof(Move) * rows);
  inference->candidate_count =
      malloc_or_die(sizeof(int) * VALUE_NET_INFERENCE_RACKS_PER_CALL);
  inference->move_index =
      malloc_or_die(sizeof(int) * VALUE_NET_INFERENCE_RACKS_PER_CALL);
  inference->board =
      malloc_or_die(sizeof(float) * rows * VALUE_NET_BOARD_FLOATS);
  inference->scalars = malloc_or_die(sizeof(float) * rows * VALUE_NET_SCALARS);
  inference->values = malloc_or_die(sizeof(float) * rows);
  inference->spreads = calloc_or_die(rows, sizeof(float));
  inference->spread_after = malloc_or_die(sizeof(double) * rows);
  return inference;
}

void value_net_leave_inference_destroy(ValueNetLeaveInference *inference) {
  if (inference == NULL) {
    return;
  }
  free(inference->leaves);
  free(inference->log_prior);
  free(inference->log_likelihood);
  free(inference->probability);
  free(inference->utilities);
  free(inference->utility_count);
  free(inference->utility_move);
  if (inference->scratch != NULL) {
    game_destroy(inference->scratch);
  }
  if (inference->features_scratch != NULL) {
    game_destroy(inference->features_scratch);
  }
  move_list_destroy(inference->list);
  free(inference->candidates);
  free(inference->candidate_count);
  free(inference->move_index);
  free(inference->board);
  free(inference->scalars);
  free(inference->values);
  free(inference->spreads);
  free(inference->spread_after);
  free(inference);
}

static double log_choose(int n, int k) {
  return lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0);
}

static void value_net_leave_inference_add(ValueNetLeaveInference *inference,
                                          const Rack *leave, double log_prior) {
  if (inference->count == inference->capacity) {
    inference->capacity =
        inference->capacity == 0 ? 256 : inference->capacity * 2;
    inference->leaves = realloc_or_die(
        inference->leaves, sizeof(Rack) * (size_t)inference->capacity);
    inference->log_prior = realloc_or_die(
        inference->log_prior, sizeof(double) * (size_t)inference->capacity);
    inference->log_likelihood =
        realloc_or_die(inference->log_likelihood,
                       sizeof(double) * (size_t)inference->capacity);
    inference->probability = realloc_or_die(
        inference->probability, sizeof(double) * (size_t)inference->capacity);
    inference->utilities = realloc_or_die(
        inference->utilities, sizeof(double) * (size_t)inference->capacity *
                                  (VALUE_NET_INFERENCE_MAX_CANDIDATES + 1));
    inference->utility_count = realloc_or_die(
        inference->utility_count, sizeof(int) * (size_t)inference->capacity);
    inference->utility_move = realloc_or_die(
        inference->utility_move, sizeof(int) * (size_t)inference->capacity);
  }
  rack_copy(&inference->leaves[inference->count], leave);
  inference->log_prior[inference->count] = log_prior;
  inference->count++;
}

// Every multiset of remaining tiles from the letters letter onward of
// available, with its number of draws, into the inference's list.
static void enumerate_leaves(ValueNetLeaveInference *inference,
                             const int *available, int letters, int letter,
                             int remaining, Rack *leave, double log_prior) {
  if (remaining == 0) {
    value_net_leave_inference_add(inference, leave, log_prior);
    return;
  }
  if (letter == letters) {
    return;
  }
  const int most =
      available[letter] < remaining ? available[letter] : remaining;
  for (int count = 0; count <= most; count++) {
    for (int copy = 0; copy < count; copy++) {
      rack_add_letter(leave, (MachineLetter)letter);
    }
    enumerate_leaves(inference, available, letters, letter + 1,
                     remaining - count, leave,
                     log_prior + log_choose(available[letter], count));
    for (int copy = 0; copy < count; copy++) {
      rack_take_letter(leave, (MachineLetter)letter);
    }
  }
}

// Scores racks [first, first + racks) of the list: each rack's candidates
// and the target move go to the net in one call, and each rack's
// log likelihood of the target move is filled in.
static void score_racks(ValueNetLeaveInference *inference,
                        const ValueNetLeaveInferenceArgs *args,
                        const Rack *played, int first, int racks) {
  int rows = 0;
  for (int rack_idx = 0; rack_idx < racks; rack_idx++) {
    const Rack *leave = &inference->leaves[first + rack_idx];
    Game *game = inference->scratch;
    game_copy(game, args->before_move);
    Rack rack;
    rack_copy(&rack, played);
    rack_union(&rack, leave);
    return_rack_to_bag(game, args->target_index);
    if (!draw_rack_from_bag(game, args->target_index, &rack)) {
      log_fatal("value net inference could not draw a candidate rack");
    }
    MoveList *list = inference->list;
    move_list_reset(list);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .disable_pat = !args->use_pat,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(list);
    Move *candidates =
        inference->candidates +
        ((size_t)rack_idx * (VALUE_NET_INFERENCE_MAX_CANDIDATES + 1));
    int count = move_list_get_count(list);
    if (count > args->candidates) {
      count = args->candidates;
    }
    int move_index = -1;
    for (int idx = 0; idx < count; idx++) {
      move_copy(&candidates[idx], move_list_get_move(list, idx));
      if (move_index < 0 && compare_moves_without_equity(
                                &candidates[idx], args->move, true) == -1) {
        move_index = idx;
      }
    }
    if (move_index < 0) {
      move_copy(&candidates[count], args->move);
      move_index = count;
      count++;
    }
    inference->candidate_count[rack_idx] = count;
    inference->move_index[rack_idx] = move_index;
    for (int idx = 0; idx < count; idx++) {
      inference->spread_after[rows] =
          value_net_spread_after_move(game, &candidates[idx]);
      value_net_features_for_move(
          game, &candidates[idx], args->target_history,
          inference->features_scratch,
          inference->board + ((size_t)rows * VALUE_NET_BOARD_FLOATS),
          inference->scalars + ((size_t)rows * VALUE_NET_SCALARS));
      rows++;
    }
  }
  args->evaluate(args->evaluate_context, rows, inference->board,
                 inference->scalars, inference->values,
                 args->utility_w_spread > 0.0 ? inference->spreads : NULL);
  inference->rows_evaluated += rows;
  int row = 0;
  for (int rack_idx = 0; rack_idx < racks; rack_idx++) {
    const int leave_idx = first + rack_idx;
    const int count = inference->candidate_count[rack_idx];
    double *utilities =
        inference->utilities +
        ((size_t)leave_idx * (VALUE_NET_INFERENCE_MAX_CANDIDATES + 1));
    for (int idx = 0; idx < count; idx++) {
      utilities[idx] = value_net_utility(
          inference->values[row + idx], inference->spreads[row + idx],
          inference->spread_after[row + idx], args->utility_w_winpct,
          args->utility_w_spread, args->utility_spread_scale);
    }
    inference->utility_count[leave_idx] = count;
    inference->utility_move[leave_idx] = inference->move_index[rack_idx];
    row += count;
  }
}

bool value_net_leave_inference_run(ValueNetLeaveInference *inference,
                                   const ValueNetLeaveInferenceArgs *args) {
  inference->count = 0;
  inference->rows_evaluated = 0;
  if (move_get_type(args->move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
      args->leave_size < 1 || args->candidates < 1 ||
      args->candidates > VALUE_NET_INFERENCE_MAX_CANDIDATES) {
    return false;
  }
  const Game *before = args->before_move;
  const int letters = ld_get_size(game_get_ld(before));
  // The tiles unseen to the inferring player before the move: the bag and the
  // target's rack (the inferring player's own rack is not in the bag).
  int available[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(game_get_bag(before), available);
  const Rack *target_rack =
      player_get_rack(game_get_player(before, args->target_index));
  for (int letter = 0; letter < letters; letter++) {
    available[letter] += rack_get_letter(target_rack, letter);
  }
  // Less the tiles the move played (a blank played as a letter is a blank).
  Rack played;
  rack_set_dist_size_and_reset(&played, letters);
  for (int idx = 0; idx < move_get_tiles_length(args->move); idx++) {
    const MachineLetter ml = move_get_tile(args->move, idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const MachineLetter tile = get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
    rack_add_letter(&played, tile);
    available[tile]--;
    if (available[tile] < 0) {
      return false;
    }
  }
  Rack leave;
  rack_set_dist_size_and_reset(&leave, letters);
  enumerate_leaves(inference, available, letters, 0, args->leave_size, &leave,
                   0.0);
  if (inference->count == 0) {
    return false;
  }
  if (inference->scratch == NULL) {
    inference->scratch = game_duplicate(before);
    inference->features_scratch = game_duplicate(before);
  }
  for (int first = 0; first < inference->count;
       first += VALUE_NET_INFERENCE_RACKS_PER_CALL) {
    const int racks =
        inference->count - first < VALUE_NET_INFERENCE_RACKS_PER_CALL
            ? inference->count - first
            : VALUE_NET_INFERENCE_RACKS_PER_CALL;
    score_racks(inference, args, &played, first, racks);
  }
  value_net_leave_inference_set_temperature(inference, args->temperature);
  return true;
}

void value_net_leave_inference_set_temperature(
    ValueNetLeaveInference *inference, double temperature) {
  double highest = -INFINITY;
  for (int idx = 0; idx < inference->count; idx++) {
    const double *utilities =
        inference->utilities +
        ((size_t)idx * (VALUE_NET_INFERENCE_MAX_CANDIDATES + 1));
    double best = -INFINITY;
    for (int cand = 0; cand < inference->utility_count[idx]; cand++) {
      best = fmax(best, utilities[cand]);
    }
    double total = 0.0;
    for (int cand = 0; cand < inference->utility_count[idx]; cand++) {
      total += exp((utilities[cand] - best) / temperature);
    }
    inference->log_likelihood[idx] =
        (utilities[inference->utility_move[idx]] - best) / temperature -
        log(total);
    highest = fmax(highest,
                   inference->log_prior[idx] + inference->log_likelihood[idx]);
  }
  double total = 0.0;
  for (int idx = 0; idx < inference->count; idx++) {
    inference->probability[idx] = exp(inference->log_prior[idx] +
                                      inference->log_likelihood[idx] - highest);
    total += inference->probability[idx];
  }
  for (int idx = 0; idx < inference->count; idx++) {
    inference->probability[idx] /= total;
  }
}

int value_net_leave_inference_get_count(
    const ValueNetLeaveInference *inference) {
  return inference->count;
}

const Rack *
value_net_leave_inference_get_leave(const ValueNetLeaveInference *inference,
                                    int index) {
  return &inference->leaves[index];
}

double value_net_leave_inference_get_probability(
    const ValueNetLeaveInference *inference, int index) {
  return inference->probability[index];
}

double value_net_leave_inference_probability_of(
    const ValueNetLeaveInference *inference, const Rack *leave) {
  for (int idx = 0; idx < inference->count; idx++) {
    if (racks_are_equal(&inference->leaves[idx], leave)) {
      return inference->probability[idx];
    }
  }
  return 0.0;
}

int value_net_leave_inference_get_rows(
    const ValueNetLeaveInference *inference) {
  return inference->rows_evaluated;
}

bool value_net_leave_inference_fill_alias(
    const ValueNetLeaveInference *inference, AliasMethod *alias_method) {
  alias_method_reset(alias_method);
  bool any = false;
  for (int idx = 0; idx < inference->count; idx++) {
    const int count = (int)lround(inference->probability[idx] *
                                  VALUE_NET_INFERENCE_ALIAS_SCALE);
    if (count > 0) {
      alias_method_add_rack(alias_method, &inference->leaves[idx], count);
      any = true;
    }
  }
  return any && alias_method_generate_tables(alias_method);
}

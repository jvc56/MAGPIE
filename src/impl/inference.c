#include "inference.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"
#include "../str/inference_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/math_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Inference {
  // KLV used to evaluate leaves to determine
  // which moves are top equity. This should be
  // the KLV of the target.
  const KLV *klv;
  ThreadControl *thread_control;

  // The following fields are owned by this struct.

  int ld_size;
  int move_capacity;
  // Target player index in the game
  int target_index;
  // Target player score
  Equity target_score;
  // Number of tiles exchanged by the target
  int target_number_of_tiles_exchanged;
  // Maximum equity loss the target can
  // lose while still being considered
  // the top move.
  Equity equity_margin;
  uint64_t current_rack_index;
  int num_threads;
  int print_interval;
  int thread_index;
  uint64_t *shared_rack_index;
  cpthread_mutex_t *shared_rack_index_lock;
  cpthread_t cpthread_id;
  // Rack containing just the unknown leave, which is
  // the tiles on the target's rack unseen to
  // the observer making the inference.
  Rack *current_target_leave;
  // Rack containing just the exchange, which is
  // the tiles the target put back into the bag which
  // are unseen to the observer making the inference.
  Rack *current_target_exchanged;
  // Rack containing the leave and the other tiles
  // which the observer may know about (for example, due
  // to a lost challenge, coffee housing, or accidental flash).
  Rack *current_target_rack;
  // The bag represented by a rack for convenience
  Rack *bag_as_rack;
  // MoveList used by the inference to generate moves
  MoveList *move_list;
  // Game used by the inference to generate moves
  Game *game;
  InferenceResults *results;
} Inference;

struct InferenceCtx {
  Game *game;
  int num_workers;
  Inference **worker_inferences;
  uint64_t shared_rack_index;
  cpthread_mutex_t shared_rack_index_lock;
  Stat **leave_stats;
  Stat **exchanged_stats;
  Stat **rack_stats;
};

void inference_destroy(Inference *inference) {
  if (!inference) {
    return;
  }
  rack_destroy(inference->current_target_leave);
  rack_destroy(inference->current_target_exchanged);
  rack_destroy(inference->bag_as_rack);
  move_list_destroy(inference->move_list);
  inference_results_destroy(inference->results);
  game_destroy(inference->game);
  free(inference);
}

uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      number_of_ways *=
          choose(rack_get_letter(bag_as_rack, i) + rack_get_letter(rack, i),
                 rack_get_letter(rack, i));
    }
  }
  return number_of_ways;
}

void increment_subtotals_for_results(const Rack *rack,
                                     InferenceResults *results,
                                     inference_stat_t inference_stat_type,
                                     uint64_t number_of_draws_for_leave) {
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      inference_results_add_to_letter_subtotal(
          results, inference_stat_type, i, rack_get_letter(rack, i),
          INFERENCE_SUBTOTAL_DRAW, number_of_draws_for_leave);
      inference_results_add_to_letter_subtotal(results, inference_stat_type, i,
                                               rack_get_letter(rack, i),
                                               INFERENCE_SUBTOTAL_LEAVE, 1);
    }
  }
}

void record_valid_leave(const Rack *rack, InferenceResults *results,
                        inference_stat_t inference_stat_type,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  stat_push(inference_results_get_equity_values(results, inference_stat_type),
            current_leave_value, number_of_draws_for_leave);
  increment_subtotals_for_results(rack, results, inference_stat_type,
                                  number_of_draws_for_leave);
}

void evaluate_possible_leave(Inference *inference) {
  Equity current_leave_value = 0;
  if (inference->target_number_of_tiles_exchanged == 0) {
    current_leave_value =
        klv_get_leave_value(inference->klv, inference->current_target_leave);
  }

  const Move *top_move = get_top_equity_move(
      inference->game, inference->thread_index, inference->move_list);
  const bool is_within_equity_margin = inference->target_score +
                                           current_leave_value +
                                           inference->equity_margin >=
                                       move_get_equity(top_move);
  const int tiles_played = move_get_tiles_played(top_move);
  const bool number_exchanged_matches =
      move_get_type(top_move) == GAME_EVENT_EXCHANGE &&
      tiles_played == inference->target_number_of_tiles_exchanged;
  const bool recordable = is_within_equity_margin || number_exchanged_matches ||
                          rack_is_empty(inference->bag_as_rack);
  if (recordable) {
    uint64_t number_of_draws_for_leave = get_number_of_draws_for_rack(
        inference->bag_as_rack, inference->current_target_leave);
    if (inference->target_number_of_tiles_exchanged > 0) {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_RACK,
                         equity_to_double(current_leave_value),
                         number_of_draws_for_leave);
      // The full rack for the exchange was recorded above,
      // but now we have to record the leave and the exchanged tiles
      for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
           exchanged_tile_index++) {
        MachineLetter tile_exchanged =
            move_get_tile(top_move, exchanged_tile_index);
        rack_add_letter(inference->current_target_exchanged, tile_exchanged);
        rack_take_letter(inference->current_target_leave, tile_exchanged);
      }
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_LEAVE,
                         equity_to_double(klv_get_leave_value(
                             inference->klv, inference->current_target_leave)),
                         number_of_draws_for_leave);
      record_valid_leave(
          inference->current_target_exchanged, inference->results,
          INFERENCE_TYPE_EXCHANGED,
          equity_to_double(klv_get_leave_value(
              inference->klv, inference->current_target_exchanged)),
          number_of_draws_for_leave);
      leave_rack_list_insert_rack(
          inference->current_target_leave, inference->current_target_exchanged,
          (int)number_of_draws_for_leave, current_leave_value,
          inference_results_get_leave_rack_list(inference->results));
      alias_method_add_rack(
          inference_results_get_alias_method(inference->results),
          inference->current_target_leave, (int)number_of_draws_for_leave);
      rack_reset(inference->current_target_exchanged);
      for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
           exchanged_tile_index++) {
        MachineLetter tile_exchanged =
            move_get_tile(top_move, exchanged_tile_index);
        rack_add_letter(inference->current_target_leave, tile_exchanged);
      }
    } else {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_LEAVE,
                         equity_to_double(current_leave_value),
                         number_of_draws_for_leave);
      alias_method_add_rack(
          inference_results_get_alias_method(inference->results),
          inference->current_target_leave, (int)number_of_draws_for_leave);
      leave_rack_list_insert_rack(
          inference->current_target_leave, NULL, (int)number_of_draws_for_leave,
          current_leave_value,
          inference_results_get_leave_rack_list(inference->results));
    }
  }
}

void increment_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_take_letter(inference->bag_as_rack, letter);
  rack_add_letter(inference->current_target_rack, letter);
  rack_add_letter(inference->current_target_leave, letter);
}

void decrement_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_add_letter(inference->bag_as_rack, letter);
  rack_take_letter(inference->current_target_rack, letter);
  rack_take_letter(inference->current_target_leave, letter);
}

void complete_inference_setup(Inference *inference, const InferenceArgs *args) {
  // This will return the inference->current_target_rack to the bag.
  return_rack_to_bag(inference->game, 0);
  return_rack_to_bag(inference->game, 1);

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, inference->ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  bool success = draw_rack_from_bag(inference->game, args->target_index,
                                    &temp_target_rack);
  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw combined (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb, ") inferred player played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and inferred player known rack (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  success = draw_rack_from_bag(inference->game, 1 - args->target_index,
                               args->nontarget_known_rack);

  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw nontarget player rack (");
    string_builder_add_rack(sb, args->nontarget_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  // Add the letters that are known to have been kept on the rack
  // for their target inferred play.
  rack_copy(inference->current_target_leave, args->target_known_rack);
  rack_subtract_using_floor_zero(inference->current_target_leave,
                                 args->target_played_tiles);

  // Set the bag_as_rack to the bag
  const Bag *bag = game_get_bag(inference->game);
  int bag_letters_array[MAX_ALPHABET_SIZE];
  memset(bag_letters_array, 0, sizeof(bag_letters_array));
  bag_increment_unseen_count(bag, bag_letters_array);
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    rack_add_letters(inference->bag_as_rack, i, bag_letters_array[i]);
  }
}

Inference *inference_create(const Game *game, int thread_index,
                            const InferenceArgs *args,
                            const InferenceResults *results) {
  Inference *inference = malloc_or_die(sizeof(Inference));
  inference->game = game_duplicate(game);
  inference->move_list = move_list_create(1);
  inference->klv =
      player_get_klv(game_get_player(inference->game, args->target_index));

  inference->ld_size = ld_get_size(game_get_ld(inference->game));
  inference->move_capacity = args->move_capacity;
  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  inference->current_rack_index = 0;

  inference->current_target_leave = rack_create(inference->ld_size);
  inference->current_target_exchanged = rack_create(inference->ld_size);
  inference->current_target_rack =
      player_get_rack(game_get_player(inference->game, args->target_index));
  inference->bag_as_rack = rack_create(inference->ld_size);

  inference->results =
      inference_results_create(inference_results_get_alias_method(results));
  inference_results_reset(inference->results, inference->move_capacity,
                          inference->ld_size);

  // Multithreading
  inference->num_threads = args->num_threads;
  inference->print_interval = args->print_interval;
  inference->thread_control = args->thread_control;
  inference->thread_index = thread_index;

  complete_inference_setup(inference, args);

  return inference;
}

void inference_reset(Inference *inference, const Game *game,
                     const InferenceArgs *args) {
  // Only update fields that will change between
  // inferences that happen in autoplay simming.
  game_copy(inference->game, game);

  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  inference->current_rack_index = 0;

  rack_reset(inference->current_target_leave);
  rack_reset(inference->current_target_exchanged);
  inference->current_target_rack = player_get_rack(
      game_get_player(inference->game, inference->target_index));
  rack_reset(inference->bag_as_rack);

  inference_results_reset(inference->results, inference->move_capacity,
                          inference->ld_size);

  complete_inference_setup(inference, args);
}

void add_inference_results(InferenceResults *inference_results_to_add,
                           InferenceResults *inference_results_to_update) {
  inference_results_add_subtotals(inference_results_to_add,
                                  inference_results_to_update);
  LeaveRackList *lrl_to_add =
      inference_results_get_leave_rack_list(inference_results_to_add);
  LeaveRackList *lrl_to_update =
      inference_results_get_leave_rack_list(inference_results_to_update);

  while (leave_rack_list_get_count(lrl_to_add) > 0) {
    const LeaveRack *leave_rack_to_add = leave_rack_list_pop_rack(lrl_to_add);
    leave_rack_list_insert_leave_rack(leave_rack_to_add, lrl_to_update);
  }
}

bool should_print_info(const Inference *inference) {
  return inference->print_interval > 0 && inference->current_rack_index > 0 &&
         inference->current_rack_index % inference->print_interval == 0;
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter) {
  if (thread_control_get_status(inference->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    return;
  }
  if (tiles_to_infer == 0) {
    bool perform_evaluation = false;
    bool print_info = false;

    cpthread_mutex_lock(inference->shared_rack_index_lock);
    if (inference->current_rack_index == *inference->shared_rack_index) {
      print_info = should_print_info(inference);
      perform_evaluation = true;
      *inference->shared_rack_index += 1;
    }
    cpthread_mutex_unlock(inference->shared_rack_index_lock);

    if (perform_evaluation) {
      evaluate_possible_leave(inference);
    }
    if (print_info) {
      print_ucgi_inference_current_rack(inference->current_rack_index,
                                        inference->thread_control);
    }
    inference->current_rack_index++;
    return;
  }
  for (int letter = start_letter; letter < inference->ld_size; letter++) {
    if (rack_get_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      iterate_through_all_possible_leaves(inference, tiles_to_infer - 1,
                                          letter);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

void *infer_worker(void *uncasted_inference) {
  Inference *inference = (Inference *)uncasted_inference;
  iterate_through_all_possible_leaves(
      inference,
      (RACK_SIZE)-rack_get_total_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER);
  return NULL;
}

void set_shared_variables_for_inference(
    Inference *inference, uint64_t *shared_rack_index,
    cpthread_mutex_t *shared_rack_index_lock) {
  inference->shared_rack_index = shared_rack_index;
  inference->shared_rack_index_lock = shared_rack_index_lock;
}

void infer_manager(InferenceCtx *ctx, InferenceResults *results) {
  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_create(&ctx->worker_inferences[thread_index]->cpthread_id,
                    infer_worker, ctx->worker_inferences[thread_index]);
  }

  const bool tiles_were_exchanged =
      ctx->worker_inferences[0]->target_number_of_tiles_exchanged > 0;

  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_join(ctx->worker_inferences[thread_index]->cpthread_id);
    InferenceResults *worker_results =
        ctx->worker_inferences[thread_index]->results;
    add_inference_results(worker_results, results);
    ctx->leave_stats[thread_index] = inference_results_get_equity_values(
        worker_results, INFERENCE_TYPE_LEAVE);
    if (tiles_were_exchanged) {
      ctx->exchanged_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_EXCHANGED);
      ctx->rack_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_RACK);
    }
  }

  stats_combine(
      ctx->leave_stats, ctx->num_workers,
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
  if (tiles_were_exchanged) {
    stats_combine(
        ctx->exchanged_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_EXCHANGED));
    stats_combine(
        ctx->rack_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_RACK));
  }
}

void verify_inference_args(const InferenceArgs *args, const Game *game_dup,
                           ErrorStack *error_stack) {
  const Bag *bag = game_get_bag(game_dup);
  int bag_letter_counts[MAX_ALPHABET_SIZE];
  memset(bag_letter_counts, 0, sizeof(bag_letter_counts));
  bag_increment_unseen_count(bag, bag_letter_counts);

  const int ld_size = ld_get_size(game_get_ld(game_dup));

  // Add the current player racks to the bag letter counts
  const Rack *player0_rack = player_get_rack(game_get_player(game_dup, 0));
  const Rack *player1_rack = player_get_rack(game_get_player(game_dup, 1));
  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] +=
        rack_get_letter(player0_rack, i) + rack_get_letter(player1_rack, i);
  }

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  if (rack_get_total_letters(&temp_target_rack) > (RACK_SIZE)) {
    const LetterDistribution *ld = game_get_ld(game_dup);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "inferred player rack (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb,
                              ") derived from the union of played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and known letters (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb,
                              ") is greater than the maximum rack size of ");
    string_builder_add_int(sb, RACK_SIZE);
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
                     string_builder_dump(sb, NULL));
    string_builder_destroy(sb);
    return;
  }

  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] -= rack_get_letter(&temp_target_rack, i);
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "inferred player played letters (");
      string_builder_add_rack(sb, &temp_target_rack, game_get_ld(game_dup),
                              false);
      string_builder_add_string(sb, ") not available in the bag");
      error_stack_push(error_stack,
                       ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG,
                       string_builder_dump(sb, NULL));
      string_builder_destroy(sb);
      return;
    }
  }

  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] -= rack_get_letter(args->nontarget_known_rack, i);
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "noninferred player rack letters (");
      string_builder_add_rack(sb, args->nontarget_known_rack,
                              game_get_ld(game_dup), false);
      string_builder_add_string(sb, ") not available in the bag");
      char *err_msg = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);
      log_fatal(err_msg);
      free(err_msg);
      return;
    }
  }

  const int num_played_letters =
      rack_get_total_letters(args->target_played_tiles);

  if (num_played_letters == 0 && args->target_num_exch == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_NO_TILES_PLAYED,
        string_duplicate("cannot infer when no tiles are played or exchanged"));
    return;
  }

  if (num_played_letters != 0 && args->target_num_exch != 0) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE,
                     string_duplicate("cannot infer when both a tile placement "
                                      "and exchange move are specified"));
    return;
  }

  if (args->target_num_exch != 0 && bag_get_letters(bag) < (RACK_SIZE) * 2) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED,
        get_formatted_string("cannot infer an exchange where there are fewer "
                             "than %d tiles in the bag",
                             (RACK_SIZE)));
    return;
  }

  if (args->target_num_exch != 0 && args->target_score != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO,
        string_duplicate("cannot infer an exchange with a nonzero score"));
    return;
  }

  if (num_played_letters > (RACK_SIZE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
        get_formatted_string("inferred player played more tiles (%d) "
                             "than can fit in a rack (%d)",
                             num_played_letters, RACK_SIZE));
    return;
  }
}

void populate_inference_args_with_game_history(InferenceArgs *args,
                                               Game *game_dup,
                                               ErrorStack *error_stack) {
  GameHistory *game_history = args->game_history;
  const int most_recent_move_event_index =
      game_history_get_most_recent_move_event_index(game_history);
  if (most_recent_move_event_index < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EMPTY_GAME_HISTORY,
        get_formatted_string(
            "cannot infer the previous play for an empty game history"));
    return;
  }
  const GameEvent *target_move_event =
      game_history_get_event(game_history, most_recent_move_event_index);
  const ValidatedMoves *last_move = game_event_get_vms(target_move_event);
  const Move *move = validated_moves_get_move(last_move, 0);
  const int move_tiles_length = move_get_tiles_length(move);
  rack_reset(args->target_played_tiles);
  for (int i = 0; i < move_tiles_length; i++) {
    if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(move_get_tile(move, i))) {
        rack_add_letter(args->target_played_tiles, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(args->target_played_tiles, move_get_tile(move, i));
      }
    }
  }
  args->target_index = game_event_get_player_index(target_move_event);
  args->target_score = game_event_get_move_score(target_move_event);
  args->target_num_exch = 0;
  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    args->target_num_exch = move_get_tiles_played(move);
    rack_reset(args->target_played_tiles);
  }
  rack_copy(args->nontarget_known_rack,
            player_get_rack(game_get_player(game_dup, 1 - args->target_index)));
  if (rack_is_empty(args->target_known_rack)) {
    for (int i = most_recent_move_event_index - 1; i >= 0; i--) {
      GameEvent *event = game_history_get_event(game_history, i);
      if (game_event_get_player_index(event) == args->target_index) {
        rack_copy(args->target_known_rack,
                  game_event_get_after_event_player_off_turn_rack(event));
        break;
      }
    }
  }

  // This will play all of the events right up to but not including the target
  // move event
  game_play_n_events(game_history, game_dup, most_recent_move_event_index,
                     false, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

// ***************************
// Inference Context functions
// ***************************

InferenceCtx *inference_ctx_create(int num_threads) {
  InferenceCtx *ctx = calloc_or_die(1, sizeof(InferenceCtx));
  ctx->num_workers = num_threads;
  ctx->worker_inferences = calloc_or_die(ctx->num_workers, sizeof(Inference *));
  return ctx;
}

void inference_ctx_set_game(InferenceCtx *ctx, const Game *game) {
  if (ctx->game) {
    game_copy(ctx->game, game);
  } else {
    ctx->game = game_duplicate(game);
  }
}

// Assumes that ctx->game is set
void inference_ctx_set_inferences(InferenceCtx *ctx, const InferenceArgs *args,
                                  InferenceResults *results) {
  ctx->shared_rack_index = 0;
  inference_results_reset(results, args->move_capacity,
                          ld_get_size(game_get_ld(args->game)));
  if (ctx->worker_inferences[0]) {
    for (int i = 0; i < ctx->num_workers; i++) {
      inference_reset(ctx->worker_inferences[i], ctx->game, args);
    }
  } else {
    cpthread_mutex_init(&ctx->shared_rack_index_lock);
    for (int i = 0; i < ctx->num_workers; i++) {
      ctx->worker_inferences[i] = inference_create(ctx->game, i, args, results);
      set_shared_variables_for_inference(ctx->worker_inferences[i],
                                         &ctx->shared_rack_index,
                                         &ctx->shared_rack_index_lock);
    }
    ctx->leave_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->exchanged_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->rack_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
  }
}

void inference_ctx_destroy(InferenceCtx *ctx) {
  if (!ctx) {
    return;
  }
  game_destroy(ctx->game);
  for (int i = 0; i < ctx->num_workers; i++) {
    inference_destroy(ctx->worker_inferences[i]);
  }
  free(ctx->worker_inferences);
  free(ctx->leave_stats);
  free(ctx->exchanged_stats);
  free(ctx->rack_stats);
  free(ctx);
}

void infer_with_initialized_ctx(InferenceArgs *args, InferenceCtx *ctx,
                                InferenceResults *results,
                                ErrorStack *error_stack) {
  inference_ctx_set_game(ctx, args->game);

  if (args->use_game_history) {
    populate_inference_args_with_game_history(args, ctx->game, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  verify_inference_args(args, ctx->game, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  inference_ctx_set_inferences(ctx, args, results);

  infer_manager(ctx, results);

  const Inference *wi = ctx->worker_inferences[0];
  inference_results_finalize(
      args->target_played_tiles, wi->current_target_leave, wi->bag_as_rack,
      results, wi->target_score, wi->target_number_of_tiles_exchanged,
      wi->equity_margin,
      thread_control_get_status(args->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT);
}

// Creates a new context if *ctx is NULL
// The caller is responsible for destroying the context
void infer(InferenceArgs *args, InferenceCtx **ctx, InferenceResults *results,
           ErrorStack *error_stack) {
  // The **ctx is a pointer to a pointer and must never be null;
  assert(ctx);
  if (*ctx == NULL) {
    *ctx = inference_ctx_create(args->num_threads);
  }
  infer_with_initialized_ctx(args, *ctx, results, error_stack);
}

void infer_without_ctx(InferenceArgs *args, InferenceResults *results,
                       ErrorStack *error_stack) {
  InferenceCtx *ctx = NULL;
  infer(args, &ctx, results, error_stack);
  inference_ctx_destroy(ctx);
}
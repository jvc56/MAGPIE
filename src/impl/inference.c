#include "inference.h"

#include "../compat/cpthread.h"
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
#include "gcg.h"
#include "move_gen.h"
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
  uint64_t total_racks_evaluated;
  int thread_index;
  uint64_t *shared_rack_index;
  cpthread_mutex_t *shared_rack_index_lock;
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

void inference_destroy(Inference *inference) {
  if (!inference) {
    return;
  }
  rack_destroy(inference->current_target_leave);
  rack_destroy(inference->current_target_exchanged);
  rack_destroy(inference->bag_as_rack);
  move_list_destroy(inference->move_list);
  free(inference);
}

void inference_copy_destroy(Inference *inference) {
  game_destroy(inference->game);
  inference_results_destroy(inference->results);
  inference_destroy(inference);
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

Inference *
inference_create(const Rack *target_played_tiles, const Rack *target_known_rack,
                 Game *game, int move_capacity, int target_index,
                 Equity target_score, int target_number_of_tiles_exchanged,
                 Equity equity_margin, const Rack *nontarget_known_rack,
                 InferenceResults *results) {
  Inference *inference = malloc_or_die(sizeof(Inference));
  inference->game = game;
  inference->klv = player_get_klv(game_get_player(game, target_index));
  inference->ld_size = rack_get_dist_size(target_played_tiles);
  inference->move_capacity = move_capacity;
  inference->target_index = target_index;
  inference->target_score = target_score;
  inference->target_number_of_tiles_exchanged =
      target_number_of_tiles_exchanged;
  inference->equity_margin = equity_margin;
  inference->current_rack_index = 0;
  inference->total_racks_evaluated = 0;
  // thread index is only meaningful for
  // duplicated inferences used in multithreading
  inference->thread_index = -1;
  // move_list is only needed for duplicated inferences
  inference->move_list = NULL;

  // shared rack index and shared rack index lock
  // are shared pointers between threads and are
  // set elsewhere.

  inference->current_target_leave = rack_create(inference->ld_size);
  inference->current_target_exchanged = rack_create(inference->ld_size);
  inference->current_target_rack =
      player_get_rack(game_get_player(game, target_index));
  inference->bag_as_rack = rack_create(inference->ld_size);

  inference_results_reset(results, inference->move_capacity,
                          inference->ld_size);

  inference->results = results;

  // This will return the inference->current_target_rack to the bag.
  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, inference->ld_size);

  rack_union(&temp_target_rack, target_played_tiles);
  rack_union(&temp_target_rack, target_known_rack);

  bool success = draw_rack_from_bag(game, target_index, &temp_target_rack);
  if (!success) {
    const LetterDistribution *ld = game_get_ld(game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw combined (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb, ") inferred player played letters (");
    string_builder_add_rack(sb, target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and inferred player known rack (");
    string_builder_add_rack(sb, target_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  success = draw_rack_from_bag(game, 1 - target_index, nontarget_known_rack);

  if (!success) {
    const LetterDistribution *ld = game_get_ld(game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw nontarget player rack (");
    string_builder_add_rack(sb, nontarget_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  // Add the letters that are known to have been kept on the rack
  // for their target inferred play.
  rack_copy(inference->current_target_leave, target_known_rack);
  rack_subtract_using_floor_zero(inference->current_target_leave,
                                 target_played_tiles);

  // Set the bag_as_rack to the bag
  const Bag *bag = game_get_bag(game);
  int bag_letters_array[MAX_ALPHABET_SIZE];
  memset(bag_letters_array, 0, sizeof(bag_letters_array));
  bag_increment_unseen_count(bag, bag_letters_array);
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    rack_add_letters(inference->bag_as_rack, i, bag_letters_array[i]);
  }

  return inference;
}

Inference *inference_duplicate(const Inference *inference, int thread_index,
                               ThreadControl *thread_control) {
  Inference *new_inference = malloc_or_die(sizeof(Inference));
  new_inference->game = game_duplicate(inference->game);
  new_inference->move_list = move_list_create(1);
  new_inference->klv = player_get_klv(
      game_get_player(new_inference->game, inference->target_index));

  new_inference->ld_size = inference->ld_size;
  new_inference->move_capacity = inference->move_capacity;
  new_inference->target_index = inference->target_index;
  new_inference->target_score = inference->target_score;
  new_inference->target_number_of_tiles_exchanged =
      inference->target_number_of_tiles_exchanged;
  new_inference->equity_margin = inference->equity_margin;
  new_inference->current_rack_index = inference->current_rack_index;
  new_inference->total_racks_evaluated = inference->total_racks_evaluated;

  new_inference->current_target_leave =
      rack_duplicate(inference->current_target_leave);
  new_inference->current_target_exchanged =
      rack_duplicate(inference->current_target_exchanged);
  new_inference->current_target_rack = player_get_rack(
      game_get_player(new_inference->game, new_inference->target_index));
  new_inference->bag_as_rack = rack_duplicate(inference->bag_as_rack);

  new_inference->results = inference_results_create(
      inference_results_get_alias_method(inference->results));
  inference_results_reset(new_inference->results, inference->move_capacity,
                          new_inference->ld_size);

  // Multithreading
  new_inference->thread_control = thread_control;
  new_inference->thread_index = thread_index;

  return new_inference;
}

void add_inference(Inference *inference_to_add,
                   Inference *inference_to_update) {
  inference_results_add_subtotals(inference_to_add->results,
                                  inference_to_update->results);
  LeaveRackList *lrl_to_add =
      inference_results_get_leave_rack_list(inference_to_add->results);
  LeaveRackList *lrl_to_update =
      inference_results_get_leave_rack_list(inference_to_update->results);

  while (leave_rack_list_get_count(lrl_to_add) > 0) {
    const LeaveRack *leave_rack_to_add = leave_rack_list_pop_rack(lrl_to_add);
    leave_rack_list_insert_leave_rack(leave_rack_to_add, lrl_to_update);
  }
}

void get_total_racks_evaluated(Inference *inference, int tiles_to_infer,
                               int start_letter,
                               uint64_t *total_racks_evaluated) {
  if (tiles_to_infer == 0) {
    *total_racks_evaluated += 1;
    return;
  }
  for (int letter = start_letter; letter < inference->ld_size; letter++) {
    if (rack_get_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      get_total_racks_evaluated(inference, tiles_to_infer - 1, letter,
                                total_racks_evaluated);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

bool should_print_info(const Inference *inference) {
  int print_info_interval =
      thread_control_get_print_info_interval(inference->thread_control);
  return print_info_interval > 0 && inference->current_rack_index > 0 &&
         inference->current_rack_index % print_info_interval == 0;
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter) {
  if (thread_control_is_winding_down(inference->thread_control)) {
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

void infer_manager(ThreadControl *thread_control, Inference *inference,
                   bool update_thread_control_status) {
  uint64_t total_racks_evaluated = 0;

  get_total_racks_evaluated(
      inference,
      (RACK_SIZE)-rack_get_total_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER, &total_racks_evaluated);
  inference->total_racks_evaluated = total_racks_evaluated;

  print_ucgi_inference_total_racks_evaluated(total_racks_evaluated,
                                             thread_control);

  int number_of_threads = thread_control_get_threads(thread_control);

  uint64_t shared_rack_index = 0;
  cpthread_mutex_t shared_rack_index_lock;

  cpthread_mutex_init(&shared_rack_index_lock);

  Inference **inferences_for_workers =
      malloc_or_die((sizeof(Inference *)) * (number_of_threads));
  cpthread_t *worker_ids =
      malloc_or_die((sizeof(cpthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    inferences_for_workers[thread_index] =
        inference_duplicate(inference, thread_index, thread_control);
    set_shared_variables_for_inference(inferences_for_workers[thread_index],
                                       &shared_rack_index,
                                       &shared_rack_index_lock);
    cpthread_create(&worker_ids[thread_index], infer_worker,
                    inferences_for_workers[thread_index]);
  }

  Stat **leave_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));

  Stat **exchanged_stats = NULL;
  Stat **rack_stats = NULL;

  const bool tiles_were_exchanged =
      inference->target_number_of_tiles_exchanged > 0;

  if (tiles_were_exchanged) {
    exchanged_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
    rack_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    cpthread_join(worker_ids[thread_index]);
    Inference *inference_worker = inferences_for_workers[thread_index];
    add_inference(inference_worker, inference);
    leave_stats[thread_index] = inference_results_get_equity_values(
        inference_worker->results, INFERENCE_TYPE_LEAVE);
    if (tiles_were_exchanged) {
      exchanged_stats[thread_index] = inference_results_get_equity_values(
          inference_worker->results, INFERENCE_TYPE_EXCHANGED);
      rack_stats[thread_index] = inference_results_get_equity_values(
          inference_worker->results, INFERENCE_TYPE_RACK);
    }
  }

  // Infer was able to finish normally, which is when it
  // iterates through every rack
  if (update_thread_control_status) {
    thread_control_set_status(thread_control,
                              THREAD_CONTROL_STATUS_MAX_ITERATIONS);
  }

  stats_combine(leave_stats, number_of_threads,
                inference_results_get_equity_values(inference->results,
                                                    INFERENCE_TYPE_LEAVE));
  free(leave_stats);
  if (tiles_were_exchanged) {
    stats_combine(exchanged_stats, number_of_threads,
                  inference_results_get_equity_values(
                      inference->results, INFERENCE_TYPE_EXCHANGED));
    stats_combine(rack_stats, number_of_threads,
                  inference_results_get_equity_values(inference->results,
                                                      INFERENCE_TYPE_RACK));
    free(exchanged_stats);
    free(rack_stats);
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    inference_copy_destroy(inferences_for_workers[thread_index]);
  }

  free(inferences_for_workers);
  free(worker_ids);
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
  GameEvent *target_move_event =
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
            game_event_get_after_event_player_on_turn_rack(target_move_event));

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
                     error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

void infer_with_game_duplicate(InferenceArgs *args, Game *game_dup,
                               InferenceResults *results,
                               ErrorStack *error_stack) {
  if (args->use_game_history) {
    populate_inference_args_with_game_history(args, game_dup, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  verify_inference_args(args, game_dup, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  Inference *inference = inference_create(
      args->target_played_tiles, args->target_known_rack, game_dup,
      args->move_capacity, args->target_index, args->target_score,
      args->target_num_exch, args->equity_margin, args->nontarget_known_rack,
      results);

  infer_manager(args->thread_control, inference,
                args->update_thread_control_status);

  inference_results_finalize(
      args->target_played_tiles, inference->current_target_leave,
      inference->bag_as_rack, inference->results, inference->target_score,
      inference->target_number_of_tiles_exchanged, inference->equity_margin);

  if (thread_control_get_status(args->thread_control) ==
      THREAD_CONTROL_STATUS_MAX_ITERATIONS) {
    // Only print if infer was able to finish normally and the thread control
    // status was updated. If thread_control_set_status status isn't max
    // iterations, it was interrupted by the user or it is part of a simulation
    // and the results should not be printed.
    print_ucgi_inference(game_get_ld(inference->game), inference->results,
                         args->thread_control);
  }

  inference_destroy(inference);
}

void infer(InferenceArgs *args, InferenceResults *results,
           ErrorStack *error_stack) {
  // Overwrite the pass in game with a duplicate that we can modify
  Game *game = game_duplicate(args->game);
  infer_with_game_duplicate(args, game, results, error_stack);
  game_destroy(game);
  // FIXME: only destroy cache if this is a stand-alone infer request, keep the
  // caches for simming with inference
  gen_destroy_cache();
}

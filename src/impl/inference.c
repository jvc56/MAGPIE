#include "inference.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/leave_rack.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"

#include "gameplay.h"
#include "move_gen.h"

#include "../str/inference_string.h"

#include "../util/log.h"
#include "../util/util.h"

typedef struct Inference {
  // The following fields are owned by the caller

  // Game used by the inference to generate moves
  Game *game;
  // KLV used to evaluate leaves to determine
  // which moves are top equity. This should be
  // the KLV of the target.
  const KLV *klv;

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
  pthread_mutex_t *shared_rack_index_lock;
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
  ThreadControl *thread_control;
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
  const bool is_within_equity_margin = inference->target_score + current_leave_value +
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
        uint8_t tile_exchanged = move_get_tile(top_move, exchanged_tile_index);
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
          number_of_draws_for_leave, current_leave_value,
          inference_results_get_leave_rack_list(inference->results));
      rack_reset(inference->current_target_exchanged);
      for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = move_get_tile(top_move, exchanged_tile_index);
        rack_add_letter(inference->current_target_leave, tile_exchanged);
      }
    } else {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_LEAVE, equity_to_double(current_leave_value),
                         number_of_draws_for_leave);
      leave_rack_list_insert_rack(
          inference->current_target_leave, NULL, number_of_draws_for_leave,
          current_leave_value,
          inference_results_get_leave_rack_list(inference->results));
    }
  }
}

void increment_letter_for_inference(Inference *inference, uint8_t letter) {
  rack_take_letter(inference->bag_as_rack, letter);
  rack_add_letter(inference->current_target_rack, letter);
  rack_add_letter(inference->current_target_leave, letter);
}

void decrement_letter_for_inference(Inference *inference, uint8_t letter) {
  rack_add_letter(inference->bag_as_rack, letter);
  rack_take_letter(inference->current_target_rack, letter);
  rack_take_letter(inference->current_target_leave, letter);
}

Inference *inference_create(const Rack *target_played_tiles, Game *game,
                            int move_capacity, int target_index,
                            int target_score,
                            int target_number_of_tiles_exchanged,
                            double equity_margin, InferenceResults *results) {

  Inference *inference = malloc_or_die(sizeof(Inference));

  inference->game = game;
  inference->klv = player_get_klv(game_get_player(game, target_index));

  inference->ld_size = rack_get_dist_size(target_played_tiles);
  inference->move_capacity = move_capacity;
  inference->target_index = target_index;
  inference->target_score = int_to_equity(target_score);
  inference->target_number_of_tiles_exchanged =
      target_number_of_tiles_exchanged;
  inference->equity_margin = double_to_equity(equity_margin);
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

  // Set the current target rack with the known unplayed tiles
  // of the target

  const Bag *bag = game_get_bag(game);

  for (int i = 0; i < inference->ld_size; i++) {
    // Add any existing tiles on the target's rack
    // to the target's leave for partial inferences
    for (int j = 0; j < rack_get_letter(inference->current_target_rack, i);
         j++) {
      rack_add_letter(inference->current_target_leave, i);
    }

    for (int j = 0; j < bag_get_letter(bag, i); j++) {
      rack_add_letter(inference->bag_as_rack, i);
    }
  }

  // Remove the tiles played in the move from the game bag
  // and add them to the target's rack
  for (int i = 0; i < inference->ld_size; i++) {
    for (int j = 0; j < rack_get_letter(target_played_tiles, i); j++) {
      rack_take_letter(inference->bag_as_rack, i);
      rack_add_letter(inference->current_target_rack, i);
    }
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

  new_inference->results = inference_results_create();
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
    LeaveRack *leave_rack_to_add = leave_rack_list_pop_rack(lrl_to_add);
    leave_rack_list_insert_rack(leave_rack_get_leave(leave_rack_to_add),
                                leave_rack_get_exchanged(leave_rack_to_add),
                                leave_rack_get_draws(leave_rack_to_add),
                                leave_rack_get_equity(leave_rack_to_add),
                                lrl_to_update);
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
  if (thread_control_get_is_halted(inference->thread_control)) {
    return;
  }
  if (tiles_to_infer == 0) {
    bool perform_evaluation = false;
    bool print_info = false;

    pthread_mutex_lock(inference->shared_rack_index_lock);
    if (inference->current_rack_index == *inference->shared_rack_index) {
      print_info = should_print_info(inference);
      perform_evaluation = true;
      *inference->shared_rack_index += 1;
    }
    pthread_mutex_unlock(inference->shared_rack_index_lock);

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
    pthread_mutex_t *shared_rack_index_lock) {
  inference->shared_rack_index = shared_rack_index;
  inference->shared_rack_index_lock = shared_rack_index_lock;
}

void infer_manager(ThreadControl *thread_control, Inference *inference) {
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
  pthread_mutex_t shared_rack_index_lock;

  if (pthread_mutex_init(&shared_rack_index_lock, NULL) != 0) {
    log_fatal("mutex init failed for inference\n");
  }

  Inference **inferences_for_workers =
      malloc_or_die((sizeof(Inference *)) * (number_of_threads));
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    inferences_for_workers[thread_index] =
        inference_duplicate(inference, thread_index, thread_control);
    set_shared_variables_for_inference(inferences_for_workers[thread_index],
                                       &shared_rack_index,
                                       &shared_rack_index_lock);
    pthread_create(&worker_ids[thread_index], NULL, infer_worker,
                   inferences_for_workers[thread_index]);
  }

  Stat **leave_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));

  Stat **exchanged_stats = NULL;
  Stat **rack_stats = NULL;

  if (inference->target_number_of_tiles_exchanged > 0) {
    exchanged_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
    rack_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    Inference *inference_worker = inferences_for_workers[thread_index];
    add_inference(inference_worker, inference);
    leave_stats[thread_index] = inference_results_get_equity_values(
        inference_worker->results, INFERENCE_TYPE_LEAVE);
    if (inference->target_number_of_tiles_exchanged > 0) {
      exchanged_stats[thread_index] = inference_results_get_equity_values(
          inference_worker->results, INFERENCE_TYPE_EXCHANGED);
      rack_stats[thread_index] = inference_results_get_equity_values(
          inference_worker->results, INFERENCE_TYPE_RACK);
    }
  }

  // Infer was able to finish normally, which is when it
  // iterates through every rack
  thread_control_halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  stats_combine(leave_stats, number_of_threads,
                inference_results_get_equity_values(inference->results,
                                                    INFERENCE_TYPE_LEAVE));
  free(leave_stats);
  if (inference->target_number_of_tiles_exchanged > 0) {
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

inference_status_t verify_inference(const Inference *inference) {
  Rack *bag_as_rack = inference->bag_as_rack;
  for (int i = 0; i < inference->ld_size; i++) {
    if (rack_get_letter(bag_as_rack, i) < 0) {
      return INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
    }
  }

  int infer_rack_number_of_letters =
      rack_get_total_letters(inference->current_target_rack);

  int number_of_letters_in_bag = rack_get_total_letters(bag_as_rack);

  if (infer_rack_number_of_letters == 0 &&
      inference->target_number_of_tiles_exchanged == 0) {
    return INFERENCE_STATUS_NO_TILES_PLAYED;
  }

  if (infer_rack_number_of_letters != 0 &&
      inference->target_number_of_tiles_exchanged != 0) {
    return INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE;
  }

  if (inference->target_number_of_tiles_exchanged != 0 &&
      number_of_letters_in_bag < (RACK_SIZE) * 2) {
    return INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED;
  }

  if (inference->target_number_of_tiles_exchanged != 0 &&
      inference->target_score != 0) {
    return INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO;
  }

  if (infer_rack_number_of_letters > (RACK_SIZE)) {
    return INFERENCE_STATUS_RACK_OVERFLOW;
  }

  return INFERENCE_STATUS_SUCCESS;
}

inference_status_t infer(InferenceArgs *args, InferenceResults *results) {
  thread_control_reset(args->thread_control, 0);

  if (!args->target_played_tiles) {
    return INFERENCE_STATUS_NO_TILES_PLAYED;
  }

  Game *game = game_duplicate(args->game);

  Inference *inference = inference_create(
      args->target_played_tiles, game, args->move_capacity, args->target_index,
      args->target_score, args->target_num_exch, args->equity_margin, results);

  inference_status_t status = verify_inference(inference);

  if (status == INFERENCE_STATUS_SUCCESS) {
    infer_manager(args->thread_control, inference);

    inference_results_finalize(
        args->target_played_tiles, inference->current_target_leave,
        inference->bag_as_rack, inference->results, inference->target_score,
        inference->target_number_of_tiles_exchanged, inference->equity_margin);

    if (thread_control_get_halt_status(args->thread_control) ==
        HALT_STATUS_MAX_ITERATIONS) {
      // Only print if infer was able to finish normally.
      // If thread_control_halt status isn't max iterations, it was interrupted
      // by the user and the results will not be valid.
      print_ucgi_inference(game_get_ld(inference->game), inference->results,
                           args->thread_control);
    }

    // Return the player to infer rack to it's original
    // state since the inference does not own that struct
    for (int i = 0; i < rack_get_dist_size(args->target_played_tiles); i++) {
      for (int j = 0; j < rack_get_letter(args->target_played_tiles, i); j++) {
        rack_take_letter(inference->current_target_rack, i);
      }
    }
  }

  game_destroy(game);
  inference_destroy(inference);
  gen_destroy_cache();

  return status;
}

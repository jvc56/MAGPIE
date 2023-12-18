#include <pthread.h>
#include <stdint.h>

#include "../def/inference_defs.h"
#include "../def/rack_defs.h"

#include "../util/util.h"

#include "../str/inference_string.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/leave_rack.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/stats.h"

#include "inference.h"
#include "move_gen.h"

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
  // Target player index in the game
  int target_index;
  int target_score;
  int target_number_of_tiles_exchanged;
  double equity_margin;
  uint64_t current_rack_index;
  uint64_t total_racks_evaluated;
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
  // MoveGen used by the inference to generate moves
  MoveGen *gen;
  ThreadControl *thread_control;
  InferenceResults *results;
} Inference;

void destroy_inference(Inference *inference) {
  destroy_rack(inference->current_target_leave);
  destroy_rack(inference->current_target_exchanged);
  destroy_rack(inference->bag_as_rack);
  destroy_generator(inference->gen);
  free(inference);
}

void destroy_inference_copy(Inference *inference) {
  destroy_game(inference->game);
  inference_results_destroy(inference->results);
  destroy_inference(inference);
}

uint64_t choose(uint64_t n, uint64_t k) {
  if (n < k) {
    return 0;
  }
  if (k == 0) {
    return 1;
  }
  return (n * choose(n - 1, k - 1)) / k;
}

uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < get_array_size(rack); i++) {
    if (get_number_of_letter(rack, i) > 0) {
      number_of_ways *= choose(get_number_of_letter(bag_as_rack, i) +
                                   get_number_of_letter(rack, i),
                               get_number_of_letter(rack, i));
    }
  }
  return number_of_ways;
}

double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *target_rack, uint8_t this_letter,
    int minimum, int number_of_target_played_tiles) {
  int number_of_this_letters_already_on_rack =
      get_number_of_letter(target_rack, this_letter);
  int minimum_adjusted_for_partial_rack =
      minimum - number_of_this_letters_already_on_rack;
  // If the partial leave already has the minimum
  // number of letters, the probability is trivially 1.
  if (minimum_adjusted_for_partial_rack <= 0) {
    return 1;
  }
  int total_number_of_letters_in_bag = get_number_of_letters(bag_as_rack);
  int total_number_of_letters_on_rack = get_number_of_letters(target_rack);
  int number_of_this_letter_in_bag =
      get_number_of_letter(bag_as_rack, this_letter);

  // If there are not enough letters to meet the minimum, the probability
  // is trivially 0.
  if (number_of_this_letter_in_bag < minimum_adjusted_for_partial_rack) {
    return 0;
  }

  int total_number_of_letters_to_draw =
      (RACK_SIZE) -
      (total_number_of_letters_on_rack + number_of_target_played_tiles);

  // If the player is emptying the bag and there are the minimum
  // number of leaves remaining, the probability is trivially 1.
  if (total_number_of_letters_in_bag <= total_number_of_letters_to_draw &&
      number_of_this_letter_in_bag >= minimum_adjusted_for_partial_rack) {
    return 1;
  }

  uint64_t total_draws =
      choose(total_number_of_letters_in_bag, total_number_of_letters_to_draw);
  if (total_draws == 0) {
    return 0;
  }
  int number_of_other_letters_in_bag =
      total_number_of_letters_in_bag - number_of_this_letter_in_bag;
  uint64_t total_draws_for_this_letter_minimum = 0;
  for (int i = minimum_adjusted_for_partial_rack;
       i <= total_number_of_letters_to_draw; i++) {
    total_draws_for_this_letter_minimum +=
        choose(number_of_this_letter_in_bag, i) *
        choose(number_of_other_letters_in_bag,
               total_number_of_letters_to_draw - i);
  }

  return ((double)total_draws_for_this_letter_minimum) / total_draws;
}

void increment_subtotals_for_results(const Rack *rack,
                                     InferenceResults *results,
                                     inference_stat_t inference_stat_type,
                                     uint64_t number_of_draws_for_leave) {
  for (int i = 0; i < get_array_size(rack); i++) {
    if (get_number_of_letter(rack, i) > 0) {
      add_to_letter_subtotal(
          results, inference_stat_type, i, get_number_of_letter(rack, i),
          INFERENCE_SUBTOTAL_DRAW, number_of_draws_for_leave);
      add_to_letter_subtotal(results, inference_stat_type, i,
                             get_number_of_letter(rack, i),
                             INFERENCE_SUBTOTAL_LEAVE, 1);
    }
  }
}

void record_valid_leave(const Rack *rack, InferenceResults *results,
                        inference_stat_t inference_stat_type,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  push(inference_results_get_equity_values(results, inference_stat_type),
       (double)current_leave_value, number_of_draws_for_leave);
  increment_subtotals_for_results(rack, results, inference_stat_type,
                                  number_of_draws_for_leave);
}

// FIXME: this should use the gameplay function
// once this is moved to impl
void inference_generate_moves_for_game(Game *game, MoveGen *gen) {
  int player_on_turn_index = game_get_player_on_turn_index(game);
  Player *player_on_turn = game_get_player(game, player_on_turn_index);
  Player *opponent = game_get_player(game, 1 - player_on_turn_index);
  generate_moves(game_get_ld(game), player_get_kwg(player_on_turn),
                 player_get_klv(player_on_turn), player_get_rack(opponent), gen,
                 game_get_board(game), player_get_rack(player_on_turn),
                 player_on_turn_index, get_tiles_remaining(game_get_bag(game)),
                 MOVE_RECORD_BEST, MOVE_SORT_EQUITY,
                 game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG));
}

// FIXME: this should use the gameplay function
// once this is moved to impl
Move *get_top_move(Inference *inference) {
  inference_generate_moves_for_game(inference->game, inference->gen);
  return move_list_get_move(gen_get_move_list(inference->gen), 0);
}

void evaluate_possible_leave(Inference *inference) {
  double current_leave_value = 0;
  if (inference->target_number_of_tiles_exchanged == 0) {
    current_leave_value =
        klv_get_leave_value(inference->klv, inference->current_target_leave);
  }
  const Move *top_move = get_top_move(inference);
  bool is_within_equity_margin = inference->target_score + current_leave_value +
                                     inference->equity_margin +
                                     (INFERENCE_EQUITY_EPSILON) >=
                                 get_equity(top_move);
  int tiles_played = move_get_tiles_played(top_move);
  bool number_exchanged_matches =
      get_move_type(top_move) == GAME_EVENT_EXCHANGE &&
      tiles_played == inference->target_number_of_tiles_exchanged;
  bool recordable = is_within_equity_margin || number_exchanged_matches ||
                    rack_is_empty(inference->bag_as_rack);
  if (recordable) {
    uint64_t number_of_draws_for_leave = get_number_of_draws_for_rack(
        inference->bag_as_rack, inference->current_target_leave);
    if (inference->target_number_of_tiles_exchanged > 0) {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_RACK, current_leave_value,
                         number_of_draws_for_leave);
      // The full rack for the exchange was recorded above,
      // but now we have to record the leave and the exchanged tiles
      for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = get_tile(top_move, exchanged_tile_index);
        add_letter_to_rack(inference->current_target_exchanged, tile_exchanged);
        take_letter_from_rack(inference->current_target_leave, tile_exchanged);
      }
      record_valid_leave(
          inference->current_target_leave, inference->results,
          INFERENCE_TYPE_LEAVE,
          klv_get_leave_value(inference->klv, inference->current_target_leave),
          number_of_draws_for_leave);
      record_valid_leave(
          inference->current_target_exchanged, inference->results,
          INFERENCE_TYPE_EXCHANGED,
          klv_get_leave_value(inference->klv,
                              inference->current_target_exchanged),
          number_of_draws_for_leave);
      insert_leave_rack(
          inference->current_target_leave, inference->current_target_exchanged,
          inference_results_get_leave_rack_list(inference->results),
          number_of_draws_for_leave, current_leave_value);
      reset_rack(inference->current_target_exchanged);
      for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = get_tile(top_move, exchanged_tile_index);
        add_letter_to_rack(inference->current_target_leave, tile_exchanged);
      }
    } else {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_LEAVE, current_leave_value,
                         number_of_draws_for_leave);
      insert_leave_rack(
          inference->current_target_leave, NULL,
          inference_results_get_leave_rack_list(inference->results),
          number_of_draws_for_leave, current_leave_value);
    }
  }
}

void increment_letter_for_inference(Inference *inference, uint8_t letter) {
  take_letter_from_rack(inference->bag_as_rack, letter);
  add_letter_to_rack(inference->current_target_rack, letter);
  add_letter_to_rack(inference->current_target_leave, letter);
}

void decrement_letter_for_inference(Inference *inference, uint8_t letter) {
  add_letter_to_rack(inference->bag_as_rack, letter);
  take_letter_from_rack(inference->current_target_rack, letter);
  take_letter_from_rack(inference->current_target_leave, letter);
}

Inference *inference_create(InferenceResults **results, Game *game,
                            Rack *target_played_tiles, int move_capacity,
                            int target_index, int target_score,
                            int target_number_of_tiles_exchanged,
                            double equity_margin) {

  Inference *inference = malloc_or_die(sizeof(Inference));

  inference->game = game;
  inference->klv = player_get_klv(game_get_player(game, target_index));

  inference->ld_size = get_array_size(target_played_tiles);
  inference->target_index = target_index;
  inference->target_score = target_score;
  inference->target_number_of_tiles_exchanged =
      target_number_of_tiles_exchanged;
  inference->equity_margin = equity_margin;
  inference->current_rack_index = 0;
  inference->total_racks_evaluated = 0;

  // shared rack index and shared rack index lock
  // are shared pointers between threads and are
  // set elsewhere.

  inference->current_target_leave = create_rack(inference->ld_size);
  inference->current_target_exchanged = create_rack(inference->ld_size);
  inference->current_target_rack =
      player_get_rack(game_get_player(game, target_index));
  inference->bag_as_rack = create_rack(inference->ld_size);
  inference->gen = create_generator(1, inference->ld_size);

  // Recreate the results since the ld_size may have changed
  // and also to reset the info.
  if (*results) {
    inference_results_destroy(*results);
  }

  *results = inference_results_create(move_capacity, inference->ld_size);

  inference->results = *results;

  // Set the initial bag
  add_bag_to_rack(game_get_bag(game), inference->bag_as_rack);

  // Set the current target rack with the known unplayed tiles
  // of the target

  // Add any existing tiles on the target's rack
  // to the target's leave for partial inferences
  for (int i = 0; i < inference->ld_size; i++) {
    for (int j = 0; j < get_number_of_letter(inference->current_target_rack, i);
         j++) {
      add_letter_to_rack(inference->current_target_leave, i);
    }
  }

  // Remove the tiles played in the move from the game bag
  // and add them to the target's rack
  for (int i = 0; i < inference->ld_size; i++) {
    for (int j = 0; j < get_number_of_letter(target_played_tiles, i); j++) {
      take_letter_from_rack(inference->bag_as_rack, i);
      add_letter_to_rack(inference->current_target_rack, i);
    }
  }

  return inference;
}

Inference *inference_duplicate(const Inference *inference,
                               ThreadControl *thread_control) {
  Inference *new_inference = malloc_or_die(sizeof(Inference));
  new_inference->game = game_duplicate(inference->game);
  new_inference->klv = player_get_klv(
      game_get_player(new_inference->game, inference->target_index));

  new_inference->ld_size = inference->ld_size;
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
  new_inference->gen = create_generator(1, new_inference->ld_size);

  new_inference->results = inference_results_create(
      get_leave_rack_list_capacity(
          inference_results_get_leave_rack_list(inference->results)),
      new_inference->ld_size);

  // Multithreading
  new_inference->thread_control = thread_control;

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

  while (get_leave_rack_list_count(lrl_to_update) > 0) {
    LeaveRack *leave_rack_to_update = pop_leave_rack(lrl_to_update);
    insert_leave_rack(leave_rack_get_leave(leave_rack_to_update),
                      leave_rack_get_exchanged(leave_rack_to_update),
                      lrl_to_add, leave_rack_get_draws(leave_rack_to_update),
                      leave_rack_get_equity(leave_rack_to_update));
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
    if (get_number_of_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      get_total_racks_evaluated(inference, tiles_to_infer - 1, letter,
                                total_racks_evaluated);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

bool should_print_info(const Inference *inference) {
  int print_info_interval = get_print_info_interval(inference->thread_control);
  return print_info_interval > 0 && inference->current_rack_index > 0 &&
         inference->current_rack_index % print_info_interval == 0;
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter,
                                         bool multithreaded) {
  if (is_halted(inference->thread_control)) {
    return;
  }
  if (tiles_to_infer == 0) {
    bool perform_evaluation = false;
    bool print_info = false;

    if (multithreaded) {
      pthread_mutex_lock(inference->shared_rack_index_lock);
      if (inference->current_rack_index == *inference->shared_rack_index) {
        print_info = should_print_info(inference);
        perform_evaluation = true;
        *inference->shared_rack_index += 1;
      }
      pthread_mutex_unlock(inference->shared_rack_index_lock);
    } else {
      print_info = should_print_info(inference);
      perform_evaluation = true;
    }

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
    if (get_number_of_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      iterate_through_all_possible_leaves(inference, tiles_to_infer - 1, letter,
                                          multithreaded);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

void *infer_worker(void *uncasted_inference) {
  Inference *inference = (Inference *)uncasted_inference;
  iterate_through_all_possible_leaves(
      inference,
      (RACK_SIZE)-get_number_of_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER, 1);
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
      (RACK_SIZE)-get_number_of_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER, &total_racks_evaluated);
  inference->total_racks_evaluated = total_racks_evaluated;

  print_ucgi_inference_total_racks_evaluated(total_racks_evaluated,
                                             thread_control);

  int number_of_threads = get_number_of_threads(thread_control);

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
        inference_duplicate(inference, thread_control);
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
  halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  combine_stats(leave_stats, number_of_threads,
                inference_results_get_equity_values(inference->results,
                                                    INFERENCE_TYPE_LEAVE));
  free(leave_stats);
  if (inference->target_number_of_tiles_exchanged > 0) {
    combine_stats(exchanged_stats, number_of_threads,
                  inference_results_get_equity_values(
                      inference->results, INFERENCE_TYPE_EXCHANGED));
    combine_stats(rack_stats, number_of_threads,
                  inference_results_get_equity_values(inference->results,
                                                      INFERENCE_TYPE_RACK));
    free(exchanged_stats);
    free(rack_stats);
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    destroy_inference_copy(inferences_for_workers[thread_index]);
  }

  free(inferences_for_workers);
  free(worker_ids);
}

inference_status_t verify_inference(const Inference *inference,
                                    Rack *config_target_played_tiles) {
  Rack *bag_as_rack = inference->bag_as_rack;
  for (int i = 0; i < inference->ld_size; i++) {
    if (get_number_of_letter(bag_as_rack, i) < 0) {
      return INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
    }
  }

  int infer_rack_number_of_letters =
      get_number_of_letters(config_target_played_tiles);

  int number_of_letters_in_bag = get_number_of_letters(bag_as_rack);

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

inference_status_t infer(const Config *config, Game *game,
                         InferenceResults **results) {
  ThreadControl *thread_control = config_get_thread_control(config);

  unhalt(thread_control);

  Rack *config_target_played_tiles = config_get_rack(config);

  if (!config_target_played_tiles) {
    return INFERENCE_STATUS_NO_TILES_PLAYED;
  }

  Inference *inference = inference_create(
      results, game, config_target_played_tiles, config_get_num_plays(config),
      config_get_target_index(config), config_get_target_score(config),
      config_get_target_number_of_tiles_exchanged(config),
      config_get_equity_margin(config));

  inference_status_t status =
      verify_inference(inference, config_target_played_tiles);

  if (status == INFERENCE_STATUS_SUCCESS) {
    infer_manager(thread_control, inference);

    inference_results_finalize(
        inference->results, inference->target_score,
        inference->target_number_of_tiles_exchanged, inference->equity_margin,
        config_target_played_tiles, inference->current_target_leave,
        inference->bag_as_rack);

    if (get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS) {
      // Only print if infer was able to finish normally.
      // If halt status isn't max iterations, it was interrupted
      // by the user and the results will not be valid.
      print_ucgi_inference(game_get_ld(inference->game), inference->results,
                           thread_control);
    }

    // Return the player to infer rack to it's original
    // state since the inference does not own that struct
    for (int i = 0; i < get_array_size(config_target_played_tiles); i++) {
      for (int j = 0; j < get_number_of_letter(config_target_played_tiles, i);
           j++) {
        take_letter_from_rack(inference->current_target_rack, i);
      }
    }
  }

  destroy_inference(inference);

  return status;
}

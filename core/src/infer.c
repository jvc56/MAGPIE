#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "klv.h"
#include "kwg.h"
#include "leave_rack.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "thread_control.h"
#include "ucgi_print.h"

InferenceRecord *create_inference_record(int draw_and_leave_subtotals_size) {
  InferenceRecord *record = malloc(sizeof(InferenceRecord));
  record->draw_and_leave_subtotals =
      (uint64_t *)malloc(draw_and_leave_subtotals_size * sizeof(uint64_t));
  record->equity_values = create_stat();
  return record;
}

void destroy_inference_record(InferenceRecord *record) {
  destroy_stat(record->equity_values);
  free(record->draw_and_leave_subtotals);
  free(record);
}

Inference *create_inference(int capacity, int distribution_size) {
  Inference *inference = malloc(sizeof(Inference));
  inference->distribution_size = distribution_size;
  inference->draw_and_leave_subtotals_size = distribution_size * (RACK_SIZE)*2;
  inference->bag_as_rack = create_rack(distribution_size);
  inference->leave = create_rack(distribution_size);
  inference->exchanged = create_rack(distribution_size);
  inference->leave_record =
      create_inference_record(inference->draw_and_leave_subtotals_size);
  inference->exchanged_record =
      create_inference_record(inference->draw_and_leave_subtotals_size);
  inference->rack_record =
      create_inference_record(inference->draw_and_leave_subtotals_size);
  inference->leave_rack_list =
      create_leave_rack_list(capacity, distribution_size);
  return inference;
}

void destroy_inference(Inference *inference) {
  destroy_rack(inference->bag_as_rack);
  destroy_rack(inference->leave);
  destroy_rack(inference->exchanged);
  destroy_leave_rack_list(inference->leave_rack_list);
  destroy_inference_record(inference->leave_record);
  destroy_inference_record(inference->exchanged_record);
  destroy_inference_record(inference->rack_record);
  free(inference);
}

void destroy_inference_copy(Inference *inference) {
  destroy_rack(inference->bag_as_rack);
  destroy_rack(inference->leave);
  destroy_rack(inference->exchanged);
  destroy_leave_rack_list(inference->leave_rack_list);
  destroy_inference_record(inference->leave_record);
  destroy_inference_record(inference->exchanged_record);
  destroy_inference_record(inference->rack_record);
  destroy_game(inference->game);
  free(inference);
}

// Functions for the inference record

int get_letter_subtotal_index(uint8_t letter, int number_of_letters,
                              int subtotal_index_offset) {
  return (letter * 2 * (RACK_SIZE)) + ((number_of_letters - 1) * 2) +
         subtotal_index_offset;
}

uint64_t get_subtotal(InferenceRecord *record, uint8_t letter,
                      int number_of_letters, int subtotal_index_offset) {
  return record->draw_and_leave_subtotals[get_letter_subtotal_index(
      letter, number_of_letters, subtotal_index_offset)];
}

void add_to_letter_subtotal(InferenceRecord *record, uint8_t letter,
                            int number_of_letters, int subtotal_index_offset,
                            uint64_t delta) {
  record->draw_and_leave_subtotals[get_letter_subtotal_index(
      letter, number_of_letters, subtotal_index_offset)] += delta;
}

uint64_t get_subtotal_sum_with_minimum(InferenceRecord *record, uint8_t letter,
                                       int minimum_number_of_letters,
                                       int subtotal_index_offset) {
  uint64_t sum = 0;
  for (int i = minimum_number_of_letters; i <= (RACK_SIZE); i++) {
    sum += get_subtotal(record, letter, i, subtotal_index_offset);
  }
  return sum;
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

uint64_t get_number_of_draws_for_rack(Rack *bag_as_rack, Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < rack->array_size; i++) {
    if (rack->array[i] > 0) {
      number_of_ways *=
          choose(bag_as_rack->array[i] + rack->array[i], rack->array[i]);
    }
  }
  return number_of_ways;
}

void get_stat_for_letter(InferenceRecord *record, Stat *stat, uint8_t letter) {
  reset_stat(stat);
  for (int i = 1; i <= (RACK_SIZE); i++) {
    uint64_t number_of_draws_with_exactly_i_of_letter =
        get_subtotal(record, letter, i, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
    if (number_of_draws_with_exactly_i_of_letter > 0) {
      push(stat, i, number_of_draws_with_exactly_i_of_letter);
    }
  }
  // Add the zero case to the stat
  // We do not have direct stats for when the letter
  // was never drawn so we infer it here
  uint64_t number_of_draws_without_letter =
      get_weight(record->equity_values) - get_weight(stat);
  push(stat, 0, number_of_draws_without_letter);
}

double
get_probability_for_random_minimum_draw(Rack *bag_as_rack, Rack *rack,
                                        uint8_t this_letter, int minimum,
                                        int number_of_actual_tiles_played) {
  int number_of_this_letters_already_on_rack = rack->array[this_letter];
  int minimum_adjusted_for_partial_rack =
      minimum - number_of_this_letters_already_on_rack;
  // If the partial leave already has the minimum
  // number of letters, the probability is trivially 1.
  if (minimum_adjusted_for_partial_rack <= 0) {
    return 1;
  }
  int total_number_of_letters_in_bag = bag_as_rack->number_of_letters;
  int total_number_of_letters_on_rack = rack->number_of_letters;
  int number_of_this_letter_in_bag = bag_as_rack->array[this_letter];

  // If there are not enough letters to meet the minimum, the probability
  // is trivially 0.
  if (number_of_this_letter_in_bag < minimum_adjusted_for_partial_rack) {
    return 0;
  }

  int total_number_of_letters_to_draw =
      (RACK_SIZE) -
      (total_number_of_letters_on_rack + number_of_actual_tiles_played);

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

double get_estimated_stdev_for_record(InferenceRecord *record) {
  return get_stdev_for_weighted_int_array(record->rounded_equity_values,
                                          (START_ROUNDED_EQUITY_VALUE));
}

void increment_subtotals_for_record(InferenceRecord *record, Rack *rack,
                                    uint64_t number_of_draws_for_leave) {
  for (int i = 0; i < rack->array_size; i++) {
    if (rack->array[i] > 0) {
      add_to_letter_subtotal(record, i, rack->array[i],
                             INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW,
                             number_of_draws_for_leave);
      add_to_letter_subtotal(record, i, rack->array[i],
                             INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
    }
  }
}

void record_valid_leave(InferenceRecord *record, Rack *rack,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  push(record->equity_values, (double)current_leave_value,
       number_of_draws_for_leave);
  increment_subtotals_for_record(record, rack, number_of_draws_for_leave);
  int rounded_equity_index =
      round_to_nearest_int(current_leave_value) - (START_ROUNDED_EQUITY_VALUE);
  if (rounded_equity_index < 0 ||
      rounded_equity_index >= (NUMBER_OF_ROUNDED_EQUITY_VALUES)) {
    printf("equity value out of range: %0.2f\n", current_leave_value);
    abort();
  }
  record->rounded_equity_values[rounded_equity_index] +=
      number_of_draws_for_leave;
}

Move *get_top_move(Inference *inference) {
  Game *game = inference->game;
  Player *player = game->players[inference->player_to_infer_index];
  reset_move_list(game->gen->move_list);
  generate_moves(game->gen, player,
                 game->players[1 - inference->player_to_infer_index]->rack,
                 game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
  return game->gen->move_list->moves[0];
}

void evaluate_possible_leave(Inference *inference) {
  double current_leave_value = 0;
  if (inference->number_of_tiles_exchanged == 0) {
    current_leave_value = get_leave_value(inference->klv, inference->leave);
  }
  Move *top_move = get_top_move(inference);
  int is_within_equity_margin = inference->actual_score + current_leave_value +
                                    inference->equity_margin +
                                    (INFERENCE_EQUITY_EPSILON) >=
                                top_move->equity;
  int number_exchanged_matches =
      top_move->move_type == MOVE_TYPE_EXCHANGE &&
      top_move->tiles_played == inference->number_of_tiles_exchanged;
  int recordable = is_within_equity_margin || number_exchanged_matches ||
                   inference->bag_as_rack->empty;
  if (recordable) {
    uint64_t number_of_draws_for_leave =
        get_number_of_draws_for_rack(inference->bag_as_rack, inference->leave);
    if (inference->number_of_tiles_exchanged > 0) {
      record_valid_leave(inference->rack_record, inference->leave,
                         current_leave_value, number_of_draws_for_leave);
      // The full rack for the exchange was recorded above,
      // but now we have to record the leave and the exchanged tiles
      for (int exchanged_tile_index = 0;
           exchanged_tile_index < top_move->tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = top_move->tiles[exchanged_tile_index];
        add_letter_to_rack(inference->exchanged, tile_exchanged);
        take_letter_from_rack(inference->leave, tile_exchanged);
      }
      record_valid_leave(inference->leave_record, inference->leave,
                         get_leave_value(inference->klv, inference->leave),
                         number_of_draws_for_leave);
      record_valid_leave(inference->exchanged_record, inference->exchanged,
                         get_leave_value(inference->klv, inference->exchanged),
                         number_of_draws_for_leave);
      insert_leave_rack(inference->leave_rack_list, inference->leave,
                        inference->exchanged, number_of_draws_for_leave,
                        current_leave_value);
      reset_rack(inference->exchanged);
      for (int exchanged_tile_index = 0;
           exchanged_tile_index < top_move->tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = top_move->tiles[exchanged_tile_index];
        add_letter_to_rack(inference->leave, tile_exchanged);
      }
    } else {
      record_valid_leave(inference->leave_record, inference->leave,
                         current_leave_value, number_of_draws_for_leave);
      insert_leave_rack(inference->leave_rack_list, inference->leave, NULL,
                        number_of_draws_for_leave, current_leave_value);
    }
  }
}

void increment_letter_for_inference(Inference *inference, uint8_t letter) {
  take_letter_from_rack(inference->bag_as_rack, letter);
  add_letter_to_rack(inference->player_to_infer_rack, letter);
  add_letter_to_rack(inference->leave, letter);
}

void decrement_letter_for_inference(Inference *inference, uint8_t letter) {
  add_letter_to_rack(inference->bag_as_rack, letter);
  take_letter_from_rack(inference->player_to_infer_rack, letter);
  take_letter_from_rack(inference->leave, letter);
}

void initialize_inference_record_for_evaluation(
    InferenceRecord *record, int draw_and_leave_subtotals_size) {
  // Reset record
  reset_stat(record->equity_values);
  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    record->draw_and_leave_subtotals[i] = 0;
  }
  for (int i = 0; i < (NUMBER_OF_ROUNDED_EQUITY_VALUES); i++) {
    record->rounded_equity_values[i] = 0;
  }
}

void initialize_inference_for_evaluation(Inference *inference, Game *game,
                                         Rack *actual_tiles_played,
                                         int player_to_infer_index,
                                         int actual_score,
                                         int number_of_tiles_exchanged,
                                         double equity_margin) {
  initialize_inference_record_for_evaluation(
      inference->leave_record, inference->draw_and_leave_subtotals_size);
  initialize_inference_record_for_evaluation(
      inference->exchanged_record, inference->draw_and_leave_subtotals_size);
  initialize_inference_record_for_evaluation(
      inference->rack_record, inference->draw_and_leave_subtotals_size);
  reset_leave_rack_list(inference->leave_rack_list);

  inference->game = game;
  inference->actual_score = actual_score;
  inference->number_of_tiles_exchanged = number_of_tiles_exchanged;
  inference->equity_margin = equity_margin;
  inference->current_rack_index = 0;

  inference->player_to_infer_index = player_to_infer_index;
  inference->klv = game->players[player_to_infer_index]->strategy_params->klv;
  inference->player_to_infer_rack = game->players[player_to_infer_index]->rack;

  reset_rack(inference->bag_as_rack);
  reset_rack(inference->leave);

  // Create the bag as a rack
  for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
    add_letter_to_rack(inference->bag_as_rack, game->gen->bag->tiles[i]);
  }

  // Add any existing tiles on the player's rack
  // to the player's leave for partial inferences
  for (int i = 0; i < inference->player_to_infer_rack->array_size; i++) {
    for (int j = 0; j < inference->player_to_infer_rack->array[i]; j++) {
      add_letter_to_rack(inference->leave, i);
    }
  }

  // Remove the tiles played in the move from the game bag
  // and add them to the player's rack
  for (int i = 0; i < actual_tiles_played->array_size; i++) {
    for (int j = 0; j < actual_tiles_played->array[i]; j++) {
      take_letter_from_rack(inference->bag_as_rack, i);
      add_letter_to_rack(inference->player_to_infer_rack, i);
    }
  }
}

InferenceRecord *copy_inference_record(InferenceRecord *inference_record,
                                       int draw_and_leave_subtotals_size) {
  InferenceRecord *new_record = malloc(sizeof(InferenceRecord));
  new_record->draw_and_leave_subtotals =
      (uint64_t *)malloc(draw_and_leave_subtotals_size * sizeof(uint64_t));
  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    new_record->draw_and_leave_subtotals[i] =
        inference_record->draw_and_leave_subtotals[i];
  }
  for (int i = 0; i < (NUMBER_OF_ROUNDED_EQUITY_VALUES); i++) {
    new_record->rounded_equity_values[i] =
        inference_record->rounded_equity_values[i];
  }
  new_record->equity_values = copy_stat(inference_record->equity_values);
  return new_record;
}

Inference *copy_inference(Inference *inference, ThreadControl *thread_control) {
  Inference *new_inference = malloc(sizeof(Inference));
  new_inference->distribution_size = inference->distribution_size;
  new_inference->draw_and_leave_subtotals_size =
      inference->distribution_size * (RACK_SIZE)*2;
  new_inference->bag_as_rack = copy_rack(inference->bag_as_rack);
  new_inference->leave = copy_rack(inference->leave);
  new_inference->exchanged = copy_rack(inference->exchanged);
  new_inference->leave_record = copy_inference_record(
      inference->leave_record, inference->draw_and_leave_subtotals_size);
  new_inference->exchanged_record = copy_inference_record(
      inference->exchanged_record, inference->draw_and_leave_subtotals_size);
  new_inference->rack_record = copy_inference_record(
      inference->rack_record, inference->draw_and_leave_subtotals_size);
  new_inference->leave_rack_list = create_leave_rack_list(
      inference->leave_rack_list->capacity, inference->distribution_size);

  // Game must be deep copied since we use the move generator
  new_inference->game = copy_game(inference->game, MOVE_LIST_CAPACITY);
  // KLV can just be a pointer since it is read only
  new_inference->klv = inference->klv;
  // Need the rack from the newly copied game
  new_inference->player_to_infer_rack =
      new_inference->game->players[inference->player_to_infer_index]->rack;

  new_inference->player_to_infer_index = inference->player_to_infer_index;
  new_inference->actual_score = inference->actual_score;
  new_inference->number_of_tiles_exchanged =
      inference->number_of_tiles_exchanged;
  new_inference->draw_and_leave_subtotals_size =
      inference->draw_and_leave_subtotals_size;
  new_inference->initial_tiles_to_infer = inference->initial_tiles_to_infer;
  new_inference->equity_margin = inference->equity_margin;
  new_inference->current_rack_index = inference->current_rack_index;

  // Multithreading
  new_inference->thread_control = thread_control;

  return new_inference;
}

void add_inference_record(InferenceRecord *inference_record_1,
                          InferenceRecord *inference_record_2,
                          int draw_and_leave_subtotals_size) {
  if (get_cardinality(inference_record_2->equity_values) == 0) {
    return;
  }
  push_stat(inference_record_1->equity_values,
            inference_record_2->equity_values);
  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    inference_record_1->draw_and_leave_subtotals[i] +=
        inference_record_2->draw_and_leave_subtotals[i];
  }
  for (int i = 0; i < (NUMBER_OF_ROUNDED_EQUITY_VALUES); i++) {
    inference_record_1->rounded_equity_values[i] +=
        inference_record_2->rounded_equity_values[i];
  }
}

void add_inference(Inference *inference_1, Inference *inference_2) {
  add_inference_record(inference_1->leave_record, inference_2->leave_record,
                       inference_1->draw_and_leave_subtotals_size);
  if (inference_2->number_of_tiles_exchanged > 0) {
    add_inference_record(inference_1->exchanged_record,
                         inference_2->exchanged_record,
                         inference_1->draw_and_leave_subtotals_size);
    add_inference_record(inference_1->rack_record, inference_2->rack_record,
                         inference_1->draw_and_leave_subtotals_size);
  }
  while (inference_2->leave_rack_list->count > 0) {
    LeaveRack *leave_rack_2 = pop_leave_rack(inference_2->leave_rack_list);
    insert_leave_rack(inference_1->leave_rack_list, leave_rack_2->leave,
                      leave_rack_2->exchanged, leave_rack_2->draws,
                      leave_rack_2->equity);
  }
}

void print_inference_info(uint64_t current_rack_index) {
  printf("inference is at %ld\n", current_rack_index);
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter,
                                         int multithreaded) {
  if (is_halted(inference->thread_control)) {
    return;
  }
  if (tiles_to_infer == 0) {
    int perform_evaluation = 0;
    int print_info = 0;

    if (multithreaded) {
      pthread_mutex_lock(inference->shared_rack_index_lock);
      if (inference->current_rack_index == *inference->shared_rack_index) {
        print_info = inference->thread_control->print_info_interval > 0 &&
                     inference->current_rack_index %
                             inference->thread_control->print_info_interval ==
                         0;
        perform_evaluation = 1;
        *inference->shared_rack_index += 1;
      }
      pthread_mutex_unlock(inference->shared_rack_index_lock);
    } else {
      print_info = inference->thread_control->print_info_interval > 0 &&
                   inference->current_rack_index %
                           inference->thread_control->print_info_interval ==
                       0;
      perform_evaluation = 1;
    }
    if (perform_evaluation) {
      evaluate_possible_leave(inference);
    }
    inference->current_rack_index++;
    if (print_info) {
      print_ucgi_inference_current_rack_index(inference->current_rack_index,
                                              inference->thread_control);
    }
    return;
  }
  for (int letter = start_letter; letter < inference->distribution_size;
       letter++) {
    if (inference->bag_as_rack->array[letter] > 0) {
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
      inference, inference->initial_tiles_to_infer, BLANK_MACHINE_LETTER, 1);
  reset_move_list(inference->game->gen->move_list);
  return NULL;
}

void infer_worker_single_threaded(Inference *inference) {
  iterate_through_all_possible_leaves(
      inference, inference->initial_tiles_to_infer, BLANK_MACHINE_LETTER, 0);
  reset_move_list(inference->game->gen->move_list);
}

void set_shared_variables_for_inference(
    Inference *inference, uint64_t *shared_rack_index,
    pthread_mutex_t *shared_rack_index_lock) {
  inference->shared_rack_index = shared_rack_index;
  inference->shared_rack_index_lock = shared_rack_index_lock;
}

void infer_manager(ThreadControl *thread_control, Inference *inference,
                   int number_of_threads) {

  if (number_of_threads == 1) {
    inference->thread_control = thread_control;
    infer_worker_single_threaded(inference);
    return;
  }

  uint64_t shared_rack_index = 0;
  pthread_mutex_t shared_rack_index_lock;

  if (pthread_mutex_init(&shared_rack_index_lock, NULL) != 0) {
    printf("mutex init failed\n");
    abort();
  }

  Inference **inferences_for_workers =
      malloc((sizeof(Inference *)) * (number_of_threads));
  pthread_t *worker_ids = malloc((sizeof(pthread_t)) * (number_of_threads));
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    inferences_for_workers[thread_index] =
        copy_inference(inference, thread_control);
    set_shared_variables_for_inference(inferences_for_workers[thread_index],
                                       &shared_rack_index,
                                       &shared_rack_index_lock);
    pthread_create(&worker_ids[thread_index], NULL, infer_worker,
                   inferences_for_workers[thread_index]);
  }

  // Combine and free
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    add_inference(inference, inferences_for_workers[thread_index]);
    destroy_inference_copy(inferences_for_workers[thread_index]);
  }

  free(inferences_for_workers);
  free(worker_ids);
}

void infer(ThreadControl *thread_control, Inference *inference, Game *game,
           Rack *actual_tiles_played, int player_to_infer_index,
           int actual_score, int number_of_tiles_exchanged,
           double equity_margin, int number_of_threads) {
  inference->status = INFERENCE_STATUS_RUNNING;

  initialize_inference_for_evaluation(inference, game, actual_tiles_played,
                                      player_to_infer_index, actual_score,
                                      number_of_tiles_exchanged, equity_margin);

  for (int i = 0; i < inference->distribution_size; i++) {
    if (inference->bag_as_rack->array[i] < 0) {
      inference->status = INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
      return;
    }
  }

  if (actual_tiles_played->number_of_letters == 0 &&
      number_of_tiles_exchanged == 0) {
    inference->status = INFERENCE_STATUS_NO_TILES_PLAYED;
    return;
  }

  if (actual_tiles_played->number_of_letters != 0 &&
      number_of_tiles_exchanged != 0) {
    inference->status = INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE;
    return;
  }

  if (number_of_tiles_exchanged != 0 &&
      inference->bag_as_rack->number_of_letters < (RACK_SIZE)*2) {
    inference->status = INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED;
    return;
  }

  if (number_of_tiles_exchanged != 0 && actual_score != 0) {
    inference->status = INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO;
    return;
  }

  if (game->players[player_to_infer_index]->rack->number_of_letters >
      (RACK_SIZE)) {
    inference->status = INFERENCE_STATUS_RACK_OVERFLOW;
    return;
  }

  if (number_of_threads < 1) {
    inference->status = INFERENCE_STATUS_INVALID_NUMBER_OF_THREADS;
    return;
  }

  int tiles_to_infer =
      (RACK_SIZE)-inference->player_to_infer_rack->number_of_letters;
  inference->initial_tiles_to_infer = tiles_to_infer;

  // Prepare the game for inference calculations
  Player *player = game->players[inference->player_to_infer_index];
  int original_recorder_type = player->strategy_params->play_recorder_type;
  int original_apply_placement_adjustment =
      game->gen->apply_placement_adjustment;
  game->gen->apply_placement_adjustment = 0;
  player->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;

  infer_manager(thread_control, inference, number_of_threads);

  player->strategy_params->play_recorder_type = original_recorder_type;
  game->gen->apply_placement_adjustment = original_apply_placement_adjustment;

  // Return the player to infer rack to it's original
  // state since the inference does not own that struct
  for (int i = 0; i < actual_tiles_played->array_size; i++) {
    for (int j = 0; j < actual_tiles_played->array[i]; j++) {
      take_letter_from_rack(inference->player_to_infer_rack, i);
    }
  }
  inference->status = INFERENCE_STATUS_SUCCESS;
}
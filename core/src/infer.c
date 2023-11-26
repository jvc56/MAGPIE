#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "klv.h"
#include "kwg.h"
#include "leave_rack.h"
#include "log.h"
#include "move.h"
#include "players_data.h"
#include "rack.h"
#include "stats.h"
#include "string_util.h"
#include "thread_control.h"
#include "ucgi_print.h"
#include "util.h"

InferenceRecord *create_inference_record(int draw_and_leave_subtotals_size) {
  InferenceRecord *record = malloc_or_die(sizeof(InferenceRecord));
  record->draw_and_leave_subtotals = (uint64_t *)malloc_or_die(
      draw_and_leave_subtotals_size * sizeof(uint64_t));
  record->equity_values = create_stat();
  return record;
}

void destroy_inference_record(InferenceRecord *record) {
  destroy_stat(record->equity_values);
  free(record->draw_and_leave_subtotals);
  free(record);
}

Inference *create_inference() {
  Inference *inference = malloc_or_die(sizeof(Inference));
  inference->total_racks_evaluated = 0;
  // These will be set when infer is called
  inference->distribution_size = 0;
  inference->draw_and_leave_subtotals_size = 0;
  inference->leave_record = NULL;
  inference->exchanged_record = NULL;
  inference->rack_record = NULL;
  inference->bag_as_rack = NULL;
  inference->leave = NULL;
  inference->exchanged = NULL;
  inference->leave_rack_list = NULL;
  return inference;
}

void destroy_inference(Inference *inference) {
  if (inference->bag_as_rack) {
    destroy_rack(inference->bag_as_rack);
  }
  if (inference->leave) {
    destroy_rack(inference->leave);
  }
  if (inference->exchanged) {
    destroy_rack(inference->exchanged);
  }
  if (inference->leave_rack_list) {
    destroy_leave_rack_list(inference->leave_rack_list);
  }
  if (inference->leave_record) {
    destroy_inference_record(inference->leave_record);
  }
  if (inference->exchanged_record) {
    destroy_inference_record(inference->exchanged_record);
  }
  if (inference->rack_record) {
    destroy_inference_record(inference->rack_record);
  }
  free(inference);
}

void destroy_inference_copy(Inference *inference) {
  destroy_game(inference->game);
  destroy_inference(inference);
}

// Functions for the inference record

int get_letter_subtotal_index(uint8_t letter, int number_of_letters,
                              int subtotal_index_offset) {
  return (letter * 2 * (RACK_SIZE)) + ((number_of_letters - 1) * 2) +
         subtotal_index_offset;
}

uint64_t get_subtotal(const InferenceRecord *record, uint8_t letter,
                      int number_of_letters, int subtotal_index_offset) {
  return record->draw_and_leave_subtotals[get_letter_subtotal_index(
      letter, number_of_letters, subtotal_index_offset)];
}

void add_to_letter_subtotal(const InferenceRecord *record, uint8_t letter,
                            int number_of_letters, int subtotal_index_offset,
                            uint64_t delta) {
  record->draw_and_leave_subtotals[get_letter_subtotal_index(
      letter, number_of_letters, subtotal_index_offset)] += delta;
}

uint64_t get_subtotal_sum_with_minimum(const InferenceRecord *record,
                                       uint8_t letter,
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

uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < rack->array_size; i++) {
    if (rack->array[i] > 0) {
      number_of_ways *=
          choose(bag_as_rack->array[i] + rack->array[i], rack->array[i]);
    }
  }
  return number_of_ways;
}

void get_stat_for_letter(const InferenceRecord *record, Stat *stat,
                         uint8_t letter) {
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

double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *rack, uint8_t this_letter, int minimum,
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

void increment_subtotals_for_record(const Rack *rack, InferenceRecord *record,
                                    uint64_t number_of_draws_for_leave) {
  printf("recording subtotals\n");
  for (int i = 0; i < rack->array_size; i++) {
    if (rack->array[i] > 0) {
      printf("adding %d: %d\n", i, rack->array[i]);
      add_to_letter_subtotal(record, i, rack->array[i],
                             INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW,
                             number_of_draws_for_leave);
      add_to_letter_subtotal(record, i, rack->array[i],
                             INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
    }
  }
}

void record_valid_leave(const Rack *rack, InferenceRecord *record,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  printf("recording valid leave\n");
  push(record->equity_values, (double)current_leave_value,
       number_of_draws_for_leave);
  increment_subtotals_for_record(rack, record, number_of_draws_for_leave);
}

Move *get_top_move(Inference *inference) {
  Game *game = inference->game;
  Player *player = game->players[inference->player_to_infer_index];
  reset_move_list(game->gen->move_list);
  generate_moves(game->players[1 - inference->player_to_infer_index]->rack,
                 game->gen, player,
                 get_tiles_remaining(game->gen->bag) >= RACK_SIZE,
                 MOVE_RECORD_BEST, MOVE_SORT_EQUITY, false);
  return game->gen->move_list->moves[0];
}

void evaluate_possible_leave(Inference *inference) {
  double current_leave_value = 0;
  if (inference->number_of_tiles_exchanged == 0) {
    current_leave_value = get_leave_value(inference->klv, inference->leave);
  }
  const Move *top_move = get_top_move(inference);
  bool is_within_equity_margin = inference->actual_score + current_leave_value +
                                     inference->equity_margin +
                                     (INFERENCE_EQUITY_EPSILON) >=
                                 top_move->equity;
  bool number_exchanged_matches =
      top_move->move_type == GAME_EVENT_EXCHANGE &&
      top_move->tiles_played == inference->number_of_tiles_exchanged;
  bool recordable = is_within_equity_margin || number_exchanged_matches ||
                    inference->bag_as_rack->empty;
  if (recordable) {
    uint64_t number_of_draws_for_leave =
        get_number_of_draws_for_rack(inference->bag_as_rack, inference->leave);
    if (inference->number_of_tiles_exchanged > 0) {
      record_valid_leave(inference->leave, inference->rack_record,
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
      record_valid_leave(inference->leave, inference->leave_record,
                         get_leave_value(inference->klv, inference->leave),
                         number_of_draws_for_leave);
      record_valid_leave(inference->exchanged, inference->exchanged_record,
                         get_leave_value(inference->klv, inference->exchanged),
                         number_of_draws_for_leave);
      insert_leave_rack(inference->leave, inference->exchanged,
                        inference->leave_rack_list, number_of_draws_for_leave,
                        current_leave_value);
      reset_rack(inference->exchanged);
      for (int exchanged_tile_index = 0;
           exchanged_tile_index < top_move->tiles_played;
           exchanged_tile_index++) {
        uint8_t tile_exchanged = top_move->tiles[exchanged_tile_index];
        add_letter_to_rack(inference->leave, tile_exchanged);
      }
    } else {
      printf("recording non exchange\n");
      record_valid_leave(inference->leave, inference->leave_record,
                         current_leave_value, number_of_draws_for_leave);
      insert_leave_rack(inference->leave, NULL, inference->leave_rack_list,
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
}

void initialize_inference_for_evaluation(
    Inference *inference, Game *game, Rack *actual_tiles_played,
    int move_capacity, int player_to_infer_index, int actual_score,
    int number_of_tiles_exchanged, double equity_margin) {

  bool rack_size_changed =
      actual_tiles_played->array_size != inference->distribution_size;
  inference->distribution_size = actual_tiles_played->array_size;
  inference->draw_and_leave_subtotals_size =
      inference->distribution_size * (RACK_SIZE) * 2;

  if (rack_size_changed) {
    // Rack size has changed, we need to recreate
    // anything that has a rack
    if (inference->leave_record) {
      destroy_inference_record(inference->leave_record);
      inference->leave_record = NULL;
    }
    if (inference->exchanged_record) {
      destroy_inference_record(inference->exchanged_record);
      inference->exchanged_record = NULL;
    }
    if (inference->rack_record) {
      destroy_inference_record(inference->rack_record);
      inference->rack_record = NULL;
    }
    if (inference->bag_as_rack) {
      destroy_rack(inference->bag_as_rack);
      inference->bag_as_rack = NULL;
    }
    if (inference->leave) {
      destroy_rack(inference->leave);
      inference->leave = NULL;
    }
    if (inference->exchanged) {
      destroy_rack(inference->exchanged);
      inference->exchanged = NULL;
    }
    if (inference->leave_rack_list) {
      destroy_leave_rack_list(inference->leave_rack_list);
      inference->leave_rack_list = NULL;
    }
  }

  // Possibly recreate records
  if (!inference->leave_record) {
    inference->leave_record =
        create_inference_record(inference->draw_and_leave_subtotals_size);
  }
  if (!inference->exchanged_record) {
    inference->exchanged_record =
        create_inference_record(inference->draw_and_leave_subtotals_size);
  }
  if (!inference->rack_record) {
    inference->rack_record =
        create_inference_record(inference->draw_and_leave_subtotals_size);
  }

  if (!inference->bag_as_rack) {
    inference->bag_as_rack = create_rack(inference->distribution_size);
  }
  if (!inference->leave) {
    inference->leave = create_rack(inference->distribution_size);
  }
  if (!inference->exchanged) {
    inference->exchanged = create_rack(inference->distribution_size);
  }

  if (!inference->leave_rack_list) {
    inference->leave_rack_list =
        create_leave_rack_list(move_capacity, inference->distribution_size);
  }

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
  inference->klv = game->players[player_to_infer_index]->klv;
  inference->player_to_infer_rack = game->players[player_to_infer_index]->rack;

  reset_rack(inference->bag_as_rack);
  reset_rack(inference->leave);

  add_bag_to_rack(game->gen->bag, inference->bag_as_rack);

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

InferenceRecord *
inference_record_duplicate(const InferenceRecord *inference_record,
                           int draw_and_leave_subtotals_size) {
  InferenceRecord *new_record = malloc_or_die(sizeof(InferenceRecord));
  new_record->draw_and_leave_subtotals = (uint64_t *)malloc_or_die(
      draw_and_leave_subtotals_size * sizeof(uint64_t));
  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    new_record->draw_and_leave_subtotals[i] =
        inference_record->draw_and_leave_subtotals[i];
  }
  new_record->equity_values = stat_duplicate(inference_record->equity_values);
  return new_record;
}

Inference *inference_duplicate(const Inference *inference,
                               ThreadControl *thread_control) {
  Inference *new_inference = malloc_or_die(sizeof(Inference));
  new_inference->distribution_size = inference->distribution_size;
  new_inference->draw_and_leave_subtotals_size =
      inference->distribution_size * (RACK_SIZE) * 2;
  new_inference->bag_as_rack = rack_duplicate(inference->bag_as_rack);
  new_inference->leave = rack_duplicate(inference->leave);
  new_inference->exchanged = rack_duplicate(inference->exchanged);
  new_inference->leave_record = inference_record_duplicate(
      inference->leave_record, inference->draw_and_leave_subtotals_size);
  new_inference->exchanged_record = inference_record_duplicate(
      inference->exchanged_record, inference->draw_and_leave_subtotals_size);
  new_inference->rack_record = inference_record_duplicate(
      inference->rack_record, inference->draw_and_leave_subtotals_size);
  new_inference->leave_rack_list = create_leave_rack_list(
      inference->leave_rack_list->capacity, inference->distribution_size);

  // Game must be deep copied since we use the move generator
  new_inference->game = game_duplicate(
      inference->game, inference->game->gen->move_list->capacity);
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

void add_inference_record(const InferenceRecord *inference_record_to_add,
                          InferenceRecord *inference_record_to_update,
                          int draw_and_leave_subtotals_size) {
  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    inference_record_to_update->draw_and_leave_subtotals[i] +=
        inference_record_to_add->draw_and_leave_subtotals[i];
  }
}

void add_inference(const Inference *inference_to_add,
                   Inference *inference_to_update) {
  add_inference_record(inference_to_add->leave_record,
                       inference_to_update->leave_record,
                       inference_to_add->draw_and_leave_subtotals_size);
  if (inference_to_update->number_of_tiles_exchanged > 0) {
    add_inference_record(inference_to_add->exchanged_record,
                         inference_to_update->exchanged_record,
                         inference_to_add->draw_and_leave_subtotals_size);
    add_inference_record(inference_to_add->rack_record,
                         inference_to_update->rack_record,
                         inference_to_add->draw_and_leave_subtotals_size);
  }
  while (inference_to_update->leave_rack_list->count > 0) {
    LeaveRack *leave_rack_to_update =
        pop_leave_rack(inference_to_update->leave_rack_list);
    insert_leave_rack(
        leave_rack_to_update->leave, leave_rack_to_update->exchanged,
        inference_to_add->leave_rack_list, leave_rack_to_update->draws,
        leave_rack_to_update->equity);
  }
}

void get_total_racks_evaluated(Inference *inference, int tiles_to_infer,
                               int start_letter,
                               uint64_t *total_racks_evaluated) {
  if (tiles_to_infer == 0) {
    *total_racks_evaluated += 1;
    return;
  }
  for (int letter = start_letter; letter < inference->distribution_size;
       letter++) {
    if (inference->bag_as_rack->array[letter] > 0) {
      increment_letter_for_inference(inference, letter);
      get_total_racks_evaluated(inference, tiles_to_infer - 1, letter,
                                total_racks_evaluated);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

bool should_print_info(const Inference *inference) {
  return inference->thread_control->print_info_interval > 0 &&
         inference->current_rack_index > 0 &&
         inference->current_rack_index %
                 inference->thread_control->print_info_interval ==
             0;
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

void infer_manager(ThreadControl *thread_control, Inference *inference) {
  uint64_t total_racks_evaluated = 0;
  get_total_racks_evaluated(inference, inference->initial_tiles_to_infer,
                            BLANK_MACHINE_LETTER, &total_racks_evaluated);
  inference->total_racks_evaluated = total_racks_evaluated;

  print_ucgi_inference_total_racks_evaluated(total_racks_evaluated,
                                             thread_control);

  int number_of_threads = thread_control->number_of_threads;
  if (number_of_threads == 1) {
    inference->thread_control = thread_control;
    infer_worker_single_threaded(inference);
    return;
  }

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

  if (inference->number_of_tiles_exchanged > 0) {
    exchanged_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
    rack_stats = malloc_or_die((sizeof(Stat *)) * (number_of_threads));
  }

  // Combine and free
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    Inference *inference_worker = inferences_for_workers[thread_index];
    add_inference(inference_worker, inference);
    leave_stats[thread_index] = inference_worker->leave_record->equity_values;
    if (inference->number_of_tiles_exchanged > 0) {
      exchanged_stats[thread_index] =
          inference_worker->exchanged_record->equity_values;
      rack_stats[thread_index] = inference_worker->rack_record->equity_values;
    }
  }

  // Infer was able to finish normally, which is when it
  // iterates through every rack
  halt(thread_control, HALT_STATUS_MAX_ITERATIONS);

  combine_stats(leave_stats, number_of_threads,
                inference->leave_record->equity_values);
  free(leave_stats);
  if (inference->number_of_tiles_exchanged > 0) {
    combine_stats(exchanged_stats, number_of_threads,
                  inference->exchanged_record->equity_values);
    combine_stats(rack_stats, number_of_threads,
                  inference->rack_record->equity_values);
    free(exchanged_stats);
    free(rack_stats);
  }

  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    destroy_inference_copy(inferences_for_workers[thread_index]);
  }

  if (get_halt_status(thread_control) == HALT_STATUS_MAX_ITERATIONS) {
    // Only print if infer was able to finish normally.
    // If halt status isn't max iterations, it was interrupted
    // by the user and the results will not be valid.
    print_ucgi_inference(inference, thread_control);
  }

  free(inferences_for_workers);
  free(worker_ids);
}

inference_status_t verify_inference(const Inference *inference) {
  for (int i = 0; i < inference->distribution_size; i++) {
    if (inference->bag_as_rack->array[i] < 0) {
      return INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
    }
  }

  if (inference->player_to_infer_rack->number_of_letters == 0 &&
      inference->number_of_tiles_exchanged == 0) {
    return INFERENCE_STATUS_NO_TILES_PLAYED;
  }

  if (inference->player_to_infer_rack->number_of_letters != 0 &&
      inference->number_of_tiles_exchanged != 0) {
    return INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE;
  }

  if (inference->number_of_tiles_exchanged != 0 &&
      inference->bag_as_rack->number_of_letters < (RACK_SIZE) * 2) {
    return INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED;
  }

  if (inference->number_of_tiles_exchanged != 0 &&
      inference->actual_score != 0) {
    return INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO;
  }

  if (inference->player_to_infer_rack->number_of_letters > (RACK_SIZE)) {
    return INFERENCE_STATUS_RACK_OVERFLOW;
  }

  return INFERENCE_STATUS_SUCCESS;
}

inference_status_t infer(const Config *config, Game *game,
                         Inference *inference) {
  unhalt(config->thread_control);

  if (!config->rack) {
    return INFERENCE_STATUS_NO_TILES_PLAYED;
  }
  initialize_inference_for_evaluation(
      inference, game, config->rack, config->num_plays,
      config->player_to_infer_index, config->actual_score,
      config->number_of_tiles_exchanged, config->equity_margin);

  inference_status_t status = verify_inference(inference);
  if (status != INFERENCE_STATUS_SUCCESS) {
    return status;
  }

  int tiles_to_infer =
      (RACK_SIZE)-inference->player_to_infer_rack->number_of_letters;
  inference->initial_tiles_to_infer = tiles_to_infer;

  infer_manager(config->thread_control, inference);

  // Return the player to infer rack to it's original
  // state since the inference does not own that struct
  for (int i = 0; i < config->rack->array_size; i++) {
    for (int j = 0; j < config->rack->array[i]; j++) {
      take_letter_from_rack(inference->player_to_infer_rack, i);
    }
  }
  return INFERENCE_STATUS_SUCCESS;
}

// Human readable print functions

void string_builder_add_leave_rack(
    const LeaveRack *leave_rack, const LetterDistribution *letter_distribution,
    StringBuilder *inference_string, int index, uint64_t total_draws) {
  if (leave_rack->exchanged->empty) {
    string_builder_add_rack(leave_rack->leave, letter_distribution,
                            inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)leave_rack->draws / total_draws) * 100, leave_rack->draws,
        leave_rack->equity);
  } else {
    string_builder_add_rack(leave_rack->leave, letter_distribution,
                            inference_string);
    string_builder_add_spaces(inference_string, 1);
    string_builder_add_rack(leave_rack->exchanged, letter_distribution,
                            inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)leave_rack->draws / total_draws) * 100, leave_rack->draws);
  }
}

void string_builder_add_letter_minimum(
    const InferenceRecord *record, const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *inference_string, uint8_t letter, int minimum,
    int number_of_tiles_played_or_exchanged) {
  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)get_weight(record->equity_values);
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  string_builder_add_formatted_string(
      inference_string, " | %-7.2f %-7.2f%-9d%-9d", inference_probability * 100,
      random_probability * 100, draw_subtotal, leave_subtotal);
}

void string_builder_add_letter_line(const Game *game,
                                    const InferenceRecord *record,
                                    const Rack *rack, const Rack *bag_as_rack,
                                    StringBuilder *inference_string,
                                    Stat *letter_stat, uint8_t letter,
                                    int max_duplicate_letter_draw,
                                    int number_of_tiles_played_or_exchanged) {
  get_stat_for_letter(record, letter_stat, letter);
  string_builder_add_user_visible_letter(game->gen->letter_distribution,
                                         inference_string, letter);
  string_builder_add_formatted_string(inference_string, ": %4.2f %4.2f",
                                      get_mean(letter_stat),
                                      get_stdev(letter_stat));

  for (int i = 1; i <= max_duplicate_letter_draw; i++) {
    string_builder_add_letter_minimum(record, rack, bag_as_rack,
                                      inference_string, letter, i,
                                      number_of_tiles_played_or_exchanged);
  }
  string_builder_add_string(inference_string, "\n");
}

void string_builder_add_inference_record(
    const InferenceRecord *record, const Game *game, const Rack *rack,
    const Rack *bag_as_rack, StringBuilder *inference_string, Stat *letter_stat,
    int number_of_tiles_played_or_exchanged) {
  uint64_t total_draws = get_weight(record->equity_values);
  uint64_t total_leaves = get_cardinality(record->equity_values);

  string_builder_add_formatted_string(
      inference_string,
      "Total possible leave draws:   %lu\nTotal possible unique leaves: "
      "%lu\nAverage leave value:          %0.2fStdev leave value:            "
      "%0.2f",
      total_draws, total_leaves, get_mean(record->equity_values),
      get_stdev(record->equity_values));
  int max_duplicate_letter_draw = 0;
  for (int letter = 0; letter < (int)game->gen->letter_distribution->size;
       letter++) {
    for (int number_of_letter = 1; number_of_letter <= (RACK_SIZE);
         number_of_letter++) {
      int draws =
          get_subtotal_sum_with_minimum(record, letter, number_of_letter,
                                        INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
      if (draws == 0) {
        break;
      }
      if (number_of_letter > max_duplicate_letter_draw) {
        max_duplicate_letter_draw = number_of_letter;
      }
    }
  }

  string_builder_add_string(inference_string, "               ");
  for (int i = 0; i < max_duplicate_letter_draw; i++) {
    string_builder_add_formatted_string(inference_string, "Has at least %d of",
                                        i + 1);
  }
  string_builder_add_string(inference_string, "\n\n   Avg  Std ");

  for (int i = 0; i < max_duplicate_letter_draw; i++) {
    string_builder_add_string(inference_string,
                              " | Pct     Rand   Tot      Unq      ");
  }
  string_builder_add_string(inference_string, "\n");

  if (total_draws > 0) {
    for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
      string_builder_add_letter_line(
          game, record, rack, bag_as_rack, inference_string, letter_stat, i,
          max_duplicate_letter_draw, number_of_tiles_played_or_exchanged);
    }
  }
}

void string_builder_add_inference(const Inference *inference,
                                  const Rack *actual_tiles_played,
                                  StringBuilder *inference_string) {
  bool is_exchange = inference->number_of_tiles_exchanged > 0;
  int number_of_tiles_played_or_exchanged;
  const Game *game = inference->game;

  string_builder_add_game(game, inference_string);

  if (!is_exchange) {
    string_builder_add_string(inference_string, "Played tiles:          ");
    string_builder_add_rack(actual_tiles_played,
                            inference->game->gen->letter_distribution,
                            inference_string);
    number_of_tiles_played_or_exchanged =
        actual_tiles_played->number_of_letters;
  } else {
    string_builder_add_formatted_string(inference_string,
                                        "Exchanged tiles:       %d",
                                        inference->number_of_tiles_exchanged);
    number_of_tiles_played_or_exchanged = inference->number_of_tiles_exchanged;
  }

  string_builder_add_formatted_string(inference_string,
                                      "\nScore:                 %d\n",
                                      inference->actual_score);

  if (inference->player_to_infer_rack->number_of_letters > 0) {
    string_builder_add_string(inference_string, "Partial Rack:          ");
    string_builder_add_rack(inference->player_to_infer_rack,
                            inference->game->gen->letter_distribution,
                            inference_string);
    string_builder_add_string(inference_string, "\n");
  }

  string_builder_add_formatted_string(inference_string,
                                      "Equity margin:         %0.2f\n",
                                      inference->equity_margin);

  // Create a transient stat to use the stat functions
  Stat *letter_stat = create_stat();

  string_builder_add_inference_record(
      inference->leave_record, game, inference->leave, inference->bag_as_rack,
      inference_string, letter_stat, number_of_tiles_played_or_exchanged);
  const InferenceRecord *common_leaves_record = inference->leave_record;
  if (is_exchange) {
    common_leaves_record = inference->rack_record;
    string_builder_add_string(inference_string, "\n\nTiles Exchanged\n\n");
    Rack *unknown_exchange_rack = create_rack(inference->leave->array_size);
    string_builder_add_inference_record(
        inference->exchanged_record, game, unknown_exchange_rack,
        inference->bag_as_rack, inference_string, letter_stat,
        inference->number_of_tiles_exchanged);
    destroy_rack(unknown_exchange_rack);
    string_builder_add_string(inference_string, "\n\nRack\n\n");
    string_builder_add_inference_record(
        inference->rack_record, game, inference->leave, inference->bag_as_rack,
        inference_string, letter_stat, 0);
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Exch    Pct    Draws\n");
  } else {
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Pct    Draws  Equity\n");
  }
  destroy_stat(letter_stat);

  // Get the list of most common leaves
  int number_of_common_leaves = inference->leave_rack_list->count;
  sort_leave_racks(inference->leave_rack_list);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    LeaveRack *leave_rack =
        inference->leave_rack_list->leave_racks[common_leave_index];
    string_builder_add_leave_rack(
        leave_rack, game->gen->letter_distribution, inference_string,
        common_leave_index, get_weight(common_leaves_record->equity_values));
  }
}
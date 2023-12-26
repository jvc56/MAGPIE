#include "inference_results.h"

#include "../def/inference_defs.h"
#include "../def/rack_defs.h"

#include "../util/util.h"

#include "leave_rack.h"
#include "stats.h"

#define NUMBER_OF_STAT_TYPES 3

struct InferenceResults {
  int subtotals_size;
  // Indexed by inference_stat_t
  Stat *equity_values[NUMBER_OF_STAT_TYPES];
  // Indexed by inference_stat_t
  uint64_t *subtotals[NUMBER_OF_STAT_TYPES];
  LeaveRackList *leave_rack_list;

  // Fields that are finalized at the end of
  // the inference execution
  int target_number_of_tiles_exchanged;
  int target_score;
  double equity_margin;
  Rack *target_played_tiles;
  Rack *target_known_unplayed_tiles;
  Rack *bag_as_rack;
};

int get_subtotals_size(int ld_size) { return ld_size * (RACK_SIZE) * 2; }

// The created inference results takes ownership of all of the
// Rack pointers passed to it in the constructor.
InferenceResults *inference_results_create() {
  InferenceResults *results = malloc_or_die(sizeof(InferenceResults));
  results->subtotals_size = 0;
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    results->equity_values[i] = NULL;
    results->subtotals[i] = NULL;
  }
  results->leave_rack_list = NULL;
  results->target_played_tiles = NULL;
  results->target_known_unplayed_tiles = NULL;
  results->bag_as_rack = NULL;
  results->subtotals_size = 0;
  return results;
}

void inference_results_destroy_internal(InferenceResults *results) {
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    if (results->equity_values[i]) {
      destroy_stat(results->equity_values[i]);
    }
    free(results->subtotals[i]);
  }
  if (results->leave_rack_list) {
    destroy_leave_rack_list(results->leave_rack_list);
  }
  if (results->target_played_tiles) {
    destroy_rack(results->target_played_tiles);
  }
  if (results->target_known_unplayed_tiles) {
    destroy_rack(results->target_known_unplayed_tiles);
  }
  if (results->bag_as_rack) {
    destroy_rack(results->bag_as_rack);
  }
}

void inference_results_destroy(InferenceResults *results) {
  inference_results_destroy_internal(results);
  free(results);
}

void inference_results_reset(InferenceResults *results, int move_capacity,
                             int ld_size) {
  inference_results_destroy_internal(results);

  results->subtotals_size = get_subtotals_size(ld_size);
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    results->equity_values[i] = create_stat();
    results->subtotals[i] =
        (uint64_t *)malloc_or_die(results->subtotals_size * sizeof(uint64_t));
    for (int j = 0; j < results->subtotals_size; j++) {
      results->subtotals[i][j] = 0;
    }
  }

  results->leave_rack_list = create_leave_rack_list(move_capacity, ld_size);
  results->target_played_tiles = NULL;
  results->target_known_unplayed_tiles = NULL;
  results->bag_as_rack = NULL;
}

void inference_results_finalize(InferenceResults *results, int target_score,
                                int target_number_of_tiles_exchanged,
                                double equity_margin, Rack *target_played_tiles,
                                Rack *target_known_unplayed_tiles,
                                Rack *bag_as_rack) {
  results->target_score = target_score;
  results->target_number_of_tiles_exchanged = target_number_of_tiles_exchanged;
  results->equity_margin = equity_margin;
  results->target_played_tiles = rack_duplicate(target_played_tiles);
  results->target_known_unplayed_tiles =
      rack_duplicate(target_known_unplayed_tiles);
  results->bag_as_rack = rack_duplicate(bag_as_rack);
}

int inference_results_get_target_number_of_tiles_exchanged(
    const InferenceResults *results) {
  return results->target_number_of_tiles_exchanged;
}

int inference_results_get_target_score(const InferenceResults *results) {
  return results->target_score;
}

double inference_results_get_equity_margin(const InferenceResults *results) {
  return results->equity_margin;
}

Rack *
inference_results_get_target_played_tiles(const InferenceResults *results) {
  return results->target_played_tiles;
}

Rack *inference_results_get_target_known_unplayed_tiles(
    const InferenceResults *results) {
  return results->target_known_unplayed_tiles;
}

Rack *inference_results_get_bag_as_rack(const InferenceResults *results) {
  return results->bag_as_rack;
}

Stat *
inference_results_get_equity_values(InferenceResults *results,
                                    inference_stat_t inference_stat_type) {
  return results->equity_values[(int)inference_stat_type];
}

LeaveRackList *
inference_results_get_leave_rack_list(InferenceResults *inference_results) {
  return inference_results->leave_rack_list;
}

int get_letter_subtotal_index(uint8_t letter, int number_of_letters,
                              inference_subtotal_t subtotal_type) {
  return (letter * 2 * (RACK_SIZE)) + ((number_of_letters - 1) * 2) +
         subtotal_type;
}

uint64_t get_subtotal(InferenceResults *results,
                      inference_stat_t inference_stat_type, uint8_t letter,
                      int number_of_letters,
                      inference_subtotal_t subtotal_type) {
  return results->subtotals[(int)inference_stat_type][get_letter_subtotal_index(
      letter, number_of_letters, (int)subtotal_type)];
}

void add_to_letter_subtotal(InferenceResults *results,
                            inference_stat_t inference_stat_type,
                            uint8_t letter, int number_of_letters,
                            inference_subtotal_t subtotal_type,
                            uint64_t delta) {
  results->subtotals[inference_stat_type][get_letter_subtotal_index(
      letter, number_of_letters, (int)subtotal_type)] += delta;
}

uint64_t get_subtotal_sum_with_minimum(InferenceResults *results,
                                       inference_stat_t inference_stat_type,
                                       uint8_t letter,
                                       int minimum_number_of_letters,
                                       int subtotal_index_offset) {
  uint64_t sum = 0;
  for (int i = minimum_number_of_letters; i <= (RACK_SIZE); i++) {
    sum += get_subtotal(results, inference_stat_type, letter, i,
                        subtotal_index_offset);
  }
  return sum;
}

void inference_results_add_subtotals(InferenceResults *result_being_added,
                                     InferenceResults *result_being_updated) {
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    for (int j = 0; j < result_being_updated->subtotals_size; j++) {
      result_being_updated->subtotals[i][j] +=
          result_being_added->subtotals[i][j];
    }
  }
}

void set_stat_for_letter(InferenceResults *inference_results,
                         inference_stat_t inference_stat_type, Stat *stat,
                         uint8_t letter) {
  reset_stat(stat);
  for (int i = 1; i <= (RACK_SIZE); i++) {
    uint64_t number_of_draws_with_exactly_i_of_letter =
        get_subtotal(inference_results, inference_stat_type, letter, i,
                     INFERENCE_SUBTOTAL_DRAW);
    if (number_of_draws_with_exactly_i_of_letter > 0) {
      push(stat, i, number_of_draws_with_exactly_i_of_letter);
    }
  }
  // Add the zero case to the stat
  // We do not have direct stats for when the letter
  // was never drawn so we infer it here
  uint64_t number_of_draws_without_letter =
      get_weight(inference_results_get_equity_values(inference_results,
                                                     inference_stat_type)) -
      get_weight(stat);
  push(stat, 0, number_of_draws_without_letter);
}
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
};

InferenceResults *inference_results_create(int move_capacity, int ld_size,
                                           int subtotals_size) {
  InferenceResults *results = malloc_or_die(sizeof(InferenceResults));
  results->subtotals_size = subtotals_size;
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    results->equity_values[i] = create_stat();
    results->subtotals[i] =
        (uint64_t *)malloc_or_die(subtotals_size * sizeof(uint64_t));
    for (int j = 0; j < subtotals_size; j++) {
      results->subtotals[i][j] = 0;
    }
  }
  results->leave_rack_list = create_leave_rack_list(move_capacity, ld_size);
  return results;
}

void inference_results_destroy(InferenceResults *results) {
  for (int i = 0; i < NUMBER_OF_STAT_TYPES; i++) {
    destroy_stat(results->equity_values[i]);
    free(results->subtotals[i]);
  }
  destroy_leave_rack_list(results->leave_rack_list);
  free(results);
}

Stat *
inference_results_get_equity_values(InferenceResults *results,
                                    inference_stat_t inference_stat_type) {
  return results->equity_values[(int)inference_stat_type];
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

LeaveRackList *
inference_results_get_leave_rack_list(InferenceResults *inference_results) {
  return inference_results->leave_rack_list;
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
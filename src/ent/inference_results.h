#ifndef INFERENCE_RESULTS_H
#define INFERENCE_RESULTS_H

#include "../def/inference_defs.h"
#include "alias_method.h"
#include "leave_rack.h"
#include "rack.h"
#include "stats.h"
#include <stdint.h>

#define SUBTOTALS_SIZE                                                         \
  (MAX_ALPHABET_SIZE * (RACK_SIZE) * NUMBER_OF_INFERENCE_SUBTOTALS)

typedef struct InferenceResults InferenceResults;

struct InferenceResults {
  // Indexed by inference_stat_t
  Stat *equity_values[NUMBER_OF_INFER_TYPES];
  // Indexed by inference_stat_t
  uint64_t subtotals[NUMBER_OF_INFER_TYPES][SUBTOTALS_SIZE];
  LeaveRackList *leave_rack_list;
  AliasMethod *alias_method;
  bool alias_method_created_internally;

  // Fields that are finalized at the end of
  // the inference execution
  int target_number_of_tiles_exchanged;
  Equity target_score;
  Equity equity_margin;
  Rack target_played_tiles;
  Rack target_known_unplayed_tiles;
  Rack bag_as_rack;
};

InferenceResults *inference_results_create(AliasMethod *alias_method);
void inference_results_destroy(InferenceResults *inference_results);
void inference_results_reset(InferenceResults *results, int move_capacity,
                             int ld_size);
void inference_results_finalize(const Rack *target_played_tiles,
                                const Rack *target_known_unplayed_tiles,
                                const Rack *bag_as_rack,
                                InferenceResults *results, Equity target_score,
                                int target_number_of_tiles_exchanged,
                                Equity equity_margin);

int inference_results_get_target_number_of_tiles_exchanged(
    const InferenceResults *results);
Equity inference_results_get_target_score(const InferenceResults *results);
Equity inference_results_get_equity_margin(const InferenceResults *results);
const Rack *
inference_results_get_target_played_tiles(const InferenceResults *results);
const Rack *inference_results_get_target_known_unplayed_tiles(
    const InferenceResults *results);
const Rack *inference_results_get_bag_as_rack(const InferenceResults *results);

Stat *inference_results_get_equity_values(InferenceResults *results,
                                          inference_stat_t inference_stat_type);
uint64_t inference_results_get_subtotal(const InferenceResults *results,
                                        inference_stat_t inference_stat_type,
                                        MachineLetter letter,
                                        int number_of_letters,
                                        inference_subtotal_t subtotal_type);

void inference_results_add_to_letter_subtotal(
    InferenceResults *results, inference_stat_t inference_stat_type,
    MachineLetter letter, int number_of_letters,
    inference_subtotal_t subtotal_type, uint64_t delta);
uint64_t inference_results_get_subtotal_sum_with_minimum(
    const InferenceResults *results, inference_stat_t inference_stat_type,
    MachineLetter letter, int minimum_number_of_letters,
    int subtotal_index_offset);
LeaveRackList *inference_results_get_leave_rack_list(
    const InferenceResults *inference_results);
AliasMethod *
inference_results_get_alias_method(const InferenceResults *inference_results);
void inference_results_add_subtotals(InferenceResults *result_being_added,
                                     InferenceResults *result_being_updated);
void inference_results_set_stat_for_letter(InferenceResults *inference_results,
                                           inference_stat_t inference_stat_type,
                                           Stat *stat, MachineLetter letter);
double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *target_rack, MachineLetter this_letter,
    int minimum, int number_of_target_played_tiles);

#endif
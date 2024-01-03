#ifndef INFERENCE_RESULTS_H
#define INFERENCE_RESULTS_H

#include "../def/inference_defs.h"

#include "leave_rack.h"
#include "letter_distribution.h"
#include "stats.h"

struct InferenceResults;
typedef struct InferenceResults InferenceResults;

InferenceResults *inference_results_create();
void inference_results_destroy(InferenceResults *inference_results);
void inference_results_reset(InferenceResults *results, int move_capacity,
                             int ld_size);
void inference_results_finalize(const Rack *target_played_tiles,
                                const Rack *target_known_unplayed_tiles,
                                const Rack *bag_as_rack,
                                InferenceResults *results, int target_score,
                                int target_number_of_tiles_exchanged,
                                double equity_margin);

int inference_results_get_target_number_of_tiles_exchanged(
    const InferenceResults *results);
int inference_results_get_target_score(const InferenceResults *results);
double inference_results_get_equity_margin(const InferenceResults *results);
Rack *
inference_results_get_target_played_tiles(const InferenceResults *results);
Rack *inference_results_get_target_known_unplayed_tiles(
    const InferenceResults *results);
Rack *inference_results_get_bag_as_rack(const InferenceResults *results);

Stat *inference_results_get_equity_values(InferenceResults *results,
                                          inference_stat_t inference_stat_type);
uint64_t inference_results_get_subtotal(const InferenceResults *results,
                                        inference_stat_t inference_stat_type,
                                        uint8_t letter, int number_of_letters,
                                        inference_subtotal_t subtotal_type);

void inference_results_add_to_letter_subtotal(
    InferenceResults *results, inference_stat_t inference_stat_type,
    uint8_t letter, int number_of_letters, inference_subtotal_t subtotal_type,
    uint64_t delta);
uint64_t inference_results_get_subtotal_sum_with_minimum(
    const InferenceResults *results, inference_stat_t inference_stat_type,
    uint8_t letter, int minimum_number_of_letters, int subtotal_index_offset);
LeaveRackList *
inference_results_get_leave_rack_list(InferenceResults *inference_results);
void inference_results_add_subtotals(InferenceResults *result_being_added,
                                     InferenceResults *result_being_updated);
void inference_results_set_stat_for_letter(InferenceResults *inference_results,
                                           inference_stat_t inference_stat_type,
                                           Stat *stat, uint8_t letter);
int inference_results_get_number_of_sorted_leave_racks(
    const InferenceResults *inference_results);

double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *target_rack, uint8_t this_letter,
    int minimum, int number_of_target_played_tiles);

#endif
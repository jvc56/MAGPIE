#ifndef INFERENCE_RESULTS_H
#define INFERENCE_RESULTS_H

#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "alias_method.h"
#include "leave_rack.h"
#include "rack.h"
#include "stats.h"
#include <stdint.h>

typedef struct InferenceResults InferenceResults;

// Per-leave entry for winpct mode: the leave composition (as letter counts)
// and its accumulated importance weight.
typedef struct {
  uint16_t counts[MAX_ALPHABET_SIZE];
  double weight;
} WinPctLeave;

InferenceResults *inference_results_create(AliasMethod *alias_method);
void inference_results_destroy(InferenceResults *inference_results);
void inference_results_reset(InferenceResults *results, int leave_list_capacity,
                             int ld_size);
void inference_results_finalize(const Rack *target_played_tiles,
                                const Rack *target_known_unplayed_tiles,
                                const Rack *bag_as_rack,
                                InferenceResults *results, Equity target_score,
                                int target_number_of_tiles_exchanged,
                                Equity equity_margin, inference_mode_t mode,
                                bool interrupted);

int inference_results_get_target_number_of_tiles_exchanged(
    const InferenceResults *results);
Equity inference_results_get_target_score(const InferenceResults *results);
Equity inference_results_get_equity_margin(const InferenceResults *results);
void inference_results_set_valid_for_current_game_state(
    InferenceResults *results, bool valid);
bool inference_results_get_valid_for_current_game_state(
    const InferenceResults *results);
bool inference_results_get_interrupted(const InferenceResults *results);
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

// Win-probability mode getters.
inference_mode_t inference_results_get_mode(const InferenceResults *results);
double inference_results_get_total_weight(const InferenceResults *results);
double inference_results_get_ess(const InferenceResults *results);
double inference_results_get_letter_posterior(const InferenceResults *results,
                                              inference_stat_t type,
                                              MachineLetter ml);
void inference_results_accumulate_winpct_weight(InferenceResults *results,
                                                inference_stat_t type,
                                                const Rack *leave,
                                                double weight);

void inference_results_add_sim(InferenceResults *results);
void inference_results_add_iter(InferenceResults *results);
uint64_t inference_results_get_sim_count(const InferenceResults *results);
uint64_t inference_results_get_iter_count(const InferenceResults *results);
int inference_results_get_winpct_leave_count(const InferenceResults *results);
const WinPctLeave *
inference_results_get_winpct_leave(const InferenceResults *results, int idx);

#endif
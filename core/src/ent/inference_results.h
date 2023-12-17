#ifndef INFERENCE_RESULTS_H
#define INFERENCE_RESULTS_H

#include "../def/inference_defs.h"

#include "leave_rack.h"
#include "letter_distribution.h"
#include "stats.h"

struct InferenceResults;
typedef struct InferenceResults InferenceResults;

InferenceResults *inference_results_create(int move_capacity, int ld_size,
                                           int subtotals_size);
void inference_results_destroy(InferenceResults *inference_results);

Stat *inference_results_get_equity_values(InferenceResults *results,
                                          inference_stat_t inference_stat_type);

uint64_t get_subtotal(InferenceResults *results,
                      inference_stat_t inference_stat_type, uint8_t letter,
                      int number_of_letters,
                      inference_subtotal_t subtotal_type);
void add_to_letter_subtotal(InferenceResults *results,
                            inference_stat_t inference_stat_type,
                            uint8_t letter, int number_of_letters,
                            inference_subtotal_t subtotal_type, uint64_t delta);
uint64_t get_subtotal_sum_with_minimum(InferenceResults *results,
                                       inference_stat_t inference_stat_type,
                                       uint8_t letter,
                                       int minimum_number_of_letters,
                                       int subtotal_index_offset);
LeaveRackList *
inference_results_get_leave_rack_list(InferenceResults *inference_results);
void inference_results_add_subtotals(InferenceResults *result_being_added,
                                     InferenceResults *result_being_updated);
void set_stat_for_letter(InferenceResults *inference_results,
                         inference_stat_t inference_stat_type, Stat *stat,
                         uint8_t letter);
#endif
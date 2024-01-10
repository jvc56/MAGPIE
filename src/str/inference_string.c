#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/inference_defs.h"
#include "../def/rack_defs.h"

#include "../ent/inference_results.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"

#include "letter_distribution_string.h"
#include "rack_string.h"

#include "../util/string_util.h"

void string_builder_add_leave_rack(const LeaveRack *leave_rack,
                                   const LetterDistribution *ld,
                                   StringBuilder *inference_string, int index,
                                   uint64_t total_draws) {

  Rack *leave_rack_leave = leave_rack_get_leave(leave_rack);
  Rack *leave_rack_exchanged = leave_rack_get_exchanged(leave_rack);
  int leave_rack_draws = leave_rack_get_draws(leave_rack);
  double leave_rack_equity = leave_rack_get_equity(leave_rack);

  if (rack_is_empty(leave_rack_exchanged)) {
    string_builder_add_rack(leave_rack_leave, ld, inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)leave_rack_draws / total_draws) * 100, leave_rack_draws,
        leave_rack_equity);
  } else {
    string_builder_add_rack(leave_rack_leave, ld, inference_string);
    string_builder_add_spaces(inference_string, 1);
    string_builder_add_rack(leave_rack_exchanged, ld, inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)leave_rack_draws / total_draws) * 100, leave_rack_draws);
  }
}

void string_builder_add_letter_minimum(
    InferenceResults *inference_results, inference_stat_t inference_stat_type,
    const Rack *rack, const Rack *bag_as_rack, StringBuilder *inference_string,
    uint8_t letter, int minimum, int number_of_tiles_played_or_exchanged) {

  Stat *equity_values = inference_results_get_equity_values(
      inference_results, inference_stat_type);

  int draw_subtotal = inference_results_get_subtotal_sum_with_minimum(
      inference_results, inference_stat_type, letter, minimum,
      INFERENCE_SUBTOTAL_DRAW);
  int leave_subtotal = inference_results_get_subtotal_sum_with_minimum(
      inference_results, inference_stat_type, letter, minimum,
      INFERENCE_SUBTOTAL_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)stat_get_weight(equity_values);
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  string_builder_add_formatted_string(
      inference_string, " | %-7.2f %-7.2f%-9d%-9d", inference_probability * 100,
      random_probability * 100, draw_subtotal, leave_subtotal);
}

void string_builder_add_letter_line(const LetterDistribution *ld,
                                    InferenceResults *inference_results,
                                    inference_stat_t inference_stat_type,
                                    const Rack *rack, const Rack *bag_as_rack,
                                    StringBuilder *inference_string,
                                    Stat *letter_stat, uint8_t letter,
                                    int max_duplicate_letter_draw,
                                    int number_of_tiles_played_or_exchanged) {
  inference_results_set_stat_for_letter(inference_results, inference_stat_type,
                                        letter_stat, letter);
  string_builder_add_user_visible_letter(ld, inference_string, letter);
  string_builder_add_formatted_string(inference_string, ": %4.2f %4.2f",
                                      stat_get_mean(letter_stat),
                                      stat_get_stdev(letter_stat));

  for (int i = 1; i <= max_duplicate_letter_draw; i++) {
    string_builder_add_letter_minimum(
        inference_results, inference_stat_type, rack, bag_as_rack,
        inference_string, letter, i, number_of_tiles_played_or_exchanged);
  }
  string_builder_add_string(inference_string, "\n");
}

void string_builder_add_inference_type(
    InferenceResults *inference_results, inference_stat_t inference_stat_type,
    const LetterDistribution *ld, const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *inference_string, Stat *letter_stat,
    int number_of_tiles_played_or_exchanged) {

  Stat *equity_values = inference_results_get_equity_values(
      inference_results, inference_stat_type);

  uint64_t total_draws = stat_get_weight(equity_values);
  uint64_t total_leaves = stat_get_cardinality(equity_values);

  string_builder_add_formatted_string(
      inference_string,
      "Total possible leave draws:   %lu\nTotal possible unique leaves: "
      "%lu\nAverage leave value:          %0.2f\nStdev leave value:            "
      "%0.2f\n",
      total_draws, total_leaves, stat_get_mean(equity_values),
      stat_get_stdev(equity_values));
  int max_duplicate_letter_draw = 0;
  uint32_t ld_size = ld_get_size(ld);
  for (int letter = 0; letter < (int)ld_size; letter++) {
    for (int number_of_letter = 1; number_of_letter <= (RACK_SIZE);
         number_of_letter++) {
      int draws = inference_results_get_subtotal_sum_with_minimum(
          inference_results, inference_stat_type, letter, number_of_letter,
          INFERENCE_SUBTOTAL_DRAW);
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
    string_builder_add_formatted_string(
        inference_string, "Has at least %d of            ", i + 1);
  }
  string_builder_add_string(inference_string, "\n\n   Avg  Std ");

  for (int i = 0; i < max_duplicate_letter_draw; i++) {
    string_builder_add_string(inference_string,
                              " | Pct     Rand   Tot      Unq      ");
  }
  string_builder_add_string(inference_string, "\n");

  if (total_draws > 0) {
    for (int i = 0; i < (int)ld_size; i++) {
      string_builder_add_letter_line(ld, inference_results, inference_stat_type,
                                     rack, bag_as_rack, inference_string,
                                     letter_stat, i, max_duplicate_letter_draw,
                                     number_of_tiles_played_or_exchanged);
    }
  }
}

void string_builder_add_inference(const LetterDistribution *ld,
                                  InferenceResults *inference_results,
                                  const Rack *target_played_tiles,
                                  StringBuilder *inference_string) {
  int target_number_of_tiles_exchanged =
      inference_results_get_target_number_of_tiles_exchanged(inference_results);

  bool is_exchange = target_number_of_tiles_exchanged > 0;
  int number_of_tiles_played_or_exchanged;

  if (!is_exchange) {
    string_builder_add_string(inference_string, "Played tiles:          ");
    string_builder_add_rack(target_played_tiles, ld, inference_string);
    number_of_tiles_played_or_exchanged =
        rack_get_total_letters(target_played_tiles);
  } else {
    string_builder_add_formatted_string(inference_string,
                                        "Exchanged tiles:       %d",
                                        target_number_of_tiles_exchanged);
    number_of_tiles_played_or_exchanged = target_number_of_tiles_exchanged;
  }

  string_builder_add_formatted_string(
      inference_string, "\nScore:                 %d\n",
      inference_results_get_target_score(inference_results));

  Rack *target_unplayed_tiles =
      inference_results_get_target_known_unplayed_tiles(inference_results);
  if (rack_get_total_letters(target_unplayed_tiles) > 0) {
    string_builder_add_string(inference_string, "Partial Rack:          ");
    string_builder_add_rack(target_unplayed_tiles, ld, inference_string);
    string_builder_add_string(inference_string, "\n");
  }

  string_builder_add_formatted_string(
      inference_string, "Equity margin:         %0.2f\n",
      inference_results_get_equity_margin(inference_results));

  // Create a transient stat to use the stat functions
  Stat *letter_stat = stat_create();

  Rack *bag_as_rack = inference_results_get_bag_as_rack(inference_results);

  string_builder_add_inference_type(inference_results, INFERENCE_TYPE_LEAVE, ld,
                                    target_unplayed_tiles, bag_as_rack,
                                    inference_string, letter_stat,
                                    number_of_tiles_played_or_exchanged);
  inference_stat_t common_leaves_type = INFERENCE_TYPE_LEAVE;
  if (is_exchange) {
    common_leaves_type = INFERENCE_TYPE_RACK;
    string_builder_add_string(inference_string, "\n\nTiles Exchanged\n\n");
    Rack *unknown_exchange_rack =
        rack_create(rack_get_dist_size(target_unplayed_tiles));
    string_builder_add_inference_type(
        inference_results, INFERENCE_TYPE_EXCHANGED, ld, unknown_exchange_rack,
        bag_as_rack, inference_string, letter_stat,
        target_number_of_tiles_exchanged);
    rack_destroy(unknown_exchange_rack);
    string_builder_add_string(inference_string, "\n\nRack\n\n");
    string_builder_add_inference_type(inference_results, INFERENCE_TYPE_RACK,
                                      ld, target_unplayed_tiles, bag_as_rack,
                                      inference_string, letter_stat, 0);
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Exch    Pct    Draws\n");
  } else {
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Pct    Draws  Equity\n");
  }
  stat_destroy(letter_stat);

  const LeaveRackList *leave_rack_list =
      inference_results_get_leave_rack_list(inference_results);
  // Get the list of most common leaves
  int number_of_common_leaves = leave_rack_list_get_count(leave_rack_list);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    const LeaveRack *leave_rack =
        leave_rack_list_get_rack(leave_rack_list, common_leave_index);
    string_builder_add_leave_rack(
        leave_rack, ld, inference_string, common_leave_index,
        stat_get_weight(inference_results_get_equity_values(
            inference_results, common_leaves_type)));
  }
}

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control) {
  char *current_rack_info_string = get_formatted_string(
      "info infercurrrack %llu\n", (long long unsigned int)current_rack_index);
  thread_control_print(thread_control, current_rack_info_string);
  free(current_rack_info_string);
}

void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control) {
  char *total_racks_info_string =
      get_formatted_string("info infertotalracks %llu\n",
                           (long long unsigned int)total_racks_evaluated);
  thread_control_print(thread_control, total_racks_info_string);
  free(total_racks_info_string);
}

void string_builder_add_ucgi_leave_rack(const LeaveRack *leave_rack,
                                        const LetterDistribution *ld,
                                        StringBuilder *ucgi_string_builder,
                                        int index, uint64_t total_draws,
                                        bool is_exchange) {
  Rack *leave_rack_leave = leave_rack_get_leave(leave_rack);
  Rack *leave_rack_exchanged = leave_rack_get_exchanged(leave_rack);
  int draws = leave_rack_get_draws(leave_rack);
  double equity = leave_rack_get_equity(leave_rack);
  if (!is_exchange) {
    string_builder_add_rack(leave_rack_leave, ld, ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, " %-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)draws / total_draws) * 100, draws, equity);
  } else {
    string_builder_add_rack(leave_rack_leave, ld, ucgi_string_builder);
    string_builder_add_spaces(ucgi_string_builder, 1);
    string_builder_add_rack(leave_rack_exchanged, ld, ucgi_string_builder);
    string_builder_add_formatted_string(
        ucgi_string_builder, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)draws / total_draws) * 100, draws);
  }
}

void string_builder_ucgi_add_letter_minimum(
    InferenceResults *inference_results, inference_stat_t inference_stat_type,
    const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *ucgi_string_builder, uint8_t letter, int minimum,
    int number_of_tiles_played_or_exchanged) {
  int draw_subtotal = inference_results_get_subtotal_sum_with_minimum(
      inference_results, inference_stat_type, letter, minimum,
      INFERENCE_SUBTOTAL_DRAW);
  int leave_subtotal = inference_results_get_subtotal_sum_with_minimum(
      inference_results, inference_stat_type, letter, minimum,
      INFERENCE_SUBTOTAL_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) /
      (double)stat_get_weight(inference_results_get_equity_values(
          inference_results, inference_stat_type));
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  string_builder_add_formatted_string(
      ucgi_string_builder, " %f %f %d %d", inference_probability * 100,
      random_probability * 100, draw_subtotal, leave_subtotal);
}

void string_builder_ucgi_add_letter_line(
    const LetterDistribution *ld, InferenceResults *inference_results,
    inference_stat_t inference_stat_type, const Rack *rack,
    const Rack *bag_as_rack, StringBuilder *ucgi_string_builder,
    Stat *letter_stat, uint8_t letter, int number_of_tiles_played_or_exchanged,
    const char *inference_record_type) {

  inference_results_set_stat_for_letter(inference_results, inference_stat_type,
                                        letter_stat, letter);
  string_builder_add_formatted_string(ucgi_string_builder, "infertile %s ",
                                      inference_record_type, 0);
  string_builder_add_user_visible_letter(ld, ucgi_string_builder, letter);
  string_builder_add_formatted_string(ucgi_string_builder, " %f %f",
                                      stat_get_mean(letter_stat),
                                      stat_get_stdev(letter_stat));

  for (int i = 1; i <= (RACK_SIZE); i++) {
    string_builder_ucgi_add_letter_minimum(
        inference_results, inference_stat_type, rack, bag_as_rack,
        ucgi_string_builder, letter, i, number_of_tiles_played_or_exchanged);
  }
  string_builder_add_string(ucgi_string_builder, "\n");
}

void string_builder_ucgi_add_inference_record(
    InferenceResults *inference_results, inference_stat_t inference_stat_type,
    const LetterDistribution *ld, const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *ucgi_string_builder, Stat *letter_stat,
    int number_of_tiles_played_or_exchanged,
    const char *inference_record_type) {
  Stat *equity_values = inference_results_get_equity_values(
      inference_results, inference_stat_type);

  uint64_t total_draws = stat_get_weight(equity_values);
  uint64_t total_leaves = stat_get_cardinality(equity_values);

  string_builder_add_formatted_string(
      ucgi_string_builder,
      "infertotaldraws %s %llu\n"
      "inferuniqueleaves %s %llu\n"
      "inferleaveavg %s %f\n"
      "inferleavestdev %s %f\n",
      inference_record_type, total_draws, inference_record_type, total_leaves,
      inference_record_type, stat_get_mean(equity_values),
      inference_record_type, stat_get_stdev(equity_values));
  for (int i = 0; i < (int)ld_get_size(ld); i++) {
    string_builder_ucgi_add_letter_line(
        ld, inference_results, inference_stat_type, rack, bag_as_rack,
        ucgi_string_builder, letter_stat, i,
        number_of_tiles_played_or_exchanged, inference_record_type);
  }
}

void print_ucgi_inference(const LetterDistribution *ld,
                          InferenceResults *inference_results,
                          ThreadControl *thread_control) {
  bool is_exchange = inference_results_get_target_number_of_tiles_exchanged(
                         inference_results) > 0;
  int number_of_tiles_played_or_exchanged =
      inference_results_get_target_number_of_tiles_exchanged(inference_results);
  if (number_of_tiles_played_or_exchanged == 0) {
    number_of_tiles_played_or_exchanged = (RACK_SIZE)-rack_get_total_letters(
        inference_results_get_target_played_tiles(inference_results));
  }

  // Create a transient stat to use the stat functions
  Stat *letter_stat = stat_create();

  StringBuilder *ucgi_string_builder = string_builder_create();
  string_builder_ucgi_add_inference_record(
      inference_results, INFERENCE_TYPE_LEAVE, ld,
      inference_results_get_target_known_unplayed_tiles(inference_results),
      inference_results_get_bag_as_rack(inference_results), ucgi_string_builder,
      letter_stat, number_of_tiles_played_or_exchanged, "leave");
  inference_stat_t common_leaves_type = INFERENCE_TYPE_LEAVE;
  if (is_exchange) {
    common_leaves_type = INFERENCE_TYPE_RACK;
    Rack *unknown_exchange_rack = rack_create(rack_get_dist_size(
        inference_results_get_target_known_unplayed_tiles(inference_results)));
    string_builder_ucgi_add_inference_record(
        inference_results, INFERENCE_TYPE_EXCHANGED, ld, unknown_exchange_rack,
        inference_results_get_bag_as_rack(inference_results),
        ucgi_string_builder, letter_stat,
        inference_results_get_target_number_of_tiles_exchanged(
            inference_results),
        "exch");
    rack_destroy(unknown_exchange_rack);
    string_builder_ucgi_add_inference_record(
        inference_results, INFERENCE_TYPE_RACK, ld,
        inference_results_get_target_known_unplayed_tiles(inference_results),
        inference_results_get_bag_as_rack(inference_results),
        ucgi_string_builder, letter_stat, 0, "rack");
  }
  stat_destroy(letter_stat);

  // Get the list of most common leaves
  const LeaveRackList *leave_rack_list =
      inference_results_get_leave_rack_list(inference_results);
  int number_of_common_leaves = leave_rack_list_get_count(leave_rack_list);
  Stat *common_leave_equity_values = inference_results_get_equity_values(
      inference_results, common_leaves_type);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    const LeaveRack *leave_rack =
        leave_rack_list_get_rack(leave_rack_list, common_leave_index);
    string_builder_add_ucgi_leave_rack(
        leave_rack, ld, ucgi_string_builder, common_leave_index,
        stat_get_weight(common_leave_equity_values), is_exchange);
  }
  thread_control_print(thread_control,
                       string_builder_peek(ucgi_string_builder));
  string_builder_destroy(ucgi_string_builder);
}
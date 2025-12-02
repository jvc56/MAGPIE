#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/equity.h"
#include "../ent/inference_results.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include "rack_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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

void string_builder_add_letter_row(
    const LetterDistribution *ld, InferenceResults *inference_results,
    inference_stat_t inference_stat_type, const Rack *rack,
    const Rack *bag_as_rack, Stat *letter_stat, MachineLetter letter,
    int max_duplicate_letter_draw, int number_of_tiles_played_or_exchanged,
    StringGrid *sg, int curr_row, StringBuilder *tmp_sb) {
  inference_results_set_stat_for_letter(inference_results, inference_stat_type,
                                        letter_stat, letter);

  int curr_col = 0;

  string_builder_add_user_visible_letter(tmp_sb, ld, letter);
  string_grid_set_cell(sg, curr_row, curr_col++,
                       string_builder_dump(tmp_sb, NULL));
  string_builder_clear(tmp_sb);

  string_grid_set_cell(
      sg, curr_row, curr_col++,
      get_formatted_string("%.2f", stat_get_mean(letter_stat)));

  string_grid_set_cell(
      sg, curr_row, curr_col++,
      get_formatted_string("%.2f", stat_get_stdev(letter_stat)));

  for (int current_min = 1; current_min <= max_duplicate_letter_draw;
       current_min++) {
    const Stat *equity_values = inference_results_get_equity_values(
        inference_results, inference_stat_type);
    uint64_t draw_subtotal = inference_results_get_subtotal_sum_with_minimum(
        inference_results, inference_stat_type, letter, current_min,
        INFERENCE_SUBTOTAL_DRAW);
    uint64_t leave_subtotal = inference_results_get_subtotal_sum_with_minimum(
        inference_results, inference_stat_type, letter, current_min,
        INFERENCE_SUBTOTAL_LEAVE);
    double inference_probability =
        ((double)draw_subtotal) / (double)stat_get_num_samples(equity_values);
    double random_probability = get_probability_for_random_minimum_draw(
        bag_as_rack, rack, letter, current_min,
        number_of_tiles_played_or_exchanged);
    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f", inference_probability * 100));
    string_grid_set_cell(
        sg, curr_row, curr_col++,
        get_formatted_string("%.2f", random_probability * 100));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%lu", draw_subtotal));
    string_grid_set_cell(sg, curr_row, curr_col++,
                         get_formatted_string("%lu", leave_subtotal));
  }
}

void string_builder_add_inference_type(
    StringBuilder *inference_string, InferenceResults *inference_results,
    inference_stat_t inference_stat_type, const LetterDistribution *ld,
    const Rack *rack, const Rack *bag_as_rack, Stat *letter_stat,
    int number_of_tiles_played_or_exchanged, bool use_ucgi_format,
    StringBuilder *tmp_sb) {

  const Stat *equity_values = inference_results_get_equity_values(
      inference_results, inference_stat_type);
  uint64_t total_draws = stat_get_num_samples(equity_values);

  if (total_draws == 0) {
    string_builder_add_string(inference_string,
                              "No possible target racks were found within the "
                              "given equity margin.\n");
    return;
  }

  uint64_t total_leaves = stat_get_num_unique_samples(equity_values);

  StringGrid *sg_summary = string_grid_create(4, 2, 1);
  string_grid_set_cell(sg_summary, 0, 0,
                       string_duplicate("Total target racks:"));
  string_grid_set_cell(sg_summary, 0, 1,
                       get_formatted_string("%lu", (unsigned long)total_draws));
  string_grid_set_cell(sg_summary, 1, 0,
                       string_duplicate("Total unique target racks:"));
  string_grid_set_cell(
      sg_summary, 1, 1,
      get_formatted_string("%lu", (unsigned long)total_leaves));
  string_grid_set_cell(sg_summary, 2, 0,
                       string_duplicate("Average leave equity:"));
  string_grid_set_cell(
      sg_summary, 2, 1,
      get_formatted_string("%0.2f", stat_get_mean(equity_values)));
  string_grid_set_cell(sg_summary, 3, 0,
                       string_duplicate("Stdev leave equity:"));
  string_grid_set_cell(
      sg_summary, 3, 1,
      get_formatted_string("%0.2f", stat_get_stdev(equity_values)));

  string_builder_add_string_grid(inference_string, sg_summary, false);
  string_builder_add_string(inference_string, "\n");
  string_grid_destroy(sg_summary);

  int max_duplicate_letter_draw = 0;
  uint32_t ld_size = ld_get_size(ld);
  for (int letter = 0; letter < (int)ld_size; letter++) {
    for (int number_of_letter = 1; number_of_letter <= (RACK_SIZE);
         number_of_letter++) {
      const uint64_t draws = inference_results_get_subtotal_sum_with_minimum(
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

  int num_rows = (int)ld_size;
  if (!use_ucgi_format) {
    num_rows += 2;
  }
  const int num_cols = 3 + max_duplicate_letter_draw * 4;

  StringGrid *sg_content = string_grid_create(num_rows, num_cols, 2);

  int curr_row = 0;

  if (!use_ucgi_format) {
    for (int i = 0; i < max_duplicate_letter_draw; i++) {
      string_grid_set_cell(sg_content, curr_row, 3 + i * 4,
                           get_formatted_string("%d", i + 1));
    }

    curr_row++;

    int curr_col = 0;
    // The 'leave' column can be blank
    curr_col++;

    string_grid_set_cell(sg_content, curr_row, curr_col++,
                         string_duplicate("Avg"));
    string_grid_set_cell(sg_content, curr_row, curr_col++,
                         string_duplicate("Std"));

    for (int i = 0; i < max_duplicate_letter_draw; i++) {
      string_grid_set_cell(sg_content, curr_row, curr_col++,
                           string_duplicate("Pct"));
      string_grid_set_cell(sg_content, curr_row, curr_col++,
                           string_duplicate("Rand"));
      string_grid_set_cell(sg_content, curr_row, curr_col++,
                           string_duplicate("Tot"));
      string_grid_set_cell(sg_content, curr_row, curr_col++,
                           string_duplicate("Unq"));
    }
    curr_row++;
  }

  for (int i = 0; i < (int)ld_size; i++) {
    string_builder_add_letter_row(
        ld, inference_results, inference_stat_type, rack, bag_as_rack,
        letter_stat, i, max_duplicate_letter_draw,
        number_of_tiles_played_or_exchanged, sg_content, i + curr_row, tmp_sb);
  }
  string_builder_add_string_grid(inference_string, sg_content, false);
  string_grid_destroy(sg_content);
}

void string_builder_add_inference_description(
    StringBuilder *inference_string, const LetterDistribution *ld,
    InferenceResults *inference_results, StringBuilder *tmp_sb) {
  int target_number_of_tiles_exchanged =
      inference_results_get_target_number_of_tiles_exchanged(inference_results);
  bool is_exchange = target_number_of_tiles_exchanged > 0;
  const Rack *target_played_tiles =
      inference_results_get_target_played_tiles(inference_results);
  const Rack *target_unplayed_tiles =
      inference_results_get_target_known_unplayed_tiles(inference_results);
  const int target_num_unplayed_tiles =
      rack_get_total_letters(target_unplayed_tiles);

  int num_info_rows = 3;
  if (target_num_unplayed_tiles > 0) {
    num_info_rows++;
  }

  StringGrid *sg = string_grid_create(num_info_rows, 2, 1);
  if (!is_exchange) {
    string_builder_add_rack(tmp_sb, target_played_tiles, ld, false);
    string_grid_set_cell(sg, 0, 0, string_duplicate("Played tiles:"));
    string_grid_set_cell(sg, 0, 1, string_builder_dump(tmp_sb, NULL));
    string_builder_clear(tmp_sb);
  } else {
    string_grid_set_cell(sg, 0, 0, string_duplicate("Exchanged tiles:"));
    string_grid_set_cell(
        sg, 0, 1, get_formatted_string("%d", target_number_of_tiles_exchanged));
  }

  string_grid_set_cell(sg, 1, 0, string_duplicate("Score:"));
  string_grid_set_cell(
      sg, 1, 1,
      get_formatted_string(
          "%d", equity_to_int(
                    inference_results_get_target_score(inference_results))));

  string_grid_set_cell(sg, 2, 0, string_duplicate("Equity margin:"));
  string_grid_set_cell(
      sg, 2, 1,
      get_formatted_string("%0.3f",
                           equity_to_double(inference_results_get_equity_margin(
                               inference_results))));

  if (target_num_unplayed_tiles > 0) {
    string_builder_add_rack(tmp_sb, target_unplayed_tiles, ld, false);
    string_grid_set_cell(sg, 3, 0, string_duplicate("Partial rack:"));
    string_grid_set_cell(sg, 3, 1, string_builder_dump(tmp_sb, NULL));
    string_builder_clear(tmp_sb);
  }

  string_builder_add_string_grid(inference_string, sg, false);
  string_builder_add_string(inference_string, "\n");
  string_grid_destroy(sg);
}

void string_builder_add_inference(StringBuilder *inference_string,
                                  InferenceResults *inference_results,
                                  const LetterDistribution *ld,
                                  int max_num_leaves_to_display,
                                  bool use_ucgi_format) {
  StringBuilder *tmp_sb = string_builder_create();

  if (!use_ucgi_format) {
    string_builder_add_inference_description(inference_string, ld,
                                             inference_results, tmp_sb);
  }

  int target_number_of_tiles_exchanged =
      inference_results_get_target_number_of_tiles_exchanged(inference_results);
  const Rack *target_played_tiles =
      inference_results_get_target_played_tiles(inference_results);
  const Rack *target_unplayed_tiles =
      inference_results_get_target_known_unplayed_tiles(inference_results);
  int number_of_tiles_played_or_exchanged =
      rack_get_total_letters(target_played_tiles);
  bool is_exchange = false;
  if (target_number_of_tiles_exchanged > 0) {
    number_of_tiles_played_or_exchanged = target_number_of_tiles_exchanged;
    is_exchange = true;
  }

  // Create a transient stat to use the stat functions
  Stat *letter_stat = stat_create(false);

  const Rack *bag_as_rack =
      inference_results_get_bag_as_rack(inference_results);

  string_builder_add_inference_type(
      inference_string, inference_results, INFERENCE_TYPE_LEAVE, ld,
      target_unplayed_tiles, bag_as_rack, letter_stat,
      number_of_tiles_played_or_exchanged, use_ucgi_format, tmp_sb);
  inference_stat_t common_leaves_type = INFERENCE_TYPE_LEAVE;
  if (is_exchange) {
    common_leaves_type = INFERENCE_TYPE_RACK;
    string_builder_add_string(inference_string, "\n\nTiles Exchanged\n\n");
    Rack unknown_exchange_rack;
    rack_set_dist_size_and_reset(&unknown_exchange_rack, ld_get_size(ld));
    string_builder_add_inference_type(
        inference_string, inference_results, INFERENCE_TYPE_EXCHANGED, ld,
        &unknown_exchange_rack, bag_as_rack, letter_stat,
        number_of_tiles_played_or_exchanged, use_ucgi_format, tmp_sb);
    string_builder_add_string(inference_string, "\n\nRack\n\n");
    string_builder_add_inference_type(inference_string, inference_results,
                                      INFERENCE_TYPE_RACK, ld,
                                      target_unplayed_tiles, bag_as_rack,
                                      letter_stat, 0, use_ucgi_format, tmp_sb);
  }
  stat_destroy(letter_stat);
  string_builder_add_string(inference_string, "\n");

  const LeaveRackList *leave_rack_list =
      inference_results_get_leave_rack_list(inference_results);
  // Get the list of most common leaves
  int num_leaves_to_display = leave_rack_list_get_count(leave_rack_list);
  if (num_leaves_to_display > max_num_leaves_to_display) {
    num_leaves_to_display = max_num_leaves_to_display;
  }
  StringGrid *sg_common_leaves =
      string_grid_create(num_leaves_to_display + 1, 5, 2);
  int curr_row = 0;
  int curr_col = 0;
  // The first column which designates the rank can be blank
  // The second column which represents the leave can also be blank
  curr_col += 2;
  if (!use_ucgi_format) {
    if (is_exchange) {
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("Ex"));
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("Pct"));
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("Tot"));
    } else {
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("Pct"));
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("To"));
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_duplicate("Eq"));
    }
  }

  curr_row++;

  for (int common_leave_index = 0; common_leave_index < num_leaves_to_display;
       common_leave_index++) {
    const LeaveRack *leave_rack =
        leave_rack_list_get_rack(leave_rack_list, common_leave_index);
    const int ld_size = ld_get_size(ld);
    Rack leave_rack_leave;
    rack_set_dist_size(&leave_rack_leave, ld_size);
    leave_rack_get_leave(leave_rack, &leave_rack_leave);
    const int leave_rack_draws = leave_rack_get_draws(leave_rack);
    const uint64_t total_draws =
        stat_get_num_samples(inference_results_get_equity_values(
            inference_results, common_leaves_type));

    curr_col = 0;

    // Rank
    string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                         get_formatted_string("%d", common_leave_index + 1));

    // Leave
    string_builder_add_rack(tmp_sb, &leave_rack_leave, ld, false);
    string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                         string_builder_dump(tmp_sb, NULL));
    string_builder_clear(tmp_sb);

    if (is_exchange) {
      // Exch
      Rack leave_rack_exchanged;
      rack_set_dist_size(&leave_rack_exchanged, ld_size);
      leave_rack_get_exchanged(leave_rack, &leave_rack_exchanged);
      string_builder_add_rack(tmp_sb, &leave_rack_exchanged, ld, false);
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);

      // Pct
      string_grid_set_cell(
          sg_common_leaves, curr_row, curr_col++,
          get_formatted_string(
              "%.2f", ((double)leave_rack_draws / (double)total_draws) * 100));
      // Total
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           get_formatted_string("%d", leave_rack_draws));
    } else {
      // Pct
      string_grid_set_cell(
          sg_common_leaves, curr_row, curr_col++,
          get_formatted_string(
              "%.2f", ((double)leave_rack_draws / (double)total_draws) * 100));

      // Total
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           get_formatted_string("%d", leave_rack_draws));

      // Equity
      const double leave_rack_equity =
          equity_to_double(leave_rack_get_equity(leave_rack));
      string_grid_set_cell(sg_common_leaves, curr_row, curr_col++,
                           get_formatted_string("%.2f", leave_rack_equity));
    }
    curr_row++;
  }
  string_builder_add_string_grid(inference_string, sg_common_leaves, false);
  string_builder_add_string(inference_string, "\n");
  string_builder_destroy(tmp_sb);
  string_grid_destroy(sg_common_leaves);
}

char *inference_result_get_string(InferenceResults *inference_results,
                                  const LetterDistribution *ld,
                                  int max_num_leaves_to_display,
                                  bool use_ucgi_format) {
  StringBuilder *inference_string = string_builder_create();
  string_builder_add_inference(inference_string, inference_results, ld,
                               max_num_leaves_to_display, use_ucgi_format);
  char *result_string = string_builder_dump(inference_string, NULL);
  string_builder_destroy(inference_string);
  return result_string;
}
#include "../def/inference_defs.h"
#include "../def/rack_defs.h"

#include "string_util.h"

#include "../ent/game.h"
#include "../ent/inference.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/movegen.h"
#include "../ent/rack.h"
#include "../ent/stats.h"

void string_builder_add_leave_rack(
    const LeaveRack *leave_rack, const LetterDistribution *letter_distribution,
    StringBuilder *inference_string, int index, uint64_t total_draws) {

  Rack *leave_rack_leave = leave_rack_get_leave(leave_rack);
  Rack *leave_rack_exchanged = leave_rack_get_exchanged(leave_rack);
  int leave_rack_draws = leave_rack_get_draws(leave_rack);
  double leave_rack_equity = leave_rack_get_equity(leave_rack);

  if (rack_is_empty(leave_rack_exchanged)) {
    string_builder_add_rack(leave_rack_leave, letter_distribution,
                            inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d %0.2f\n", index + 1,
        ((double)leave_rack_draws / total_draws) * 100, leave_rack_draws,
        leave_rack_equity);
  } else {
    string_builder_add_rack(leave_rack_leave, letter_distribution,
                            inference_string);
    string_builder_add_spaces(inference_string, 1);
    string_builder_add_rack(leave_rack_exchanged, letter_distribution,
                            inference_string);
    string_builder_add_formatted_string(
        inference_string, "%-3d %-6.2f %-6d\n", index + 1,
        ((double)leave_rack_draws / total_draws) * 100, leave_rack_draws);
  }
}

void string_builder_add_letter_minimum(
    const InferenceRecord *record, const Rack *rack, const Rack *bag_as_rack,
    StringBuilder *inference_string, uint8_t letter, int minimum,
    int number_of_tiles_played_or_exchanged) {

  Stat *record_equity_values = inference_record_get_equity_values(record);

  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)get_weight(record_equity_values);
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
  string_builder_add_user_visible_letter(gen_get_ld(game_get_gen(game)),
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

  Stat *record_equity_values = inference_record_get_equity_values(record);

  uint64_t total_draws = get_weight(record_equity_values);
  uint64_t total_leaves = get_cardinality(record_equity_values);

  string_builder_add_formatted_string(
      inference_string,
      "Total possible leave draws:   %lu\nTotal possible unique leaves: "
      "%lu\nAverage leave value:          %0.2fStdev leave value:            "
      "%0.2f",
      total_draws, total_leaves, get_mean(record_equity_values),
      get_stdev(record_equity_values));
  int max_duplicate_letter_draw = 0;
  uint32_t ld_size =
      letter_distribution_get_size(gen_get_ld(game_get_gen(game)));
  for (int letter = 0; letter < (int)ld_size; letter++) {
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
    for (int i = 0; i < (int)ld_size; i++) {
      string_builder_add_letter_line(
          game, record, rack, bag_as_rack, inference_string, letter_stat, i,
          max_duplicate_letter_draw, number_of_tiles_played_or_exchanged);
    }
  }
}

void string_builder_add_inference(const Inference *inference,
                                  const Rack *actual_tiles_played,
                                  StringBuilder *inference_string) {
  int number_of_tiles_exchanged =
      inference_get_number_of_tiles_exchanged(inference);
  const Game *game = inference_get_game(inference);

  bool is_exchange = number_of_tiles_exchanged > 0;
  int number_of_tiles_played_or_exchanged;

  LetterDistribution *ld = gen_get_ld(game_get_gen(game));

  string_builder_add_game(game, inference_string);

  if (!is_exchange) {
    string_builder_add_string(inference_string, "Played tiles:          ");
    string_builder_add_rack(actual_tiles_played, ld, inference_string);
    number_of_tiles_played_or_exchanged =
        get_number_of_letters(actual_tiles_played);
  } else {
    string_builder_add_formatted_string(inference_string,
                                        "Exchanged tiles:       %d",
                                        number_of_tiles_exchanged);
    number_of_tiles_played_or_exchanged = number_of_tiles_exchanged;
  }

  string_builder_add_formatted_string(inference_string,
                                      "\nScore:                 %d\n",
                                      inference_get_actual_score(inference));

  Rack *player_to_infer_rack = inference_get_player_to_infer_rack(inference);
  if (get_number_of_letters(player_to_infer_rack) > 0) {
    string_builder_add_string(inference_string, "Partial Rack:          ");
    string_builder_add_rack(player_to_infer_rack, ld, inference_string);
    string_builder_add_string(inference_string, "\n");
  }

  string_builder_add_formatted_string(inference_string,
                                      "Equity margin:         %0.2f\n",
                                      inference_get_equity_margin(inference));

  // Create a transient stat to use the stat functions
  Stat *letter_stat = create_stat();

  InferenceRecord *leave_record = inference_get_leave_record(inference);
  InferenceRecord *rack_record = inference_get_rack_record(inference);
  InferenceRecord *exchanged_record = inference_get_exchanged_record(inference);
  Rack *inference_leave = inference_get_leave(inference);
  Rack *bag_as_rack = inference_get_bag_as_rack(inference);

  string_builder_add_inference_record(
      leave_record, game, inference_leave, bag_as_rack, inference_string,
      letter_stat, number_of_tiles_played_or_exchanged);
  const InferenceRecord *common_leaves_record = leave_record;
  if (is_exchange) {
    common_leaves_record = rack_record;
    string_builder_add_string(inference_string, "\n\nTiles Exchanged\n\n");
    Rack *unknown_exchange_rack = create_rack(get_array_size(inference_leave));
    string_builder_add_inference_record(
        exchanged_record, game, unknown_exchange_rack, bag_as_rack,
        inference_string, letter_stat, number_of_tiles_exchanged);
    destroy_rack(unknown_exchange_rack);
    string_builder_add_string(inference_string, "\n\nRack\n\n");
    string_builder_add_inference_record(rack_record, game, inference_leave,
                                        bag_as_rack, inference_string,
                                        letter_stat, 0);
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Exch    Pct    Draws\n");
  } else {
    string_builder_add_string(
        inference_string,
        "\nMost Common       \n\n#   Leave   Pct    Draws  Equity\n");
  }
  destroy_stat(letter_stat);

  LeaveRackList *leave_rack_list = inference_get_leave_rack_list(inference);
  // Get the list of most common leaves
  int number_of_common_leaves = get_leave_rack_list_count(leave_rack_list);
  sort_leave_racks(leave_rack_list);
  for (int common_leave_index = 0; common_leave_index < number_of_common_leaves;
       common_leave_index++) {
    LeaveRack *leave_rack = get_leave_rack(leave_rack_list, common_leave_index);
    string_builder_add_leave_rack(
        leave_rack, ld, inference_string, common_leave_index,
        get_weight(inference_record_get_equity_values(common_leaves_record)));
  }
}
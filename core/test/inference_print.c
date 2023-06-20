#include <stdio.h>
#include <string.h>

#include "../src/infer.h"
#include "../src/rack.h"

#include "game_print.h"
#include "inference_print.h"
#include "test_util.h"

void write_leave_rack(char *buffer, LeaveRack *leave_rack, int index,
                      uint64_t total_draws,
                      LetterDistribution *letter_distribution) {
  char leave_string[(RACK_SIZE)] = "";
  write_rack_to_end_of_buffer(leave_string, letter_distribution,
                              leave_rack->leave);
  if (leave_rack->exchanged->empty) {
    sprintf(buffer + strlen(buffer), "%-3d %-7s %-6.2f %-6d %0.2f\n", index + 1,
            leave_string, ((double)leave_rack->draws / total_draws) * 100,
            leave_rack->draws, leave_rack->equity);
  } else {
    char exchanged_string[(RACK_SIZE)] = "";
    write_rack_to_end_of_buffer(exchanged_string, letter_distribution,
                                leave_rack->exchanged);
    sprintf(buffer + strlen(buffer), "%-3d %-7s %-7s %-6.2f %-6d\n", index + 1,
            leave_string, exchanged_string,
            ((double)leave_rack->draws / total_draws) * 100, leave_rack->draws);
  }
}

void write_letter_minimum(InferenceRecord *record, Rack *rack,
                          Rack *bag_as_rack, char *inference_string,
                          uint8_t letter, int minimum,
                          int number_of_tiles_played_or_exchanged) {
  int draw_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
  int leave_subtotal = get_subtotal_sum_with_minimum(
      record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
  double inference_probability =
      ((double)draw_subtotal) / (double)get_weight(record->equity_values);
  double random_probability = get_probability_for_random_minimum_draw(
      bag_as_rack, rack, letter, minimum, number_of_tiles_played_or_exchanged);
  sprintf(inference_string + strlen(inference_string),
          " | %-7.2f %-7.2f%-9d%-9d", inference_probability * 100,
          random_probability * 100, draw_subtotal, leave_subtotal);
}

void write_letter_line(Game *game, InferenceRecord *record, Rack *rack,
                       Rack *bag_as_rack, Stat *letter_stat,
                       char *inference_string, uint8_t letter,
                       int max_duplicate_letter_draw,
                       int number_of_tiles_played_or_exchanged) {
  get_stat_for_letter(record, letter_stat, letter);
  sprintf(inference_string + strlen(inference_string), "%c: %4.2f %4.2f",
          machine_letter_to_human_readable_letter(
              game->gen->letter_distribution, letter),
          get_mean(letter_stat), get_stdev(letter_stat));

  for (int i = 1; i <= max_duplicate_letter_draw; i++) {
    write_letter_minimum(record, rack, bag_as_rack, inference_string, letter, i,
                         number_of_tiles_played_or_exchanged);
  }
  write_string_to_end_of_buffer(inference_string, "\n");
}

void write_inference_record(char *buffer, InferenceRecord *record, Game *game,
                            Rack *rack, Rack *bag_as_rack, Stat *letter_stat,
                            int number_of_tiles_played_or_exchanged,
                            int number_of_threads) {
  uint64_t total_draws = get_weight(record->equity_values);
  uint64_t total_leaves = get_cardinality(record->equity_values);
  sprintf(buffer + strlen(buffer), "Total possible leave draws:   %ld\n",
          total_draws);
  sprintf(buffer + strlen(buffer), "Total possible unique leaves: %ld\n",
          total_leaves);
  sprintf(buffer + strlen(buffer), "Average leave value:          %0.2f\n",
          get_mean(record->equity_values));
  if (number_of_threads == 1) {
    sprintf(buffer + strlen(buffer), "Stdev leave value:            %0.2f\n\n",
            get_stdev(record->equity_values));
  } else {
    sprintf(buffer + strlen(buffer), "Stdev leave value:            %0.2f\n\n",
            get_estimated_stdev_for_record(record));
  }

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

  sprintf(buffer + strlen(buffer), "               ");
  for (int i = 0; i < max_duplicate_letter_draw; i++) {
    sprintf(buffer + strlen(buffer), "Has at least %d of                   ",
            i + 1);
  }
  sprintf(buffer + strlen(buffer), "\n");
  sprintf(buffer + strlen(buffer), "\n");

  sprintf(buffer + strlen(buffer), "   Avg  Std ");

  for (int i = 0; i < max_duplicate_letter_draw; i++) {
    sprintf(buffer + strlen(buffer), " | Pct     Rand   Tot      Unq      ");
  }
  sprintf(buffer + strlen(buffer), "\n");

  if (total_draws > 0) {
    for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
      write_letter_line(game, record, rack, bag_as_rack, letter_stat, buffer, i,
                        max_duplicate_letter_draw,
                        number_of_tiles_played_or_exchanged);
    }
  }
}

void print_inference(Inference *inference, Rack *actual_tiles_played,
                     int number_of_threads) {
  char inference_string[50000] = "";

  int is_exchange = inference->number_of_tiles_exchanged > 0;
  int number_of_tiles_played_or_exchanged;
  Game *game = inference->game;
  if (!is_exchange) {
    write_string_to_end_of_buffer(inference_string, "Played tiles:          ");
    write_rack_to_end_of_buffer(inference_string,
                                inference->game->gen->letter_distribution,
                                actual_tiles_played);
    number_of_tiles_played_or_exchanged =
        actual_tiles_played->number_of_letters;
  } else {
    write_string_to_end_of_buffer(inference_string, "Exchanged tiles:       ");
    write_int_to_end_of_buffer(inference_string,
                               inference->number_of_tiles_exchanged);
    number_of_tiles_played_or_exchanged = inference->number_of_tiles_exchanged;
  }
  sprintf(inference_string + strlen(inference_string), "\n");
  sprintf(inference_string + strlen(inference_string),
          "Score:                 %d\n", inference->actual_score);
  if (inference->player_to_infer_rack->number_of_letters > 0) {
    write_string_to_end_of_buffer(inference_string, "Partial Rack:          ");
    write_rack_to_end_of_buffer(inference_string,
                                inference->game->gen->letter_distribution,
                                inference->player_to_infer_rack);
    sprintf(inference_string + strlen(inference_string), "\n");
  }
  sprintf(inference_string + strlen(inference_string),
          "Equity margin:         %0.2f\n", inference->equity_margin);

  // Create a transient stat to use the stat functions
  Stat *letter_stat = create_stat();

  char records_string[500000] = "";
  write_inference_record(records_string, inference->leave_record, game,
                         inference->leave, inference->bag_as_rack, letter_stat,
                         number_of_tiles_played_or_exchanged,
                         number_of_threads);
  InferenceRecord *common_leaves_record = inference->leave_record;
  if (is_exchange) {
    common_leaves_record = inference->rack_record;
    sprintf(records_string + strlen(records_string), "\n\nTiles Exchanged\n\n");
    Rack * unknown_exchange_rack = create_rack(inference->leave->array_size);
    write_inference_record(
        records_string, inference->exchanged_record, game, unknown_exchange_rack,
        inference->bag_as_rack, letter_stat,
        inference->number_of_tiles_exchanged, number_of_threads);
    destroy_rack(unknown_exchange_rack);
    sprintf(records_string + strlen(records_string), "\n\nRack\n\n");
    write_inference_record(records_string, inference->rack_record, game,
                           inference->leave, inference->bag_as_rack,
                           letter_stat, 0, number_of_threads);
    sprintf(records_string + strlen(records_string),
            "\nMost Common       \n\n#   Leave   Exch    Pct    Draws\n");
  } else {
    sprintf(records_string + strlen(records_string),
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
    write_leave_rack(records_string, leave_rack, common_leave_index,
                     get_weight(common_leaves_record->equity_values),
                     game->gen->letter_distribution);
  }

  print_game(inference->game);
  printf("\n%s\n", inference_string);
  printf("\n%s\n", records_string);
}
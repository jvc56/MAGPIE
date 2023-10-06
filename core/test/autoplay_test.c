#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/autoplay.h"
#include "../src/config.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/infer.h"
#include "../src/thread_control.h"

#include "superconfig.h"
#include "test_util.h"

int are_stats_equal(Stat *stat_1, Stat *stat_2) {
  if (stat_1 == stat_2) {
    return 1; // Pointers are the same, this should never be true
  }

  if (!stat_1 || !stat_2) {
    return 2; // One of the stats is NULL
  }

  if (!(stat_1->cardinality == stat_2->cardinality &&
        stat_1->weight == stat_2->weight &&
        fabs(stat_1->mean - stat_2->mean) < 0.000000001)) {
    return 3;
  }
  return 0;
}

int are_inference_records_equal(InferenceRecord *record_1,
                                InferenceRecord *record_2,
                                int draw_and_leave_subtotals_size) {
  if (record_1 == record_2) {
    return 4; // Pointers are the same, the records are equal
  }

  if (!record_1 || !record_2) {
    return 5; // One of the records is NULL
  }

  int are_stats_equal_value =
      are_stats_equal(record_1->equity_values, record_2->equity_values);

  if (are_stats_equal_value != 0) {
    return are_stats_equal_value;
  }

  for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
    if (record_1->draw_and_leave_subtotals[i] !=
        record_2->draw_and_leave_subtotals[i]) {
      return 6;
    }
  }
  return 0;
}

int are_leave_racks_equal(LeaveRack *rack_1, LeaveRack *rack_2) {
  if (rack_1 == rack_2) {
    return 8; // Pointers are the same, this should never happen
  }

  if (!rack_1 || !rack_2) {
    return 9; // One of the racks is NULL
  }

  if (rack_1->draws != rack_2->draws) {
    return 10;
  }
  return 0;
}

int are_leave_rack_lists_equal(LeaveRackList *list_1, LeaveRackList *list_2,
                               int number_of_listed_racks) {
  if (list_1 == list_2) {
    return 11; // Pointers are the same, the lists are equal
  }

  if (!list_1 || !list_2) {
    return 12; // One of the lists is NULL
  }

  // Check each pointer
  for (int i = 0; i < number_of_listed_racks; i++) {
    int are_leave_racks_equal_value =
        are_leave_racks_equal(list_1->leave_racks[i], list_2->leave_racks[i]);
    if (are_leave_racks_equal_value != 0) {
      return are_leave_racks_equal_value + 1000 * i;
    }
  }

  return 0;
}

int are_inferences_equal(Inference *inference_1, Inference *inference_2) {
  if (inference_1 == inference_2) {
    return 14; // Pointers are the same, the inferences are equal
  }

  if (!inference_1 || !inference_2) {
    return 15; // One of the inferences is NULL
  }

  if (inference_1->number_of_tiles_exchanged !=
      inference_2->number_of_tiles_exchanged) {
    return 16;
  }

  if (inference_1->draw_and_leave_subtotals_size !=
      inference_2->draw_and_leave_subtotals_size) {
    return 17;
  }

  int leave_records_equal_value = are_inference_records_equal(
      inference_1->leave_record, inference_2->leave_record,
      inference_1->draw_and_leave_subtotals_size);
  if (leave_records_equal_value != 0) {
    return leave_records_equal_value + 20;
  }

  if (inference_1->number_of_tiles_exchanged > 0) {
    int exchanged_records_equal_value = are_inference_records_equal(
        inference_1->exchanged_record, inference_2->exchanged_record,
        inference_1->draw_and_leave_subtotals_size);
    if (exchanged_records_equal_value != 0) {
      return exchanged_records_equal_value + 40;
    }
    int rack_records_equal_value = are_inference_records_equal(
        inference_1->rack_record, inference_2->rack_record,
        inference_1->draw_and_leave_subtotals_size);
    if (rack_records_equal_value != 0) {
      return rack_records_equal_value + 60;
    }
  }

  uint64_t number_of_listed_racks = inference_1->leave_rack_list->capacity;
  if (inference_1->exchanged == 0 &&
      get_cardinality(inference_1->leave_record->equity_values) <
          number_of_listed_racks) {
    number_of_listed_racks =
        get_cardinality(inference_1->leave_record->equity_values) <
        number_of_listed_racks;
  } else if (inference_1->exchanged != 0 &&
             get_cardinality(inference_1->rack_record->equity_values) <
                 number_of_listed_racks) {
    number_of_listed_racks =
        get_cardinality(inference_1->rack_record->equity_values);
  }
  return are_leave_rack_lists_equal(inference_1->leave_rack_list,
                                    inference_2->leave_rack_list,
                                    number_of_listed_racks);
}

void print_error_case(Game *game, Inference *inference_1,
                      Inference *inference_2, Rack *tiles_played,
                      int player_on_turn_index, int score,
                      int number_of_tiles_exchanged, int number_of_threads) {
  print_game(game);
  printf("pindex: %d\n", player_on_turn_index);
  printf("score: %d\n", score);
  printf("exch: %d\n", number_of_tiles_exchanged);
  printf("threads: %d\n", number_of_threads);
  printf("inf 1\n");
  print_inference(inference_1, tiles_played);
  printf("inf 2\n");
  print_inference(inference_2, tiles_played);
}

void play_game_test(ThreadControl *thread_control, Game *game,
                    Inference *inference_1, Inference *inference_2,
                    Rack *tiles_played, Rack *full_rack, time_t seed,
                    int test_inference) {
  draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, RACK_SIZE);
  draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, RACK_SIZE);
  while (!game->game_end_reason) {
    generate_moves(game->gen, game->players[game->player_on_turn_index],
                   game->players[1 - game->player_on_turn_index]->rack,
                   game->gen->bag->last_tile_index + 1 >= RACK_SIZE);

    Move *move_to_play = create_move();
    copy_move(game->gen->move_list->moves[0], move_to_play);
    if (test_inference &&
        (move_to_play->move_type == GAME_EVENT_EXCHANGE ||
         move_to_play->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE)) {
      reset_rack(tiles_played);
      reset_rack(full_rack);
      for (int i = 0; i < move_to_play->tiles_length; i++) {
        uint8_t letter = move_to_play->tiles[i];
        if (letter != PLAYED_THROUGH_MARKER) {
          if (is_blanked(letter)) {
            letter = BLANK_MACHINE_LETTER;
          }
          add_letter_to_rack(tiles_played, letter);
        }
      }

      // Remove tiles from rack for the inference
      for (int ml = 0;
           ml < game->players[game->player_on_turn_index]->rack->array_size;
           ml++) {
        int number_of_ml =
            game->players[game->player_on_turn_index]->rack->array[ml];
        for (int li = 0; li < number_of_ml; li++) {
          take_letter_from_rack(game->players[game->player_on_turn_index]->rack,
                                ml);
          add_letter_to_rack(full_rack, ml);
          add_letter(game->gen->bag, ml);
        }
      }

      int number_of_tiles_exchanged = 0;
      if (move_to_play->move_type == GAME_EVENT_EXCHANGE) {
        number_of_tiles_exchanged = move_to_play->tiles_played;
        reset_rack(tiles_played);
      }

      // Single threaded infer
      infer(thread_control, inference_1, game, tiles_played,
            game->player_on_turn_index, move_to_play->score,
            number_of_tiles_exchanged, 0, 1);
      int status = inference_1->status;
      sort_leave_racks(inference_1->leave_rack_list);
      if (status != INFERENCE_STATUS_SUCCESS) {
        printf("bad status for single threaded %d, seed is >%ld<\n", status,
               seed);
        print_error_case(game, inference_1, inference_2, tiles_played,
                         game->player_on_turn_index, move_to_play->score,
                         number_of_tiles_exchanged, 7);
      }

      infer(thread_control, inference_2, game, tiles_played,
            game->player_on_turn_index, move_to_play->score,
            number_of_tiles_exchanged, 0, 7);
      status = inference_2->status;
      sort_leave_racks(inference_2->leave_rack_list);
      if (status != INFERENCE_STATUS_SUCCESS) {
        printf("bad status for 7 threads %d, seed is >%ld<\n", status, seed);
        print_error_case(game, inference_1, inference_2, tiles_played,
                         game->player_on_turn_index, move_to_play->score,
                         number_of_tiles_exchanged, 7);
      }

      int are_inferences_equal_value =
          are_inferences_equal(inference_1, inference_2);

      if (are_inferences_equal_value != 0) {
        printf("1 and 7 not equal %d, seed is >%ld<\n",
               are_inferences_equal_value, seed);
        print_error_case(game, inference_1, inference_2, tiles_played,
                         game->player_on_turn_index, move_to_play->score,
                         number_of_tiles_exchanged, 7);
      }

      infer(thread_control, inference_2, game, tiles_played,
            game->player_on_turn_index, move_to_play->score,
            number_of_tiles_exchanged, 0, 10);
      sort_leave_racks(inference_2->leave_rack_list);
      if (status != INFERENCE_STATUS_SUCCESS) {
        printf("bad status for 10 threads %d, seed is >%ld<\n", status, seed);
        print_error_case(game, inference_1, inference_2, tiles_played,
                         game->player_on_turn_index, move_to_play->score,
                         number_of_tiles_exchanged, 7);
      }

      are_inferences_equal_value =
          are_inferences_equal(inference_1, inference_2);
      if (are_inferences_equal_value != 0) {
        printf("1 and 10 not equal %d, seed is >%ld<\n",
               are_inferences_equal_value, seed);
        print_error_case(game, inference_1, inference_2, tiles_played,
                         game->player_on_turn_index, move_to_play->score,
                         number_of_tiles_exchanged, 10);
      }

      // Add tiles back to rack
      for (int ml = 0;
           ml < game->players[game->player_on_turn_index]->rack->array_size;
           ml++) {
        for (int li = 0; li < full_rack->array[ml]; li++) {
          add_letter_to_rack(game->players[game->player_on_turn_index]->rack,
                             ml);
          draw_letter(game->gen->bag, ml);
        }
      }
    }
    play_move(game, move_to_play);
    reset_move_list(game->gen->move_list);
    destroy_move(move_to_play);
  }
}

void autoplay_inference_test(Config *config) {
  Game *game = create_game(config);
  ThreadControl *thread_control = create_thread_control(NULL);
  // Use the player_to_infer_index as a intean
  // indicating whether to test inferences in autoplay.
  int test_inference = config->player_to_infer_index >= 0;
  Rack *full_rack = create_rack(config->letter_distribution->size);
  Rack *tiles_played = create_rack(config->letter_distribution->size);
  Inference *inference_1 =
      create_inference(30, config->letter_distribution->size);
  Inference *inference_2 =
      create_inference(30, config->letter_distribution->size);
  time_t seed;
  seed = time(NULL);
  reseed_prng(game->gen->bag, seed);
  for (int i = 0; i < config->number_of_games_or_pairs; i++) {
    reset_game(game);
    play_game_test(thread_control, game, inference_1, inference_2, tiles_played,
                   full_rack, seed, test_inference);
  }

  destroy_game(game);
  destroy_rack(full_rack);
  destroy_rack(tiles_played);
  destroy_inference(inference_1);
  destroy_inference(inference_2);
  destroy_thread_control(thread_control);
}

void autoplay_game_pairs_test(SuperConfig *superconfig) {
  Config *csw_config = get_csw_config(superconfig);
  int game_pairs = 1000;
  int number_of_threads = 11;
  int original_number_of_game_pairs = csw_config->number_of_games_or_pairs;
  csw_config->number_of_games_or_pairs = game_pairs;
  int original_number_of_threads = csw_config->number_of_threads;
  csw_config->number_of_threads = number_of_threads;
  int original_use_game_pairs = csw_config->use_game_pairs;
  csw_config->use_game_pairs = 1;

  ThreadControl *thread_control = create_thread_control_from_config(csw_config);
  AutoplayResults *autoplay_results = create_autoplay_results();

  autoplay(thread_control, autoplay_results, csw_config, 0);

  assert(autoplay_results->total_games == game_pairs * 2);
  assert(autoplay_results->p1_firsts == game_pairs);
  assert(get_weight(autoplay_results->p1_score) ==
         get_weight(autoplay_results->p2_score));
  assert(get_cardinality(autoplay_results->p1_score) ==
         get_cardinality(autoplay_results->p2_score));
  assert(within_epsilon(get_mean(autoplay_results->p1_score),
                        get_mean(autoplay_results->p2_score)));
  assert(within_epsilon(get_stdev(autoplay_results->p1_score),
                        get_stdev(autoplay_results->p2_score)));
  csw_config->number_of_games_or_pairs = original_number_of_game_pairs;
  csw_config->number_of_threads = original_number_of_threads;
  csw_config->use_game_pairs = original_use_game_pairs;

  destroy_thread_control(thread_control);
  destroy_autoplay_results(autoplay_results);
}

void test_autoplay(SuperConfig *superconfig) {
  autoplay_game_pairs_test(superconfig);
}
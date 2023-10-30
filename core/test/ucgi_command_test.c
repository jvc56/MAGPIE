#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/game.h"
#include "../src/go_params.h"
#include "../src/infer.h"
#include "../src/log.h"
#include "../src/sim.h"
#include "../src/thread_control.h"
#include "../src/ucgi_command.h"

#include "test_constants.h"
#include "test_util.h"

void block_for_search(UCGICommandVars *ucgi_command_vars, int max_seconds) {
  // Poll for the end of the command
  int seconds_elapsed = 0;
  while (1) {
    if (get_mode(ucgi_command_vars->thread_control) == MODE_STOPPED) {
      break;
    } else {
      sleep(1);
    }
    seconds_elapsed++;
    if (seconds_elapsed >= max_seconds) {
      log_fatal("Test aborted after searching for %d seconds", max_seconds);
    }
  }
}

void assert_nthline_equals(char *text, int n, const char *expected_line) {
  // returns the nth line of the text (0-indexed)
  StringSplitter *split_text = split_string(text, '\n', false);
  bool n_less_than_number_of_items =
      n < string_splitter_get_number_of_items(split_text);
  bool line_matches = false;
  if (n_less_than_number_of_items) {
    line_matches =
        strings_equal(expected_line, string_splitter_get_item(split_text, n));
  }
  destroy_string_splitter(split_text);
  assert(n_less_than_number_of_items);
  assert(line_matches);
}

void test_ucgi_command() {
  int depth;
  int stopcondition;
  int threads;
  int player_index;
  int score;
  int number_of_tiles_exchanged;
  int plays;
  int iters;
  int checkstop;
  int info;
  StringBuilder *move_string_builder = create_string_builder();
  StringBuilder *test_stdin_input = create_string_builder();

  size_t len;
  size_t prev_len = 0;
  char *output_buffer;
  FILE *file_handler = open_memstream(&output_buffer, &len);

  UCGICommandVars *ucgi_command_vars = create_ucgi_command_vars(file_handler);
  // Test the ucgi command
  int result = process_ucgi_command_async("ucgi", ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  assert(
      strings_equal("id name MAGPIE 0.1\nucgiok\n", output_buffer + prev_len));
  prev_len = len;

  // Test the quit command
  result = process_ucgi_command_async("quit", ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_QUIT);
  prev_len = len;

  // Test the position cgp command
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "position cgp %s",
                                      ION_OPENING_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  assert(strings_equal("CSW21", ucgi_command_vars->last_lexicon_name));
  assert(strings_equal("english", ucgi_command_vars->last_ld_name));
  assert(ucgi_command_vars->loaded_game->gen->bag->last_tile_index + 1 == 83);
  prev_len = len;

  // Test go parse failures
  // invalid stop cond
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s",
                                      "go stopcondition 93");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(!is_halted(ucgi_command_vars->thread_control));
  assert(result == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED);
  prev_len = len;

  // Test go parse failures
  // stop cond and infinite
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s",
                                      "go stopcondition 95 i 0");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED);
  prev_len = len;

  // Test go parse failures
  // sim + depth of 0
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s",
                                      "go sim stopcondition 95 depth 0");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED);
  prev_len = len;

  // Test go parse failures
  // nonpositive threads
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input, "%s", "go sim stopcondition 95 depth 1 threads 0");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED);
  prev_len = len;

  // Test go parse failures
  // too many search types
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s", "go sim infer");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_COMMAND_PARSE_FAILED);
  prev_len = len;

  // Test sim finishing probabilistically
  depth = 2;
  stopcondition = 95;
  threads = 8;
  plays = 3;
  iters = 100000;
  checkstop = 300;
  info = 70;

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      ZILLION_OPENING_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go sim depth %d stopcondition %d threads %d plays %d i %d "
      "checkstop %d info %d",
      depth, stopcondition, threads, plays, iters, checkstop, info);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Check the go params
  assert(ucgi_command_vars->go_params->depth == depth);
  assert(ucgi_command_vars->go_params->stop_condition ==
         SIM_STOPPING_CONDITION_95PCT);
  assert(ucgi_command_vars->go_params->threads == threads);
  assert(ucgi_command_vars->go_params->num_plays == plays);
  assert(ucgi_command_vars->go_params->max_iterations == iters);
  assert(ucgi_command_vars->go_params->check_stopping_condition_interval ==
         checkstop);
  assert(ucgi_command_vars->go_params->print_info_interval == info);

  block_for_search(ucgi_command_vars, 15);
  // Command is done
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_PROBABILISTIC);
  assert(ucgi_command_vars->thread_control->check_stopping_condition_interval ==
         checkstop);
  assert(ucgi_command_vars->thread_control->print_info_interval == info);

  sort_plays_by_win_rate(ucgi_command_vars->simmer->simmed_plays,
                         ucgi_command_vars->simmer->num_simmed_plays);

  string_builder_clear(move_string_builder);
  string_builder_add_move_description(
      ucgi_command_vars->simmer->simmed_plays[0]->move,
      ucgi_command_vars->loaded_game->gen->letter_distribution,
      move_string_builder);

  // If this isn't the best simming move, the universe implodes or something
  assert_strings_equal(string_builder_peek(move_string_builder), "8D ZILLION");
  // This test assumes that the 95pct condition was
  // met at the first check, which is 300 in this case.
  // If this isn't true then buy a lotto ticket or something.

  // Use checkstop / info to get the number of prints
  // made during the sim.
  // Add 1 since the info is printed one last time
  // when the sim finished.
  // Multiply it by the number of plays + 2 because each play
  // gets its own line, and there's a line for bestsofar (or bestmove)
  // and another line for nps stats.
  int number_of_output_lines = (plays + 2) * ((checkstop / info) + 1);
  assert(number_of_output_lines == count_newlines(output_buffer + prev_len));

  prev_len = len;

  // Test sim static only
  plays = 20;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      ION_OPENING_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go sim depth %d stopcondition %d threads %d plays %d i %d "
      "checkstop %d info %d static",
      depth, stopcondition, threads, plays, iters, checkstop, info);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Check the go params
  assert(ucgi_command_vars->go_params->static_search_only);
  block_for_search(ucgi_command_vars, 2);
  // Command is done
  assert(ucgi_command_vars->thread_control->halt_status == HALT_STATUS_NONE);
  // A static search should only print out the number
  // of plays specified plus a "bestmove" line.
  assert((plays + 1) == count_newlines(output_buffer + prev_len));
  prev_len = len;

  // Test max iterations
  depth = 2;
  threads = 10;
  plays = 15;
  iters = 200;
  info = 60;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      DELDAR_VS_HARSHAN_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input, "go sim depth %d threads %d plays %d i %d info %d",
      depth, threads, plays, iters, info);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // This shouldn't take more than 30 seconds.
  block_for_search(ucgi_command_vars, 30);
  // Command is done
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_MAX_ITERATIONS);
  assert(ucgi_command_vars->simmer->iteration_count == iters);
  number_of_output_lines = (plays + 2) * ((iters / info) + 1);
  assert(number_of_output_lines == count_newlines(output_buffer + prev_len));

  prev_len = len;

  // Test user interrupt
  // With these params, sim shouldn't stop
  // for a long time which will ensure that
  // we can always halt it before it finishes.
  depth = 2;
  threads = 10;
  plays = 15;
  iters = 1000000;
  info = 1000000;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      DELDAR_VS_HARSHAN_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input, "go sim depth %d threads %d plays %d i %d info %d",
      depth, threads, plays, iters, info);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Let the command run for a bit
  sleep(2);
  // Send the halt signal
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s", "stop");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // It should not take more than 5 seconds to halt.
  block_for_search(ucgi_command_vars, 5);
  // Command is done
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_USER_INTERRUPT);

  // The sim should not have had time to print anything
  // since the print info interval was set very high.
  // It should only print the moves once at the end when
  // it finishes.
  assert(plays + 2 == count_newlines(output_buffer + prev_len));
  prev_len = len;

  // Test infer
  Stat *letter_stat = create_stat();

  player_index = 0;
  score = 58;
  number_of_tiles_exchanged = 0;
  info = 4;
  plays = 20;
  threads = 4;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      EMPTY_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go infer tiles MUZAKY pidx %d score %d exch %d plays %d info %d "
      "threads %d",
      player_index, score, number_of_tiles_exchanged, plays, info, threads);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  // Check the go params
  assert(ucgi_command_vars->go_params->player_index == player_index);
  assert(ucgi_command_vars->go_params->score == score);
  assert(ucgi_command_vars->go_params->number_of_tiles_exchanged ==
         number_of_tiles_exchanged);
  assert(strings_equal(ucgi_command_vars->go_params->tiles, "MUZAKY"));

  // It shouldn't take too long to run an inference for a 2 tile leave.
  block_for_search(ucgi_command_vars, 5);
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_MAX_ITERATIONS);
  assert(ucgi_command_vars->inference->status == INFERENCE_STATUS_SUCCESS);
  Inference *inference = ucgi_command_vars->inference;
  Game *game = ucgi_command_vars->loaded_game;
  // Letters not possible:
  // A - YAKUZA
  // B - ZAMBUK
  // K - none in bag
  // Q - QUAKY
  // Z - none in bag
  assert(get_weight(inference->leave_record->equity_values) == 83);
  assert(get_cardinality(inference->leave_record->equity_values) == 22);
  for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    if (i == human_readable_letter_to_machine_letter(
                 game->gen->letter_distribution, "A") ||
        i == human_readable_letter_to_machine_letter(
                 game->gen->letter_distribution, "B") ||
        i == human_readable_letter_to_machine_letter(
                 game->gen->letter_distribution, "K") ||
        i == human_readable_letter_to_machine_letter(
                 game->gen->letter_distribution, "Q") ||
        i == human_readable_letter_to_machine_letter(
                 game->gen->letter_distribution, "Z")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    }
  }
  get_stat_for_letter(inference->leave_record, letter_stat,
                      human_readable_letter_to_machine_letter(
                          game->gen->letter_distribution, "E"));
  assert(within_epsilon(get_mean(letter_stat), (double)12 / 83));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "Q"),
                            1, 6),
                        (double)1 / 94));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "B"),
                            1, 6),
                        (double)2 / 94));
  // 1: for printing the max index +
  // dist size / info: for the number of current index prints +
  // 4: for inference info +
  // dist size: for info about each tile
  // plays: for most common leaves
  number_of_output_lines =
      1 +
      (ucgi_command_vars->loaded_game->gen->letter_distribution->size / info) +
      4 + ucgi_command_vars->loaded_game->gen->letter_distribution->size +
      plays;
  assert(number_of_output_lines == count_newlines(output_buffer + prev_len));

  prev_len = len;

  // test exchange
  player_index = 0;
  score = 0;
  number_of_tiles_exchanged = 5;
  info = 500;
  plays = 20;
  threads = 10;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      VS_MATT);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go infer pidx %d score %d exch %d plays %d info %d threads %d",
      player_index, score, number_of_tiles_exchanged, plays, info, threads);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  block_for_search(ucgi_command_vars, 60);
  assert(ucgi_command_vars->inference->status == INFERENCE_STATUS_SUCCESS);
  number_of_output_lines =
      1 + (inference->total_racks_evaluated / info) +
      (4 + ucgi_command_vars->loaded_game->gen->letter_distribution->size) * 3 +
      plays;
  assert(number_of_output_lines == count_newlines(output_buffer + prev_len));

  prev_len = len;

  // test interrupt
  player_index = 0;
  score = 0;
  number_of_tiles_exchanged = 3;
  plays = 20;
  info = 5000000;
  threads = 5;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      EMPTY_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go infer pidx %d score %d exch %d plays %d info %d threads %d",
      player_index, score, number_of_tiles_exchanged, plays, info, threads);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Let the command run for a bit
  sleep(2);
  // Send the halt signal
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s", "stop");
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // It should not take more than 5 seconds to halt.
  block_for_search(ucgi_command_vars, 5);
  // Command is done
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_USER_INTERRUPT);
  prev_len = len;

  // test exchange
  player_index = 0;
  score = 30;
  number_of_tiles_exchanged = 5;
  info = 500;
  plays = 20;
  threads = 10;
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      EMPTY_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input,
      "go infer pidx %d score %d exch %d plays %d info %d threads %d",
      player_index, score, number_of_tiles_exchanged, plays, info, threads);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // The inference should fail
  block_for_search(ucgi_command_vars, 5);
  assert(ucgi_command_vars->inference->status ==
         INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO);
  number_of_output_lines = 1;
  assert(number_of_output_lines == count_newlines(output_buffer + prev_len));

  // Test sim finishing with async API
  depth = 2;
  threads = 8;
  plays = 3;

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      ZILLION_OPENING_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input, "go sim depth %d threads %d plays %d i %d", depth,
      threads, plays, 100000);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Check the go params
  assert(ucgi_command_vars->go_params->depth == depth);
  assert(ucgi_command_vars->go_params->stop_condition == 0);
  assert(ucgi_command_vars->go_params->threads == threads);
  assert(ucgi_command_vars->go_params->num_plays == plays);
  assert(ucgi_command_vars->go_params->max_iterations == 100000);
  assert(ucgi_command_vars->go_params->check_stopping_condition_interval == 0);
  assert(ucgi_command_vars->go_params->print_info_interval == 0);
  sleep(5);

  char *ret = ucgi_search_status(ucgi_command_vars);
  assert_nthline_equals(ret, 3, "bestsofar 8d.ZILLION");
  free(ret);
  // Sleep a couple more seconds and then stop the search.
  sleep(2);
  char *ret2 = ucgi_stop_search(ucgi_command_vars);
  assert_nthline_equals(ret2, 3, "bestmove 8d.ZILLION");
  free(ret2);

  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_USER_INTERRUPT);

  // Test async API with a sim that finished fast.
  // 100 iterations should finish faster than in 5 seconds.
  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(test_stdin_input, "%s%s", "position cgp ",
                                      ZILLION_OPENING_CGP);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);

  string_builder_clear(test_stdin_input);
  string_builder_add_formatted_string(
      test_stdin_input, "go sim depth %d threads %d plays %d i %d", depth,
      threads, plays, 100);
  result = process_ucgi_command_async(string_builder_peek(test_stdin_input),
                                      ucgi_command_vars);
  assert(result == UCGI_COMMAND_STATUS_SUCCESS);
  // Check the go params
  assert(ucgi_command_vars->go_params->depth == depth);
  assert(ucgi_command_vars->go_params->stop_condition == 0);
  assert(ucgi_command_vars->go_params->threads == threads);
  assert(ucgi_command_vars->go_params->num_plays == plays);
  assert(ucgi_command_vars->go_params->max_iterations == 100);
  assert(ucgi_command_vars->go_params->check_stopping_condition_interval == 0);
  assert(ucgi_command_vars->go_params->print_info_interval == 0);
  sleep(5);
  // sim should have ended by now
  int mode = get_mode(ucgi_command_vars->thread_control);
  assert(mode != MODE_SEARCHING);
  ret = ucgi_search_status(ucgi_command_vars);
  assert_nthline_equals(ret, 3, "bestmove 8d.ZILLION");
  free(ret);
  // Sleep a couple more seconds and then try to stop the search. It
  // shouldn't work.
  sleep(2);
  ret2 = ucgi_stop_search(ucgi_command_vars);
  assert(!ret2);
  assert(ucgi_command_vars->thread_control->halt_status ==
         HALT_STATUS_MAX_ITERATIONS);

  fclose(file_handler);
  free(output_buffer);
  destroy_ucgi_command_vars(ucgi_command_vars);
  destroy_stat(letter_stat);
  destroy_string_builder(move_string_builder);
  destroy_string_builder(test_stdin_input);
}
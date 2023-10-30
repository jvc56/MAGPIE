#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/command.h"
#include "../src/game.h"
#include "../src/infer.h"
#include "../src/log.h"
#include "../src/sim.h"
#include "../src/thread_control.h"

#include "test_constants.h"
#include "test_util.h"

void block_for_search(CommandVars *command_vars, int max_seconds) {
  // Poll for the end of the command
  int seconds_elapsed = 0;
  while (1) {
    if (get_mode(command_vars->config->thread_control) == MODE_STOPPED) {
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

void assert_command_status_and_output(CommandVars *command_vars,
                                      const char *command, bool should_halt,
                                      int seconds_to_wait,
                                      halt_status_t expected_halt_status,
                                      error_status_t expected_error_status_type,
                                      int expected_error_code,
                                      int expected_output_line_count) {
  char *test_output_filename =
      get_formatted_string("%s%s", TESTDATA_FILEPATH, "test_command_output");

  remove(test_output_filename);
  set_outfile(command_vars->config->thread_control, test_output_filename);
  set_errorfile(command_vars->config->thread_control, test_output_filename);

  command_vars->command = command;
  execute_command_async(command_vars);
  if (should_halt) {
    // If halting, let the search start
    sleep(2);
    halt(command_vars->config->thread_control, HALT_STATUS_USER_INTERRUPT);
  }
  block_for_search(command_vars, seconds_to_wait);

  char *test_output = get_string_from_file(test_output_filename);
  if (command_vars->error_status->type != expected_error_status_type) {
    printf("expected error status != actual error status (%d != %d)\n",
           expected_error_status_type, command_vars->error_status->type);
  }
  if (command_vars->error_status->code != expected_error_code) {
    printf("expected error code != actual error code (%d != %d)\n",
           expected_error_code, command_vars->error_status->code);
  }
  assert(command_vars->error_status->type == expected_error_status_type);
  assert(command_vars->error_status->code == expected_error_code);
  assert(get_mode(command_vars->config->thread_control) == MODE_STOPPED);
  int newlines_in_output = count_newlines(test_output);
  if (newlines_in_output != expected_output_line_count) {
    printf("output:\n>%s<\n", test_output);
    printf("counts do not match %d != %d\n", newlines_in_output,
           expected_output_line_count);
    assert(0);
  }
  remove(test_output_filename);
  free(test_output);
  free(test_output_filename);
}

void test_command_execution() {
  CommandVars *command_vars = create_command_vars();

  assert_command_status_and_output(
      command_vars, "go sim lex CSW21 i 1000 plies", false, 5, HALT_STATUS_NONE,
      ERROR_STATUS_TYPE_CONFIG_LOAD,
      (int)CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES, 1);

  assert_command_status_and_output(
      command_vars,
      "position cgp 15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 "
      "ABC5DF/YXZ 0/0 0 lex CSW21",
      false, 5, HALT_STATUS_NONE, ERROR_STATUS_TYPE_CGP_LOAD,
      (int)CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS, 1);

  // Test load cgp
  assert_command_status_and_output(
      command_vars, "position cgp " ION_OPENING_CGP, false, 5, HALT_STATUS_NONE,
      ERROR_STATUS_TYPE_NONE, 0, 0);

  // Sim finishing probabilistically
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 stop 95 threads 8 numplays 3 i 100000 check 300 "
      "info 500 cgp " ZILLION_OPENING_CGP,
      false, 60, HALT_STATUS_PROBABILISTIC, ERROR_STATUS_TYPE_NONE, 0, 5);

  // Sim statically
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 stop 95 threads 8 numplays 20 i 100000 check 300 "
      "info 70 static cgp " ZILLION_OPENING_CGP,
      false, 60, HALT_STATUS_NONE, ERROR_STATUS_TYPE_NONE, 0, 21);

  // Sim finishes with max iterations
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 threads 10 numplays 15 i "
      "200 info 60 nostatic cgp " DELDAR_VS_HARSHAN_CGP,
      false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0,
      (17 * 4));

  // Sim interrupted by user
  assert_command_status_and_output(
      command_vars,
      "go sim plies 2 threads 10 numplays 15 i "
      "1000000 info 1000000 cgp " DELDAR_VS_HARSHAN_CGP,
      true, 5, HALT_STATUS_USER_INTERRUPT, ERROR_STATUS_TYPE_NONE, 0, 17);

  // Infer finishes normally
  assert_command_status_and_output(
      command_vars,
      "go infer rack MUZAKY pindex 0 score 58 exch 0 numplays 20 threads 4 "
      "cgp " EMPTY_CGP,
      false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0,
      5 + 27 + 20);

  // Infer interrupted
  assert_command_status_and_output(
      command_vars,
      "go infer rack " EMPTY_RACK_STRING
      " pindex 0 score 0 exch 3 numplays 20 threads 3 "
      "cgp " EMPTY_CGP,
      true, 5, HALT_STATUS_USER_INTERRUPT, ERROR_STATUS_TYPE_NONE, 0, 1);

  // Autoplay finishes normally
  assert_command_status_and_output(command_vars,
                                   "go autoplay lex CSW21 s1 equity s2 equity "
                                   "r1 best r2 best i 10 numplays 1 threads 3",
                                   false, 30, HALT_STATUS_MAX_ITERATIONS,
                                   ERROR_STATUS_TYPE_NONE, 0, 1);

  // Autoplay interrupted
  assert_command_status_and_output(command_vars,
                                   "go autoplay lex CSW21 s1 equity s2 equity "
                                   "r1 best r2 best i 10000000 threads 5",
                                   true, 5, HALT_STATUS_USER_INTERRUPT,
                                   ERROR_STATUS_TYPE_NONE, 0, 1);

  for (int i = 0; i < 3; i++) {
    // Catalan
    assert_command_status_and_output(command_vars, "position cgp " CATALAN_CGP,
                                     false, 5, HALT_STATUS_NONE,
                                     ERROR_STATUS_TYPE_NONE, 0, 0);
    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " CATALAN_CGP,
                                     false, 60, HALT_STATUS_MAX_ITERATIONS,
                                     ERROR_STATUS_TYPE_NONE, 0, (17 * 4));
    assert_command_status_and_output(
        command_vars,
        "go infer rack AIMSX pindex 0 score 52 exch "
        "0 numplays 20 threads 4 info 1000000 cgp " EMPTY_CATALAN_CGP,
        false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0,
        5 + 27 + 20);

    assert_command_status_and_output(
        command_vars,
        "go autoplay s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1 cgp " CATALAN_CGP,
        false, 30, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0, 1);
    // CSW
    assert_command_status_and_output(
        command_vars, "position cgp " ION_OPENING_CGP, false, 5,
        HALT_STATUS_NONE, ERROR_STATUS_TYPE_NONE, 0, 0);

    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " DELDAR_VS_HARSHAN_CGP,
                                     false, 60, HALT_STATUS_MAX_ITERATIONS,
                                     ERROR_STATUS_TYPE_NONE, 0, (17 * 4));

    assert_command_status_and_output(
        command_vars,
        "go infer rack DGINR pindex 0 score 18 exch 0 numplays 20 threads 4 "
        "info 1000000 "
        "cgp " EMPTY_CGP,
        false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0,
        5 + 27 + 20);

    assert_command_status_and_output(
        command_vars,
        "go autoplay lex CSW21 s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1",
        false, 30, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0, 1);
    // Polish
    assert_command_status_and_output(command_vars, "position cgp " POLISH_CGP,
                                     false, 5, HALT_STATUS_NONE,
                                     ERROR_STATUS_TYPE_NONE, 0, 0);

    assert_command_status_and_output(command_vars,
                                     "go sim plies 2 threads 10 numplays 15 i "
                                     "200 info 60 cgp " POLISH_CGP,
                                     false, 60, HALT_STATUS_MAX_ITERATIONS,
                                     ERROR_STATUS_TYPE_NONE, 0, 68);

    assert_command_status_and_output(
        command_vars,
        "go infer rack HUJA pindex 0 score 20 exch 0 "
        "numplays 20 info 1000000  threads 4 cgp " EMPTY_POLISH_CGP,
        false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0,
        5 + 33 + 20);

    assert_command_status_and_output(
        command_vars,
        "go autoplay s1 equity s2 equity "
        "r1 best r2 best i 10 numplays 1 lex OSPS44",
        false, 30, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0, 1);
  }
  destroy_command_vars(command_vars);
}

void test_command() { test_command_execution(); }
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

void block_for_search(CommandVars *command_vars, int max_seconds) {
  // Poll for the end of the command
  int seconds_elapsed = 0;
  while (1) {
    if (get_mode(command_vars->thread_control) == MODE_STOPPED) {
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

void assert_command_status_and_output(const char *command, bool halt,
                                      int seconds_to_wait,
                                      halt_status_t expected_halt_status,
                                      error_status_t expected_error_status_type,
                                      int expected_error_code,
                                      const char *expected_output) {
  size_t len;
  size_t prev_len = 0;
  char *output_buffer;
  FILE *file_handler = open_memstream(&output_buffer, &len);

  CommandVars *command_vars = create_command_vars(file_handler);
  command_vars->command = command;
  execute_command_async(command_vars);
  if (halt) {
    // If halting, let the search start
    sleep(2);
    halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT);
  }
  block_for_search(command_vars, seconds_to_wait);
  assert(command_vars->thread_control->halt_status == expected_halt_status);
  assert(get_mode(command_vars->thread_control) == MODE_STOPPED);
  assert(command_vars->error_status->type == expected_error_status_type);
  assert(command_vars->error_status->code == expected_error_code);
  assert_strings_equal(expected_output, output_buffer);
}

void test_command_execution() {
  assert_command_status_and_output(
      "go sim lex CSW21 i 1000 plies", false, 5, HALT_STATUS_NONE,
      ERROR_STATUS_TYPE_CONFIG_LOAD,
      (int)CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES, "");

  assert_command_status_and_output(
      "position cgp 15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 "
      "ABC5DF/YXZ 0/0 0",
      false, 5, HALT_STATUS_NONE, ERROR_STATUS_TYPE_CGP_LOAD,
      (int)CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS, "");

  // Test load cgp
  assert_command_status_and_output("position cgp " ION_OPENING_CGP, false, 5,
                                   HALT_STATUS_NONE, ERROR_STATUS_TYPE_NONE, 0,
                                   "");
  assert(ucgi_command_vars->game->gen->bag->last_tile_index + 1 == 83);

  // Sim finishing probabilistically
  assert_command_status_and_output(
      "go sim depth 2 stop 95 threads 8 numplays 3 i 100000 check 300 "
      "info 70 lex CSW21 cgp " ZILLION_OPENING_CGP,
      false, 60, HALT_STATUS_PROBABILISTIC, ERROR_STATUS_TYPE_NONE, 0, "");

  // Sim statically
  assert_command_status_and_output(
      "go sim depth 2 stop 95 threads 8 numplays 20 i 100000 check 300 "
      "info 70 static lex CSW21 cgp " ZILLION_OPENING_CGP,
      false, 60, HALT_STATUS_NONE, ERROR_STATUS_TYPE_NONE, 0, "");

  // Sim finishes with max iterations
  assert_command_status_and_output(
      "go sim depth 2 threads 10 numplays 15 i "
      "200 info 60 lex CSW21 cgp " DELDAR_VS_HARSHAN_CGP,
      false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0, "");

  // Sim interrupted by user
  assert_command_status_and_output(
      "go sim depth 2 threads 10 numplays 15 i "
      "1000000 info 1000000 cgp " DELDAR_VS_HARSHAN_CGP,
      true, 5, HALT_STATUS_USER_INTERRUPT, ERROR_STATUS_TYPE_NONE, 0, "");

  // Infer finishes normally
  assert_command_status_and_output(
      "go infer tiles MUZAKY pindex 0 score 58 exch 0 numplays 20 threads 4 "
      "cgp " EMPTY_CGP,
      false, 60, HALT_STATUS_MAX_ITERATIONS, ERROR_STATUS_TYPE_NONE, 0, "");

  // Infer interrupted
  assert_command_status_and_output(
      "go infer pindex 0 score 0 exch 3 numplays 20 threads 3 "
      "cgp " EMPTY_CGP,
      true, 5, HALT_STATUS_USER_INTERRUPT, ERROR_STATUS_TYPE_NONE, 0, "");

  // Autoplay finishes normally
  assert_command_status_and_output("go autoplay lex CSW21 s1 equity s2 equity "
                                   "r1 best r2 best i 10 numplays 1",
                                   false, 30, HALT_STATUS_MAX_ITERATIONS,
                                   ERROR_STATUS_TYPE_NONE, 0, "");

  // Autoplay interrupted
  assert_command_status_and_output(
      "go autoplay lex CSW21 s1 equity s2 equity r1 best r2 best i 10000000",
      true, 5, HALT_STATUS_USER_INTERRUPT, ERROR_STATUS_TYPE_NONE, 0, "");

  // FIXME: run more commands that change rack size
  //   run all 3 command in catalan
  //   run all 3 in csw
  //   run all 3 in polish?
  //   repeat
}

void test_command() { test_command_execution(); }
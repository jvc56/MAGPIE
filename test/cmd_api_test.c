#include "../src/impl/cmd_api.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_EMPTY_BOARD_CGP                                                   \
  "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AQRTUYZ/ 0/0 0"

void assert_run_sync_success(Magpie *mp, const char *command) {
  const cmd_exit_code exit_code = magpie_run_sync(mp, command);
  if (exit_code != MAGPIE_SUCCESS) {
    char *error = magpie_get_and_clear_error(mp);
    (void)fprintf(stderr, "command '%s' failed with exit code %d: %s\n",
                  command, exit_code, error);
    free(error);
  }
  assert(exit_code == MAGPIE_SUCCESS);
  assert(!magpie_has_error(mp));
}

void test_cmd_api_create_failure(void) {
  Magpie *mp = magpie_create("./nonexistent_data_path");
  assert(magpie_has_error(mp));
  assert(magpie_get_thread_status(mp) == MAGPIE_THREAD_STATUS_UNINITIALIZED);

  char *creation_error = magpie_get_and_clear_error(mp);
  assert(!strings_equal(creation_error, ""));
  free(creation_error);
  assert(!magpie_has_error(mp));

  // Commands on a failed instance report failure instead of crashing.
  const cmd_exit_code exit_code = magpie_run_sync(mp, "set -lex CSW21");
  assert(exit_code == MAGPIE_DID_NOT_RUN);
  assert(magpie_has_error(mp));
  char *run_error = magpie_get_and_clear_error(mp);
  assert(has_substring(run_error, "magpie creation failed"));
  free(run_error);

  char *status_message = magpie_get_last_command_status_message(mp);
  assert(strings_equal(status_message, ""));
  free(status_message);

  magpie_stop_current_command(mp);
  magpie_destroy(mp);
}

void test_cmd_api_run_commands(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));

  // No error is pending after creation.
  char *no_error = magpie_get_and_clear_error(mp);
  assert(strings_equal(no_error, ""));
  free(no_error);

  assert_run_sync_success(mp, "set -lex CSW21");
  assert_run_sync_success(mp, TEST_EMPTY_BOARD_CGP);

  // Commands that only set options produce empty output.
  char *set_output = magpie_get_last_command_output(mp);
  assert(strings_equal(set_output, ""));
  free(set_output);

  assert_run_sync_success(mp, "generate");
  assert(magpie_get_thread_status(mp) == MAGPIE_THREAD_STATUS_FINISHED);

  // The default output is machine readable and has no display header.
  char *machine_output = magpie_get_last_command_output(mp);
  assert(!strings_equal(machine_output, ""));
  assert(!has_substring(machine_output, "StEq"));
  free(machine_output);

  // The human-readable variant includes the display header.
  const cmd_exit_code human_readable_exit_code =
      magpie_run_sync_human_readable(mp, "shmoves");
  assert(human_readable_exit_code == MAGPIE_SUCCESS);
  char *human_readable_output = magpie_get_last_command_output(mp);
  assert(has_substring(human_readable_output, "StEq"));
  free(human_readable_output);

  // The output format resets to machine readable on the next command.
  assert_run_sync_success(mp, "shmoves");
  char *second_machine_output = magpie_get_last_command_output(mp);
  assert(!has_substring(second_machine_output, "StEq"));
  free(second_machine_output);

  magpie_destroy(mp);
}

void test_cmd_api_errors(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));

  // An unparseable command does not run.
  cmd_exit_code exit_code = magpie_run_sync(mp, "notacommand");
  assert(exit_code == MAGPIE_DID_NOT_RUN);
  assert(magpie_has_error(mp));
  char *parse_error = magpie_get_and_clear_error(mp);
  assert(has_substring(parse_error, "notacommand"));
  free(parse_error);

  // The last command output is cleared by a failed command.
  char *failed_output = magpie_get_last_command_output(mp);
  assert(strings_equal(failed_output, ""));
  free(failed_output);

  // A command that runs but fails returns MAGPIE_ERROR.
  assert_run_sync_success(mp, "set -lex CSW21");
  exit_code = magpie_run_sync(mp, "shmoves");
  assert(exit_code == MAGPIE_ERROR);
  assert(magpie_has_error(mp));
  char *show_moves_error = magpie_get_and_clear_error(mp);
  assert(!strings_equal(show_moves_error, ""));
  free(show_moves_error);

  // Empty commands are valid no-ops, run repeatedly without error.
  assert_run_sync_success(mp, "");
  assert_run_sync_success(mp, "");
  char *empty_command_output = magpie_get_last_command_output(mp);
  assert(strings_equal(empty_command_output, ""));
  free(empty_command_output);

  magpie_destroy(mp);
}

void test_cmd_api(void) {
  test_cmd_api_create_failure();
  test_cmd_api_run_commands();
  test_cmd_api_errors();
}

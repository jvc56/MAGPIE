#include "peg_command_test.h"

#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>

// Runs the `peg` command on a 1-in-bag CGP and asserts it completes
// without an error, that running it again with -pegpv true also succeeds,
// and that running it on a position that isn't 1-in-bag returns
// ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY without crashing or printing a result.
//
// Position from test/peg_test.c (NWL20, mover ENOSTXY, 1 tile in bag).
#define PEG_ONE_IN_BAG_CGP                                                     \
  "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"       \
  "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"             \
  "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20"

#define PEG_NON_ONE_IN_BAG_CGP                                                 \
  "cgp 15/15/15/15/15/15/15/7CAT5/15/15/15/15/15/15/15 "                       \
  "ABCDEFG/HIJKLMN 0/0 0 -lex NWL20"

// Executes a command without aborting on error; returns the top error code.
static error_code_t exec_returning_error(Config *config, const char *cmd) {
  ErrorStack *error_stack = error_stack_create();
  config_load_command(config, cmd, error_stack);
  if (error_stack_is_empty(error_stack)) {
    config_execute_command(config, error_stack);
  }
  error_code_t code = error_stack_top(error_stack);
  if (code != ERROR_STATUS_SUCCESS) {
    error_stack_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  return code;
}

static void test_peg_command_rejects_non_one_in_bag(void) {
  Config *config = config_create_or_die("set -mode sync -s1 score -s2 score");
  load_and_exec_config_or_die(config, PEG_NON_ONE_IN_BAG_CGP);
  error_code_t code = exec_returning_error(config, "peg");
  assert(code == ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY);
  config_destroy(config);
}

static void test_peg_command_smoke(void) {
  Config *config = config_create_or_die("set -mode sync -s1 score -s2 score");
  load_and_exec_config_or_die(config, PEG_ONE_IN_BAG_CGP);
  load_and_exec_config_or_die(config, "peg");
  config_destroy(config);
}

static void test_peg_command_pegpv_setting(void) {
  Config *config = config_create_or_die("set -mode sync -s1 score -s2 score");
  load_and_exec_config_or_die(config, PEG_ONE_IN_BAG_CGP);
  // Toggling -pegpv true should not error and the subsequent peg run should
  // produce per-scenario blocks (not asserted directly here; covered by
  // visual inspection / future golden tests).
  load_and_exec_config_or_die(config, "set -pegpv true");
  load_and_exec_config_or_die(config, "peg");
  config_destroy(config);
}

void test_peg_command(void) {
  test_peg_command_rejects_non_one_in_bag();
  test_peg_command_smoke();
  test_peg_command_pegpv_setting();
}

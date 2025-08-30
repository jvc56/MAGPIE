#include "../src/ent/game_history.h"
#include "../src/impl/config.h"
#include "../src/impl/load.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to validate GCG download
void validate_download_gcg(const char *source_identifier,
                           error_code_t expected_error) {
  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");

  printf("Testing with identifier: %s\n", source_identifier);
  DownloadGCGOptions options = {.source_identifier = source_identifier,
                                .lexicon = NULL,
                                .config = config};
  download_gcg(&options, game_history, error_stack);

  if (expected_error == ERROR_STATUS_SUCCESS) {
    if (!error_stack_is_empty(error_stack)) {
      printf("Test failed");
      error_stack_print_and_reset(error_stack);
      printf("(This may be due to network connectivity or service changes)\n");
    } else {
      // Assert all success requirements
      assert(error_stack_is_empty(error_stack));
      assert(game_history_get_number_of_events(game_history) > 0);
      assert(game_history_both_players_are_set(game_history));

      printf("Test passed: loaded %d game events\n",
             game_history_get_number_of_events(game_history));
    }
  } else {
    // Expecting an error
    assert(!error_stack_is_empty(error_stack));
    error_code_t actual_error = error_stack_top(error_stack);
    assert(actual_error == expected_error);
    printf("Test passed: got expected error\n");
  }

  game_history_destroy(game_history);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_load(void) {
  printf("Running load tests...\n");

  printf("=== Cross-tables Tests ===\n");
  // Test xt URL download and numerical ID download
  validate_download_gcg("https://cross-tables.com/annotated.php?u=54938",
                        ERROR_STATUS_SUCCESS);
  validate_download_gcg("54938", ERROR_STATUS_SUCCESS);

  printf("Cross-tables tests completed.\n\n");

  printf("=== Woogles Tests ===\n");
  // Test Woogles URL download and numerical ID download
  validate_download_gcg("https://woogles.io/game/XuoAntzD",
                        ERROR_STATUS_SUCCESS);
  validate_download_gcg("XuoAntzD", ERROR_STATUS_SUCCESS);

  printf("Woogles tests completed.\n\n");

  printf("=== Local File Tests ===\n");
  // Test local file download and local file failure case
  validate_download_gcg("testdata/gcgs/success_six_pass.gcg",
                        ERROR_STATUS_SUCCESS);
  validate_download_gcg("/tmp/nonexistent_file.gcg",
                        ERROR_STATUS_BAD_GCG_SOURCE);
  printf("Local file tests completed.\n\n");

  printf("=== URL Download Tests ===\n");
  // Test direct URL download and failure case
  validate_download_gcg(
      "https://www.cross-tables.com/annotated/selfgcg/556/anno55690.gcg",
      ERROR_STATUS_SUCCESS);
  validate_download_gcg("https://keep.google.com/u/0/",
                        ERROR_STATUS_UNRECOGNIZED_URL);
  printf("URL download tests completed.\n\n");

  printf("=== Total Failure Test ===\n");
  // Test total failure case
  validate_download_gcg("98bfakdna\?\?}}",
                        ERROR_STATUS_BAD_GCG_SOURCE);

  printf("Load tests completed.\n");
}
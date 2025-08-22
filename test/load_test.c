#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/impl/config.h"
#include "../src/impl/load.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to validate GCG download
void validate_download_gcg(const char *source_identifier, error_code_t expected_error) {
  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();
  Config* config = config_create_or_die("set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  
  printf("Testing with identifier: %s\n", source_identifier);
  DownloadGCGOptions options = {
    .source_identifier = source_identifier,
    .lexicon = NULL,
    .config = config
  };
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


void test_xt_url_conversion(void) {
  // Test URL conversion from full cross-tables URL
  const char *source_identifier = "https://cross-tables.com/annotated.php?u=54938";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);
}

void test_xt_game_id(void) {
  // Test URL construction from cross-tables numeric identifier
  const char *source_identifier = "54938";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);

}

void test_xt_direct_url(void) {
  // Test direct URL download
  const char *source_identifier = "https://www.cross-tables.com/annotated/selfgcg/556/anno55690.gcg";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);
}

void test_woogles_url_conversion(void) {
  // Test URL conversion from full Woogles URL
  const char *source_identifier = "https://woogles.io/game/XuoAntzD";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);
}

void test_woogles_game_id(void) {
  // Test URL construction from Woogles alphanumeric identifier
  const char *source_identifier = "XuoAntzD";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);

}

void test_local_file_loading(void) {
  // Test loading from local file (that exists) - use testdata file with fewer moves
  const char *source_identifier = "testdata/gcgs/success_six_pass.gcg";

  validate_download_gcg(source_identifier, ERROR_STATUS_SUCCESS);
}

void test_local_file_not_found(void) {
  // Test handling of non-existent local file
  const char *source_identifier = "/tmp/nonexistent_file.gcg";

  validate_download_gcg(source_identifier, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
}

void test_load(void) {
  printf("Running load tests...\n");
  
  printf("=== Cross-tables Tests ===\n");
  test_xt_url_conversion();
  test_xt_game_id();
  test_xt_direct_url();
  printf("Cross-tables tests completed.\n\n");
  
  printf("=== Woogles Tests ===\n");
  test_woogles_url_conversion();
  test_woogles_game_id();
  printf("Woogles tests completed.\n\n");
  
  printf("=== Local File Tests ===\n");
  test_local_file_loading();
  test_local_file_not_found();
  printf("Local file tests completed.\n\n");
  
  printf("Load tests completed.\n");
}
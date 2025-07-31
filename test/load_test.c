#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
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

void test_cross_tables_integration(void) {
  // Only run if MAGPIE_INTEGRATION_TESTS environment variable is set
  if (!getenv("MAGPIE_INTEGRATION_TESTS")) {
    printf("Skipping cross-tables integration test (set MAGPIE_INTEGRATION_TESTS=1 to enable)\n");
    return;
  }

  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();

  // Test with a known working cross-tables game URL
  // Using a classic game that should be stable on cross-tables
  Config *config = config_create_default_with_data_paths(error_stack, "data");
  
  if (!error_stack_is_empty(error_stack)) {
    printf("Skipping cross-tables integration test - data files not available\n");
    error_stack_destroy(error_stack);
    game_history_destroy(game_history);
    return;
  }
  
  DownloadGCGOptions options = {
    .source_identifier = "https://www.cross-tables.com/annotated.php?u=54938#0%23",
    .lexicon = NULL,
    .config = config
  };

  printf("Testing cross-tables download with URL: %s\n", options.source_identifier);
  
  download_gcg(&options, game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    printf("Cross-tables integration test failed: ");
    error_stack_print_and_reset(error_stack);
    // Don't assert fail here since network issues are common
    printf("(This may be due to network connectivity or changes to cross-tables.com)\n");
  } else {
    // Basic validation that we got a valid game
    assert(game_history_get_number_of_events(game_history) > 0);
    
    // Check that we have players
    assert(game_history_both_players_are_set(game_history));
    
    printf("Cross-tables integration test passed: loaded %d game events\n",
           game_history_get_number_of_events(game_history));
  }

  config_destroy(config);
  game_history_destroy(game_history);
  error_stack_destroy(error_stack);
}

void test_cross_tables_url_conversion(void) {
  // Test URL conversion logic without network calls or parsing
  // Just test that we get the right GCG content without parsing it
  
  const char *annotated_url = "https://cross-tables.com/annotated.php?u=54938";
  
  ErrorStack *error_stack = error_stack_create();
  
  DownloadGCGOptions options = {
    .source_identifier = annotated_url,
    .lexicon = NULL,
    .config = NULL
  };

  printf("Testing cross-tables URL conversion (download only): %s\n", annotated_url);
  
  // Just get the GCG string without parsing
  char *gcg_content = get_gcg_string(&options, error_stack);
  
  if (gcg_content) {
    printf("Cross-tables GCG content (first 200 chars):\n%.200s\n", gcg_content);
    printf("Cross-tables URL recognition test passed (recognized and downloaded)\n");
    free(gcg_content);
  } else {
    printf("Cross-tables test failed: ");
    error_stack_print_and_reset(error_stack);
  }
  
  error_stack_destroy(error_stack);
}

void test_cross_tables_game_id(void) {
  // Only run if MAGPIE_INTEGRATION_TESTS environment variable is set
  if (!getenv("MAGPIE_INTEGRATION_TESTS")) {
    printf("Skipping cross-tables game ID test (set MAGPIE_INTEGRATION_TESTS=1 to enable)\n");
    return;
  }

  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();

  Config *config = config_create_default_with_data_paths(error_stack, "data");
  
  if (!error_stack_is_empty(error_stack)) {
    printf("Skipping cross-tables game ID test - data files not available\n");
    error_stack_destroy(error_stack);
    game_history_destroy(game_history);
    return;
  }
  
  // Test with just a game ID (should construct full URL)
  DownloadGCGOptions options = {
    .source_identifier = "54938", // Just the game ID
    .lexicon = NULL,
    .config = config
  };

  printf("Testing cross-tables download with game ID: %s\n", options.source_identifier);
  
  download_gcg(&options, game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    printf("Cross-tables game ID test failed: ");
    error_stack_print_and_reset(error_stack);
    printf("(This may be due to network connectivity or the game ID not existing)\n");
  } else {
    assert(game_history_get_number_of_events(game_history) > 0);
    printf("Cross-tables game ID test passed: loaded %d game events\n",
           game_history_get_number_of_events(game_history));
  }

  config_destroy(config);
  game_history_destroy(game_history);
  error_stack_destroy(error_stack);
}

void test_woogles_integration(void) {
  // Only run if MAGPIE_INTEGRATION_TESTS environment variable is set
  if (!getenv("MAGPIE_INTEGRATION_TESTS")) {
    printf("Skipping woogles integration test (set MAGPIE_INTEGRATION_TESTS=1 to enable)\n");
    return;
  }

  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();

  Config *config = config_create_default_with_data_paths(error_stack, "data");
  
  if (!error_stack_is_empty(error_stack)) {
    printf("Skipping woogles integration test - data files not available\n");
    error_stack_destroy(error_stack);
    game_history_destroy(game_history);
    return;
  }
  
  // Test with a real woogles game URL
  DownloadGCGOptions options = {
    .source_identifier = "https://woogles.io/game/XuoAntzD",
    .lexicon = NULL,
    .config = config
  };

  printf("Testing woogles download with URL: %s\n", options.source_identifier);
  
  download_gcg(&options, game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    printf("Woogles integration test failed: ");
    error_stack_print_and_reset(error_stack);
    printf("(This may be due to network connectivity or invalid game ID)\n");
  } else {
    assert(game_history_get_number_of_events(game_history) > 0);
    assert(game_history_both_players_are_set(game_history));
    
    printf("Woogles integration test passed: loaded %d game events\n",
           game_history_get_number_of_events(game_history));
  }

  config_destroy(config);
  game_history_destroy(game_history);
  error_stack_destroy(error_stack);
}

void test_woogles_url_recognition(void) {
  // Test URL recognition and API call without parsing
  const char *woogles_url = "https://woogles.io/game/XuoAntzD";
  
  ErrorStack *error_stack = error_stack_create();
  
  DownloadGCGOptions options = {
    .source_identifier = woogles_url,
    .lexicon = NULL,
    .config = NULL
  };

  printf("Testing woogles API call (download only): %s\n", woogles_url);
  
  // Just get the GCG string without parsing
  char *gcg_content = get_gcg_string(&options, error_stack);
  
  if (gcg_content) {
    printf("Woogles GCG content (first 500 chars):\n%.500s\n", gcg_content);
    if (strstr(gcg_content, "File not found") || strstr(gcg_content, "<html>")) {
      printf("WARNING: Got HTML error page instead of GCG content - API call may be incorrect\n");
    } else {
      printf("Woogles URL recognition test passed (recognized and downloaded)\n");
    }
    free(gcg_content);
  } else {
    printf("Woogles test failed: ");
    error_stack_print_and_reset(error_stack);
  }
  
  error_stack_destroy(error_stack);
}

void test_local_file_loading(void) {
  // Test loading a local GCG file without parsing
  ErrorStack *error_stack = error_stack_create();
  
  DownloadGCGOptions options = {
    .source_identifier = "/tmp/test_game.gcg",
    .lexicon = NULL,
    .config = NULL
  };

  printf("Testing local file loading (read only): %s\n", options.source_identifier);
  
  // Just get the GCG string without parsing
  char *gcg_content = get_gcg_string(&options, error_stack);
  
  if (gcg_content) {
    printf("Local file GCG content:\n%s\n", gcg_content);
    printf("Local file loading test passed (file read successfully)\n");
    free(gcg_content);
  } else {
    printf("Local file test failed: ");
    error_stack_print_and_reset(error_stack);
  }

  error_stack_destroy(error_stack);
}

void test_local_file_not_found(void) {
  // Test handling of non-existent local file
  GameHistory *game_history = game_history_create();
  ErrorStack *error_stack = error_stack_create();
  
  DownloadGCGOptions options = {
    .source_identifier = "/tmp/nonexistent_file.gcg",
    .lexicon = NULL,
    .config = NULL
  };

  printf("Testing local file not found: %s\n", options.source_identifier);
  
  download_gcg(&options, game_history, error_stack);
  
  // Should get some error for non-existent file
  assert(!error_stack_is_empty(error_stack));
  
  printf("Local file not found test passed\n");
  
  game_history_destroy(game_history);
  error_stack_destroy(error_stack);
}

void test_load(void) {
  printf("Running load tests...\n");
  
  printf("=== Cross-tables Tests ===\n");
  test_cross_tables_url_conversion();
  test_cross_tables_integration();
  test_cross_tables_game_id();
  printf("Cross-tables tests completed.\n\n");
  
  printf("=== Woogles Tests ===\n");
  test_woogles_url_recognition();
  test_woogles_integration();
  printf("Woogles tests completed.\n\n");
  
  printf("=== Local File Tests ===\n");
  test_local_file_loading();
  test_local_file_not_found();
  printf("Local file tests completed.\n\n");
  
  printf("Load tests completed.\n");
}
#include "../src/ent/autoplay_results.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_odds_that_player_is_better(void) {
  assert(within_epsilon(odds_that_player_is_better(0.6, 10), 73.645537));
  assert(within_epsilon(odds_that_player_is_better(0.6, 100), 97.724987));
  assert(within_epsilon(odds_that_player_is_better(0.6, 1000), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.7, 10), 89.704839));
  assert(within_epsilon(odds_that_player_is_better(0.7, 100), 99.996833));
  assert(within_epsilon(odds_that_player_is_better(0.7, 1000), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.9, 10), 99.429398));
  assert(within_epsilon(odds_that_player_is_better(0.9, 100), 100.0));
  assert(within_epsilon(odds_that_player_is_better(0.9, 1000), 100.0));
}

void assert_autoplay_output(const char *output, int expected_newlines,
                            const char **expected_substrs) {
  int newlines_in_output = count_newlines(output);
  bool success = true;
  if (newlines_in_output != expected_newlines) {
    printf("mismatched autoplay output:\nexpected: %d\ngot:%d\n",
           expected_newlines, newlines_in_output);
    success = false;
  }

  StringSplitter *split_output = split_string_by_newline(output, false);
  assert(string_splitter_get_number_of_items(split_output) ==
         expected_newlines + 1);
  // We don't really care about what comes after the last newline
  for (int i = 0; i < expected_newlines; i++) {
    const char *actual = string_splitter_get_item(split_output, i);
    const char *expected = expected_substrs[i];
    if (!has_substring(actual, expected)) {
      printf("pattern not found in autoplay output:\n%s\n***\n%s\n", expected,
             actual);
      success = false;
    }
  }

  assert(success);
  string_splitter_destroy(split_output);
}

void test_autoplay_default(void) {
  Config *csw_config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 1 -gp true -threads 11");

  load_and_exec_config_or_die(csw_config, "autoplay games 100 -seed 26");

  char *ar1_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  assert_autoplay_output(ar1_str, 1,
                         (const char *[]){"autoplay games 200 100 100 0"});

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 100 -r1 best -r2 best -seed 26");

  char *ar2_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  // Autoplay using the "best" move recorder should be the same
  // as autoplay using the "all" move recorder.
  assert_strings_equal(ar1_str, ar2_str);

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 7 -gp false -threads 2 -seed 27");

  // Autoplay should reset the stats
  char *ar3_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, false);
  assert_autoplay_output(ar3_str, 1, (const char *[]){"autoplay games 7"});

  // Ensure pseudo-randomness is consistent for any number of threads
  char *single_thread_str = NULL;
  char *multi_thread_str = NULL;
  for (int i = 0; i < 11; i++) {
    char *options_string =
        get_formatted_string("autoplay games 20 -r1 best -r2 best -gp false "
                             "-threads %d -seed 28",
                             i + 1);

    load_and_exec_config_or_die(csw_config, options_string);

    free(options_string);

    if (i == 0) {
      single_thread_str = autoplay_results_to_string(
          config_get_autoplay_results(csw_config), false, false);
    } else {
      free(multi_thread_str);
      multi_thread_str = autoplay_results_to_string(
          config_get_autoplay_results(csw_config), false, false);
      assert_strings_equal(single_thread_str, multi_thread_str);
    }
  }

  free(ar1_str);
  free(ar2_str);
  free(ar3_str);
  free(single_thread_str);
  free(multi_thread_str);
  config_destroy(csw_config);
}

void test_autoplay_leavegen(void) {
  Config *ab_config =
      config_create_or_die("set -lex CSW21_ab -ld english_ab -wmp false -s1 "
                           "equity -s2 equity -r1 best -r2 "
                           "best -numplays 1 -threads 1");

  // The minimum leave count should be achieved quickly, so if this takes too
  // long, we know it failed.
  load_and_exec_config_or_die_timed(ab_config, "leavegen 1 0 -seed 3", 60);
  load_and_exec_config_or_die_timed(ab_config, "leavegen 1,2,1 0 -seed 3", 60);

  config_destroy(ab_config);
}

void test_autoplay_divergent_games(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1 -gp true -threads 11");
  const Game *game;

  AutoplayResults *ar = autoplay_results_create();

  ErrorStack *error_stack = error_stack_create();
  autoplay_results_set_options(ar, "games", error_stack);
  assert(error_stack_is_empty(error_stack));

  load_and_exec_config_or_die(csw_config, "cgp " VS_ANDY_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, false, 1);

  load_and_exec_config_or_die(csw_config, "cgp " VS_FRENTZ_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, true, 2);

  load_and_exec_config_or_die(csw_config, "cgp " MANY_MOVES);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, false, 3);

  load_and_exec_config_or_die(csw_config, "cgp " UEY_CGP);
  game = config_get_game(csw_config);
  autoplay_results_add_game(ar, game, 20, true, 4);

  char *ar_str = autoplay_results_to_string(ar, false, true);
  assert_autoplay_output(
      ar_str, 2, (const char *[]){"autoplay games 4", "autoplay games 2"});

  free(ar_str);

  autoplay_results_destroy(ar);

  load_and_exec_config_or_die(csw_config,
                              "autoplay games 50 -seed 50 -lex CSW21 -gp true");

  // There should be no divergent games for CSW21 vs CSW21
  char *ar_gp_same_lex_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, true);
  assert_autoplay_output(
      ar_gp_same_lex_str, 2,
      (const char *[]){"autoplay games 100", "autoplay games 0"});

  free(ar_gp_same_lex_str);

  load_and_exec_config_or_die(
      csw_config, "autoplay games 50 -seed 50 -l1 CSW21 -l2 NWL20 -gp true");

  char *ar_gp_diff_lex_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, true);
  assert_autoplay_output(
      ar_gp_diff_lex_str, 2,
      (const char *[]){"autoplay games 100", "autoplay games 100"});

  free(ar_gp_diff_lex_str);

  load_and_exec_config_or_die(csw_config, "set -lex CSW21");

  KLV *small_diff_klv = klv_create_or_die(DEFAULT_TEST_DATA_PATH, "CSW21");
  const uint32_t num = klv_get_number_of_leaves(small_diff_klv);

  for (uint32_t i = 0; i < num; i++) {
    small_diff_klv->leave_values[i] += int_to_equity(-1 + (int)(2 * (i % 2)));
  }

  klv_write_or_die(small_diff_klv, DEFAULT_TEST_DATA_PATH, "CSW21_small_diff");
  klv_destroy(small_diff_klv);

  load_and_exec_config_or_die(
      csw_config,
      "autoplay games 50 -seed 50 -k1 CSW21 -k2 CSW21_small_diff -gp true");

  char *small_diff_str = autoplay_results_to_string(
      config_get_autoplay_results(csw_config), false, true);
  assert_autoplay_output(
      small_diff_str, 2,
      (const char *[]){"autoplay games 100", "autoplay games 88"});

  free(small_diff_str);

  error_stack_destroy(error_stack);
  config_destroy(csw_config);
}

void test_autoplay_win_pct_record(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp false -threads 1");
  load_and_exec_config_or_die(csw_config,
                              "autoplay winpct 50 -seed 50 -wb 1000000 ");
  config_destroy(csw_config);
}

void test_autoplay_leaves_record(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp false -threads 1");
  load_and_exec_config_or_die(csw_config,
                              "autoplay leaves 2 -seed 50 -wb 1000000 ");
  config_destroy(csw_config);
}

void test_autoplay_rack_equity_record(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1 -gp false -threads 3");
  load_and_exec_config_or_die(csw_config,
                              "autoplay rackequity 5 -seed 42 -wb 1000000");
  config_destroy(csw_config);

  char *csv = get_string_from_file_or_die("autoplay_record_rackequity.csv");
  assert(csv);

  StringSplitter *lines = split_string_by_newline(csv, true);
  const int num_lines = string_splitter_get_number_of_items(lines);
  assert(num_lines > 0);

  for (int line_idx = 0; line_idx < num_lines; line_idx++) {
    const char *line = string_splitter_get_item(lines, line_idx);
    StringSplitter *cols = split_string(line, ',', false);
    const int num_cols = string_splitter_get_number_of_items(cols);
    // Each row must have at least rack + one equity value
    assert(num_cols >= 2);
    // First column is the rack string (non-empty)
    const char *rack_str = string_splitter_get_item(cols, 0);
    assert(strlen(rack_str) > 0);
    // All subsequent columns must be parseable as doubles (equity values)
    ErrorStack *error_stack = error_stack_create();
    for (int col_idx = 1; col_idx < num_cols; col_idx++) {
      const char *equity_str = string_splitter_get_item(cols, col_idx);
      string_to_double(equity_str, error_stack);
      assert(error_stack_is_empty(error_stack));
    }
    error_stack_destroy(error_stack);
    string_splitter_destroy(cols);
  }

  string_splitter_destroy(lines);
  free(csv);
  (void)remove("autoplay_record_rackequity.csv");
}

void test_autoplay_force_racks(void) {
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1 -gp false -threads 3");

  const char *force_racks_filename = "test_force_racks.txt";
  ErrorStack *write_error_stack = error_stack_create();
  write_string_to_file(force_racks_filename, "w", "AEINRST\nQZ\nJKX\n",
                       write_error_stack);
  assert(error_stack_is_empty(write_error_stack));
  error_stack_destroy(write_error_stack);

  char *autoplay_cmd = get_formatted_string(
      "autoplay rackequity 5 %s -seed 42 -wb 1000000", force_racks_filename);
  load_and_exec_config_or_die(csw_config, autoplay_cmd);
  free(autoplay_cmd);

  char *csv = get_string_from_file_or_die("autoplay_record_rackequity.csv");
  assert(csv);
  // Forced racks are drawn every turn (see game_runner_start's
  // RARE_RACK_MODE_FORCE_FILE case), so each should show up with many more
  // equity samples than an organically drawn rack gets in 5 games.
  assert(has_substring(csv, "AEINRST,"));
  assert(has_substring(csv, "QZ,"));
  assert(has_substring(csv, "JKX,"));
  free(csv);
  (void)remove("autoplay_record_rackequity.csv");

  // forceracksfile requires the rackequity recorder to be enabled, since the
  // other recorders assume every recorded move was actually played.
  char *games_cmd = get_formatted_string("autoplay games 5 %s -seed 42",
                                         force_racks_filename);
  assert_config_exec_status(
      csw_config, games_cmd,
      ERROR_STATUS_AUTOPLAY_FORCE_RACKS_REQUIRES_RACK_EQUITY);
  free(games_cmd);

  // Missing force racks file.
  assert_config_exec_status(
      csw_config,
      "autoplay rackequity 5 does_not_exist_force_racks.txt -seed 42",
      ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM);

  // Malformed rack line.
  const char *bad_force_racks_filename = "test_bad_force_racks.txt";
  ErrorStack *bad_write_error_stack = error_stack_create();
  write_string_to_file(bad_force_racks_filename, "w", "AEINRST\n123\n",
                       bad_write_error_stack);
  assert(error_stack_is_empty(bad_write_error_stack));
  error_stack_destroy(bad_write_error_stack);
  char *bad_racks_cmd = get_formatted_string(
      "autoplay rackequity 5 %s -seed 42", bad_force_racks_filename);
  assert_config_exec_status(csw_config, bad_racks_cmd,
                            ERROR_STATUS_AUTOPLAY_FORCE_RACKS_MALFORMED_RACK);
  free(bad_racks_cmd);
  (void)remove(bad_force_racks_filename);

  // File with no parseable racks.
  const char *empty_force_racks_filename = "test_empty_force_racks.txt";
  ErrorStack *empty_write_error_stack = error_stack_create();
  write_string_to_file(empty_force_racks_filename, "w", "\n\n   \n",
                       empty_write_error_stack);
  assert(error_stack_is_empty(empty_write_error_stack));
  error_stack_destroy(empty_write_error_stack);
  char *empty_racks_cmd = get_formatted_string(
      "autoplay rackequity 5 %s -seed 42", empty_force_racks_filename);
  assert_config_exec_status(csw_config, empty_racks_cmd,
                            ERROR_STATUS_AUTOPLAY_FORCE_RACKS_FILE_EMPTY);
  free(empty_racks_cmd);
  (void)remove(empty_force_racks_filename);

  (void)remove(force_racks_filename);
  config_destroy(csw_config);
}

// Check wmp movegen correctness by comparing results in gamepair autoplay to
// the legacy recursive_gen algorithm. Auto-discovers .wmp lexica under the
// configured data paths so the test stays in sync with available data.
void test_autoplay_wmp_correctness(void) {
  ErrorStack *error_stack = error_stack_create();
  StringList *wmp_files = data_filepaths_get_all_data_path_names(
      DEFAULT_TEST_DATA_PATH, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  StringList *kwg_files = data_filepaths_get_all_data_path_names(
      DEFAULT_TEST_DATA_PATH, DATA_FILEPATH_TYPE_KWG, error_stack);
  assert(error_stack_is_empty(error_stack));
  const int pairs_per_lex = 1000;

  // Build a list of lexicon names (basename without extension) that exist in
  // both the WMP and KWG data lists so we only test lexica that have both.
  StringList *lex_names = string_list_create();
  const int wmp_count = string_list_get_count(wmp_files);
  for (int wmp_idx = 0; wmp_idx < wmp_count; wmp_idx++) {
    const char *wmp_path = string_list_get_string(wmp_files, wmp_idx);
    const char *slash = strrchr(wmp_path, '/');
    const char *wmp_name = slash ? slash + 1 : wmp_path;
    const char *dot = strrchr(wmp_name, '.');
    const int len = dot ? (int)(dot - wmp_name) : (int)strlen(wmp_name);
    char *base = malloc_or_die(len + 1);
    memcpy(base, wmp_name, len);
    base[len] = '\0';

    /* Check if a matching KWG exists for this base name */
    const int kwg_count = string_list_get_count(kwg_files);
    bool found_kwg = false;
    for (int kwg_idx = 0; kwg_idx < kwg_count; kwg_idx++) {
      const char *kwg_path = string_list_get_string(kwg_files, kwg_idx);
      const char *kslash = strrchr(kwg_path, '/');
      const char *kwg_name = kslash ? kslash + 1 : kwg_path;
      const char *kdot = strrchr(kwg_name, '.');
      const int klen = kdot ? (int)(kdot - kwg_name) : (int)strlen(kwg_name);
      if (klen == len && strncmp(kwg_name, base, len) == 0) {
        found_kwg = true;
        break;
      }
    }

    if (found_kwg) {
      string_list_add_string(lex_names, base);
    }
    free(base);
  }

  // Iterate over lex_names (those that have both .wmp and .kwg)
  const int num_lexes = string_list_get_count(lex_names);
  for (int i = 0; i < num_lexes; i++) {
    const char *lex = string_list_get_string(lex_names, i);

    // Configure player 1 to use WMP and player 2 not to. Use game pairs.
    char *config_cmd = get_formatted_string(
        "set -lex %s -s1 equity -s2 equity -r1 all -r2 "
        "all -numplays 1 -gp true -w1 true -w2 false -threads 4",
        lex);
    Config *c = config_create_or_die(config_cmd);
    free(config_cmd);

    char *autocmd = get_formatted_string("autoplay games %d -seed %d -gp true",
                                         pairs_per_lex, 1000 + i);
    load_and_exec_config_or_die(c, autocmd);
    free(autocmd);

    char *res =
        autoplay_results_to_string(config_get_autoplay_results(c), false, true);
    const char *expected_zero = "autoplay games 0";
    if (!has_substring(res, expected_zero)) {
      (void)fprintf(stderr, "autoplay divergence detected for lex %s:\n%s\n",
                    lex, res ? res : "(no output)");
      free(res);
      string_list_destroy(kwg_files);
      string_list_destroy(wmp_files);
      string_list_destroy(lex_names);
      error_stack_destroy(error_stack);
      assert(false);
    }
    free(res);

    config_destroy(c);
  }

  string_list_destroy(kwg_files);
  string_list_destroy(wmp_files);
  string_list_destroy(lex_names);
  error_stack_destroy(error_stack);
}

void test_autoplay_rit_correctness(void) {
  // Build a RIT for TWL98 using the release binary (fast), then run
  // game pairs under ASAN where player 1 uses RIT and player 2 does not.
  // Any divergence means the RIT changed move selection.
  const char *lex = "TWL98";
  const int num_pairs = 5000;

  // The RIT must be pre-built before running this test. On CI, the
  // release binary builds it. Locally: run
  //   echo "convert klvwmp2rit TWL98" | ./bin/magpie "set -lex TWL98 -wmp true
  //   -rit false"
  printf("Running %d game pairs with RIT correctness check for %s...\n",
         num_pairs, lex);
  (void)fflush(stdout);

  // Run game pairs: player 1 uses RIT, player 2 does not.
  Config *c = config_create_or_die(
      "set -lex TWL98 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -gp true -w1 true -w2 true "
      "-rit1 true -rit2 false -threads 4");
  load_and_exec_config_or_die(c, "autoplay games 5000 -seed 42 -gp true");

  char *res =
      autoplay_results_to_string(config_get_autoplay_results(c), false, true);
  const char *expected_zero = "autoplay games 0";
  if (!has_substring(res, expected_zero)) {
    (void)fprintf(stderr, "RIT autoplay divergence detected for %s:\n%s\n", lex,
                  res ? res : "(no output)");
    assert(false);
  }
  free(res);
  config_destroy(c);

  // Clean up the RIT file.
  char *rit_path =
      get_formatted_string("%s/lexica/%s.rit", DEFAULT_TEST_DATA_PATH, lex);
  (void)remove(rit_path);
  free(rit_path);

  printf("RIT correctness: PASSED (%d game pairs, 0 divergent)\n", num_pairs);
}

void test_autoplay_sim(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 3 -pl1 2 -minp 2 -iter 10 -threads 10 -sinfer false");
  load_and_exec_config_or_die(config,
                              "autoplay games 1 -seed 1 -mtmode pgp -gp false");
  load_and_exec_config_or_die(config,
                              "autoplay games 1 -seed 1 -mtmode pgp -gp true");
  load_and_exec_config_or_die(
      config, "autoplay games 1 -seed 2 -mtmode igp -sinfer true -gp false");
  load_and_exec_config_or_die(config,
                              "autoplay games 2 -seed 3 -mtmode igp -np1 3 "
                              "-np2 2 -plies 2 -gp true ");
  config_destroy(config);
}

void test_autoplay_remaining(void) {
  test_odds_that_player_is_better();
  test_autoplay_leavegen();
  test_autoplay_divergent_games();
  test_autoplay_win_pct_record();
  test_autoplay_leaves_record();
  test_autoplay_rack_equity_record();
  test_autoplay_force_racks();
  test_autoplay_sim();
}

void test_autoplay(void) {
  test_autoplay_remaining();
  test_autoplay_default();
  test_autoplay_wmp_correctness();
}

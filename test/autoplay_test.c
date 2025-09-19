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
  Config *csw_config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1  -gp true -threads 11");

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
  Config *ab_config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -s1 equity -s2 equity -r1 best -r2 "
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
                           "all -numplays 1  -gp true -threads 11");
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
  for (int wi = 0; wi < wmp_count; wi++) {
    const char *wmp_path = string_list_get_string(wmp_files, wi);
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
    for (int ki = 0; ki < kwg_count; ki++) {
      const char *kwg_path = string_list_get_string(kwg_files, ki);
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
    } else {
      free(base);
    }
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
    assert(has_substring(res, expected_zero));
    free(res);

    config_destroy(c);
  }

  string_list_destroy(kwg_files);
  string_list_destroy(wmp_files);
  string_list_destroy(lex_names);
  error_stack_destroy(error_stack);
}

void test_autoplay(void) {
  test_odds_that_player_is_better();
  test_autoplay_default();
  test_autoplay_leavegen();
  test_autoplay_divergent_games();
  test_autoplay_wmp_correctness();
  test_autoplay_win_pct_record();
  test_autoplay_leaves_record();
}

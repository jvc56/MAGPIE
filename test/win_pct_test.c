#include "win_pct_test.h"

#include "../src/def/rack_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/win_pct_counts.h"
#include "../src/impl/config.h"
#include "../src/impl/win_pct_smoother.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void assert_win_pct_get(const float actual, const double expected) {
  assert(within_epsilon(actual, expected));
}

static const WinPct *load_config_win_pcts_or_die(Config *config) {
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return config_get_win_pcts(config);
}

void test_win_pct(void) {
  // Without -winpct the table is the letter distribution's.
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const WinPct *win_pct = load_config_win_pcts_or_die(config);
  assert(strings_equal(win_pct_get_name(win_pct), "winpct_english"));
  assert(win_pct_get_max_bag(win_pct) == 86);

  // At the opening (86 in the bag, both racks full) the player on turn is
  // slightly favored and expects to outscore the opponent from here, and a
  // lead of 500 is decided either way.
  const double opening_tied = win_pct_get(win_pct, 0, 86, RACK_SIZE, RACK_SIZE);
  assert(opening_tied > 0.5 && opening_tied < 0.6);
  assert(win_pct_get(win_pct, 500, 86, RACK_SIZE, RACK_SIZE) > 0.99);
  assert(win_pct_get(win_pct, -500, 86, RACK_SIZE, RACK_SIZE) < 0.01);
  const Equity opening_swing =
      win_pct_get_expected_swing(win_pct, 86, RACK_SIZE, RACK_SIZE);
  assert(opening_swing > 0 && opening_swing < int_to_equity(40));
  // Win percentages never fall as the lead grows.
  for (int spread = -499; spread <= 500; spread++) {
    assert(win_pct_get(win_pct, spread, 86, RACK_SIZE, RACK_SIZE) >=
           win_pct_get(win_pct, spread - 1, 86, RACK_SIZE, RACK_SIZE));
  }
  // With the bag empty, one tile against a full rack all but guarantees
  // going out first, which is worth more than tempo at the opening.
  assert(win_pct_get_expected_swing(win_pct, 0, 1, RACK_SIZE) > opening_swing);
  assert(win_pct_get(win_pct, 0, 0, 1, RACK_SIZE) > opening_tied);

  // -winpct names another table and default returns to the distribution's.
  load_and_exec_config_or_die(config, "set -winpct winpct_french");
  win_pct = load_config_win_pcts_or_die(config);
  assert(strings_equal(win_pct_get_name(win_pct), "winpct_french"));
  load_and_exec_config_or_die(config, "set -winpct default");
  win_pct = load_config_win_pcts_or_die(config);
  assert(strings_equal(win_pct_get_name(win_pct), "winpct_english"));
  config_destroy(config);
}

void test_win_pct_coverage(void) {
  // An explicitly chosen table is kept across a lexicon change, and a
  // 102-tile bag (88 tiles in the bag at the start) outgrows the English
  // table, which covers bags of up to 86.
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1 "
      "-winpct winpct_english");
  assert_config_exec_status(config, "set -lex FRA20",
                            ERROR_STATUS_CONFIG_WIN_PCT_TOO_SMALL);
  config_destroy(config);

  // The default follows the distribution: loaded lazily when first needed,
  // and swapped when the lexicon changes.
  config = config_create_or_die(
      "set -lex FRA20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const WinPct *win_pct = load_config_win_pcts_or_die(config);
  assert(strings_equal(win_pct_get_name(win_pct), "winpct_french"));
  assert(win_pct_get_max_bag(win_pct) == 88);
  load_and_exec_config_or_die(config, "set -lex CSW21");
  win_pct = load_config_win_pcts_or_die(config);
  assert(strings_equal(win_pct_get_name(win_pct), "winpct_english"));
  config_destroy(config);

  // A distribution without a table of its own has no default.
  config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -wmp false -s1 score -s2 score -r1 "
      "all -r2 all -numplays 1");
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_top(error_stack) ==
         ERROR_STATUS_CONFIG_LOAD_WIN_PCT_ERROR);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

enum {
  STATE_TEST_MAX_BAG = 86,
  STATE_TEST_MAX_SPREAD = 500,
};

static void write_counts(const WinPctCounts *counts, const char *name) {
  char *path = get_formatted_string("./testdata/strategy/%s.csv", name);
  char *contents = win_pct_counts_get_string(counts);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(path, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  free(contents);
  free(path);
}

static void remove_counts(const char *name) {
  char *path = get_formatted_string("./testdata/strategy/%s.csv", name);
  (void)remove(path);
  free(path);
}

static WinPct *load_win_pct_or_die(const char *name) {
  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pct = win_pct_create(DEFAULT_TEST_DATA_PATH, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return win_pct;
}

void test_win_pct_state(void) {
  WinPctCounts *counts =
      win_pct_counts_create(STATE_TEST_MAX_BAG, STATE_TEST_MAX_SPREAD);
  const int bag_cell = win_pct_get_cell_index(50, RACK_SIZE, RACK_SIZE);
  const int endgame_cell = win_pct_get_cell_index(0, 1, RACK_SIZE);
  // Bag of 50: swings of -10, 10, 30, and 600 (past the table's spreads).
  win_pct_counts_add_swing(counts, 0, bag_cell, -10);
  win_pct_counts_add_swing(counts, 0, bag_cell, 10);
  win_pct_counts_add_swing(counts, 1, bag_cell, 30);
  win_pct_counts_add_swing(counts, 1, bag_cell, 600);
  // On turn with one tile against a full rack: swings of 20 and 40.
  win_pct_counts_add_swing(counts, 0, endgame_cell, 20);
  win_pct_counts_add_swing(counts, 1, endgame_cell, 40);
  write_counts(counts, "winpct_state_test");

  // The text form reads back to the same counts.
  char *contents = win_pct_counts_get_string(counts);
  ErrorStack *error_stack = error_stack_create();
  WinPctCounts *reread = win_pct_counts_create_from_string(
      contents, "winpct_state_test", error_stack);
  assert(error_stack_is_empty(error_stack));
  char *recontents = win_pct_counts_get_string(reread);
  assert(strings_equal(contents, recontents));
  free(recontents);
  free(contents);
  win_pct_counts_destroy(reread);

  WinPct *win_pct = load_win_pct_or_die("winpct_state_test");
  assert(win_pct_get_max_bag(win_pct) == STATE_TEST_MAX_BAG);
  // Ahead by 10 with swings -10, 10, 30, 600: one tie and three wins.
  assert_win_pct_get(win_pct_get(win_pct, 10, 50, RACK_SIZE, RACK_SIZE),
                     3.5 / 4);
  // Behind by 30: the tie and the 600.
  assert_win_pct_get(win_pct_get(win_pct, -30, 50, RACK_SIZE, RACK_SIZE),
                     1.5 / 4);
  // Behind by far more than any spread: only the 600 still wins.
  assert_win_pct_get(win_pct_get(win_pct, -700, 50, RACK_SIZE, RACK_SIZE),
                     1.0 / 4);
  // The mean swing is exact beyond the table's spreads.
  assert(win_pct_get_expected_swing(win_pct, 50, RACK_SIZE, RACK_SIZE) ==
         int_to_equity(630) / 4);
  assert(win_pct_get_expected_swing(win_pct, 0, 1, RACK_SIZE) ==
         int_to_equity(30));
  // Rack sizes don't matter while the bag holds tiles.
  assert(win_pct_get_expected_swing(win_pct, 50, 3, 2) ==
         int_to_equity(630) / 4);
  // A bag size without data takes its nearest neighbor's.
  assert(win_pct_get_expected_swing(win_pct, 60, RACK_SIZE, RACK_SIZE) ==
         int_to_equity(630) / 4);
  // A bag-empty state without data takes all bag-empty states pooled.
  assert(win_pct_get_expected_swing(win_pct, 0, RACK_SIZE, RACK_SIZE) ==
         int_to_equity(30));
  win_pct_destroy(win_pct);

  // Smoothing keeps each state's own observations and needs both samples.
  StringBuilder *report = string_builder_create();
  WinPctCounts *smoothed = win_pct_smooth(counts, report, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(within_epsilon(
      win_pct_counts_row_get_total(
          smoothed, win_pct_counts_get_const_row(smoothed, 0, bag_cell)),
      4.0));
  win_pct_counts_destroy(smoothed);
  WinPctCounts *one_sample =
      win_pct_counts_create(STATE_TEST_MAX_BAG, STATE_TEST_MAX_SPREAD);
  win_pct_counts_add_swing(one_sample, 0, bag_cell, 10);
  assert(win_pct_smooth(one_sample, report, error_stack) == NULL);
  assert(error_stack_top(error_stack) == ERROR_STATUS_WIN_PCT_NO_DATA_FOUND);
  error_stack_reset(error_stack);
  win_pct_counts_destroy(one_sample);
  string_builder_destroy(report);

  // A table with the wrong rack size is rejected.
  write_string_to_file("./testdata/strategy/winpct_state_test_bad_rack.csv",
                       "w", "winpct 2 6 86 500\n", error_stack);
  assert(error_stack_is_empty(error_stack));
  const WinPct *mismatched = win_pct_create(
      DEFAULT_TEST_DATA_PATH, "winpct_state_test_bad_rack", error_stack);
  assert(mismatched == NULL);
  error_stack_reset(error_stack);
  remove_counts("winpct_state_test_bad_rack");
  error_stack_destroy(error_stack);

  // The convert command smooths a recorded table into a loadable one.
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "convert winpct winpct_state_test winpct_state_test_smoothed");
  WinPct *smoothed_win_pct = load_win_pct_or_die("winpct_state_test_smoothed");
  assert(win_pct_get_max_bag(smoothed_win_pct) == STATE_TEST_MAX_BAG);
  win_pct_destroy(smoothed_win_pct);
  config_destroy(config);

  win_pct_counts_destroy(counts);
  remove_counts("winpct_state_test");
  remove_counts("winpct_state_test_smoothed");
}

void test_win_pct_record(void) {
  remove_counts("CSW21_winpct_record");
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-gp false -threads 2");
  load_and_exec_config_or_die(config, "autoplay winpct 40 -seed 51");
  // A second run adds to the first.
  load_and_exec_config_or_die(config, "autoplay winpct 40 -seed 52");
  config_destroy(config);

  ErrorStack *error_stack = error_stack_create();
  char *contents = get_string_from_file(
      "./testdata/strategy/CSW21_winpct_record.csv", error_stack);
  WinPctCounts *counts = win_pct_counts_create_from_string(
      contents, "CSW21_winpct_record", error_stack);
  assert(error_stack_is_empty(error_stack));
  // Every game records its first position, a full bag, in one sample or the
  // other (and an opening exchange or pass records another).
  const int first_cell = win_pct_get_cell_index(
      win_pct_counts_get_max_bag(counts), RACK_SIZE, RACK_SIZE);
  double first_position_total = 0.0;
  for (int sample = 0; sample < WIN_PCT_NUM_SAMPLES; sample++) {
    first_position_total += win_pct_counts_row_get_total(
        counts, win_pct_counts_get_const_row(counts, sample, first_cell));
  }
  assert(first_position_total >= 80.0 && first_position_total < 100.0);
  win_pct_counts_destroy(counts);
  error_stack_destroy(error_stack);
  free(contents);
  remove_counts("CSW21_winpct_record");
}

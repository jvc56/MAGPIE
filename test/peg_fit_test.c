#include "peg_fit_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/rack_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/ent/peg_adjust.h"
#include "../src/impl/config.h"
#include "../src/impl/peg_fit.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  PEG_FIT_TEST_POSITIONS = 60,
  // The most tiles a play of the standard table leaves in the bag, which is
  // the fit's default.
  PEG_FIT_TEST_STANDARD_TILES_LEFT =
      PEG_ADJUST_STANDARD_VALUES_LENGTH - RACK_SIZE - 1,
  // A longer table, which reaches the plays that leave up to two racks.
  PEG_FIT_TEST_LONG_TILES_LEFT = RACK_SIZE * 2,
  // The longest table and the shortest.
  PEG_FIT_TEST_MAX_TILES_LEFT = PEG_ADJUST_MAX_VALUES_LENGTH - RACK_SIZE - 1,
  PEG_FIT_TEST_MIN_TILES_LEFT = 0,
};

// The length of a table whose plays leave up to tiles_left tiles in the bag.
static int get_number_of_values(int tiles_left) {
  return RACK_SIZE + tiles_left + 1;
}

// Positions whose values are exactly a position term plus a class term, each
// with a different set of classes, give back the class terms of a table of
// number_of_values values.
static void assert_peg_fit_solve_exact(int number_of_values) {
  const int base_class = number_of_values;
  double class_terms[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  for (int class_idx = 0; class_idx < base_class; class_idx++) {
    class_terms[class_idx] = (double)((class_idx * 7) % 11) - 5.0;
  }
  class_terms[base_class] = 0;
  PegFitPosition positions[PEG_FIT_TEST_POSITIONS];
  int expected_observations[PEG_FIT_MAX_NUMBER_OF_CLASSES] = {0};
  for (int pos_idx = 0; pos_idx < PEG_FIT_TEST_POSITIONS; pos_idx++) {
    PegFitPosition *position = &positions[pos_idx];
    *position = (PegFitPosition){.game_index = pos_idx, .turn = 0};
    const double position_term = (double)((pos_idx * 13) % 17) - 8.0;
    for (int class_idx = 1; class_idx <= base_class; class_idx++) {
      // Every position has the base class and some of the others.
      if (class_idx != base_class && (pos_idx + class_idx) % 3 == 0) {
        continue;
      }
      position->has_class[class_idx] = true;
      position->value[class_idx] = position_term + class_terms[class_idx];
      expected_observations[class_idx]++;
    }
  }
  PegFitResult result;
  peg_fit_solve(positions, PEG_FIT_TEST_POSITIONS, number_of_values, &result);
  for (int class_idx = 0; class_idx < PEG_FIT_MAX_NUMBER_OF_CLASSES;
       class_idx++) {
    assert(result.observations[class_idx] == expected_observations[class_idx]);
    if (class_idx == 0 || class_idx > base_class) {
      // No position has class 0 or a class past the base class, so their
      // terms stay 0.
      assert(result.terms[class_idx] == 0);
      continue;
    }
    assert(fabs(result.terms[class_idx] - class_terms[class_idx]) < 1e-6);
    assert(result.standard_errors[class_idx] < 1e-6);
  }
}

static void test_peg_fit_solve_exact(void) {
  assert_peg_fit_solve_exact(PEG_ADJUST_STANDARD_VALUES_LENGTH);
  assert_peg_fit_solve_exact(get_number_of_values(PEG_FIT_TEST_MIN_TILES_LEFT));
  assert_peg_fit_solve_exact(
      get_number_of_values(PEG_FIT_TEST_LONG_TILES_LEFT));
  assert_peg_fit_solve_exact(PEG_ADJUST_MAX_VALUES_LENGTH);
}

// Noise of mean 0 in one class's values leaves the other terms alone and
// shows in that class's standard error.
static void test_peg_fit_solve_noise(void) {
  PegFitPosition positions[PEG_FIT_TEST_POSITIONS];
  const int base_class = PEG_ADJUST_STANDARD_VALUES_LENGTH;
  const int noisy_class = RACK_SIZE + 2;
  for (int pos_idx = 0; pos_idx < PEG_FIT_TEST_POSITIONS; pos_idx++) {
    PegFitPosition *position = &positions[pos_idx];
    *position = (PegFitPosition){.game_index = pos_idx, .turn = 0};
    position->has_class[base_class] = true;
    position->value[base_class] = 0;
    position->has_class[noisy_class] = true;
    position->value[noisy_class] = 10.0 + (pos_idx % 2 == 0 ? 1.0 : -1.0);
  }
  PegFitResult result;
  peg_fit_solve(positions, PEG_FIT_TEST_POSITIONS,
                PEG_ADJUST_STANDARD_VALUES_LENGTH, &result);
  assert(fabs(result.terms[noisy_class] - 10.0) < 1e-6);
  // The residuals are +-0.5 about the position terms.
  const double expected_standard_error =
      sqrt(0.25 * PEG_FIT_TEST_POSITIONS / (PEG_FIT_TEST_POSITIONS - 1) /
           PEG_FIT_TEST_POSITIONS);
  assert(fabs(result.standard_errors[noisy_class] - expected_standard_error) <
         1e-9);
}

static char *get_table_filename(const char *strategy_dir, const char *name) {
  return get_formatted_string("%s/%s.csv", strategy_dir, name);
}

// A table of number_of_values values has a row for each index from 1 to its
// last.
static void assert_table_rows(const char *table, int number_of_values) {
  StringSplitter *lines = split_string_by_newline(table, true);
  int expected_index = 1;
  for (int line_idx = 0; line_idx < string_splitter_get_number_of_items(lines);
       line_idx++) {
    const char *line = string_splitter_get_item(lines, line_idx);
    if (line[0] == '#') {
      continue;
    }
    StringSplitter *fields = split_string(line, ',', true);
    assert(string_splitter_get_number_of_items(fields) == 2);
    ErrorStack *error_stack = error_stack_create();
    assert(string_to_int(string_splitter_get_item(fields, 0), error_stack) ==
           expected_index);
    const double value =
        string_to_double(string_splitter_get_item(fields, 1), error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(isfinite(value));
    error_stack_destroy(error_stack);
    string_splitter_destroy(fields);
    expected_index++;
  }
  assert(expected_index == number_of_values);
  string_splitter_destroy(lines);
}

// Runs "pegfit <fit_args> <name> <tiles_left> <options>", which writes the
// table named name under data_dir. The table has the rows of one whose plays
// leave up to tiles_left tiles in the bag and loads at that length. Returns
// its contents.
static char *fit_table(Config *config, const char *data_dir,
                       const char *fit_args, const char *name, int tiles_left,
                       const char *options) {
  char *cmd = get_formatted_string("pegfit %s %s %d %s", fit_args, name,
                                   tiles_left, options);
  load_and_exec_config_or_die(config, cmd);
  free(cmd);
  char *strategy_dir = get_formatted_string("%s/strategy", data_dir);
  char *filename = get_table_filename(strategy_dir, name);
  char *table = get_string_from_file_or_die(filename);
  free(filename);
  free(strategy_dir);
  const int number_of_values = get_number_of_values(tiles_left);
  assert_table_rows(table, number_of_values);
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(data_dir, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(peg_adjust_get_number_of_values(peg_adjust) == number_of_values);
  peg_adjust_destroy(peg_adjust);
  error_stack_destroy(error_stack);
  return table;
}

static void test_peg_fit_command(void) {
  char *tmp_dir = test_platform_make_temporary_directory("magpie_peg_fit_");
  char *strategy_dir = get_formatted_string("%s/strategy", tmp_dir);
  assert(cfile_mkdir(strategy_dir) == 0);
  // test/fixtures holds Quackle's table.
  char *set_cmd = get_formatted_string(
      "set -path %s:./test/fixtures:%s -lex CSW21 -wmp true", tmp_dir,
      DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);

  // With static endgames the table does not depend on the thread count.
  load_and_exec_config_or_die(config,
                              "pegfit 6 2 0 fit_static_1 -threads 1 -seed 7");
  load_and_exec_config_or_die(config,
                              "pegfit 6 2 0 fit_static_3 -threads 3 -seed 7");
  char *static_1_filename = get_table_filename(strategy_dir, "fit_static_1");
  char *static_3_filename = get_table_filename(strategy_dir, "fit_static_3");
  char *static_1 = get_string_from_file_or_die(static_1_filename);
  char *static_3 = get_string_from_file_or_die(static_3_filename);
  assert_strings_equal(static_1, static_3);
  assert_table_rows(static_1, PEG_ADJUST_STANDARD_VALUES_LENGTH);

  // The table reaches the plays that leave up to <tiles_left> tiles in the
  // bag. The default is the standard table's.
  char *standard =
      fit_table(config, tmp_dir, "6 2 0", "fit_standard",
                PEG_FIT_TEST_STANDARD_TILES_LEFT, "-threads 3 -seed 7");
  assert_strings_equal(static_1, standard);
  free(standard);
  // A longer table takes its positions from fuller bags too, and does not
  // depend on the thread count either.
  char *long_1 = fit_table(config, tmp_dir, "4 2 0", "fit_long_1",
                           PEG_FIT_TEST_LONG_TILES_LEFT, "-threads 1 -seed 7");
  char *long_3 = fit_table(config, tmp_dir, "4 2 0", "fit_long_3",
                           PEG_FIT_TEST_LONG_TILES_LEFT, "-threads 3 -seed 7");
  assert_strings_equal(long_1, long_3);
  free(long_3);
  free(long_1);
  // The longest table and the shortest, which has only the plays that empty
  // the bag.
  free(fit_table(config, tmp_dir, "1 1 0", "fit_longest",
                 PEG_FIT_TEST_MAX_TILES_LEFT, "-threads 2 -seed 7"));
  free(fit_table(config, tmp_dir, "2 1 0", "fit_shortest",
                 PEG_FIT_TEST_MIN_TILES_LEFT, "-threads 2 -seed 7"));
  char *too_long_cmd = get_formatted_string("pegfit 1 1 0 fit_too_long %d",
                                            PEG_FIT_TEST_MAX_TILES_LEFT + 1);
  assert_config_exec_status(config, too_long_cmd,
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  free(too_long_cmd);
  assert_config_exec_status(config, "pegfit 1 1 0 fit_not_a_number x",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);

  // The default name is the lexicon's.
  load_and_exec_config_or_die(config, "pegfit 2 1 1 -threads 2 -seed 7");
  char *solved_filename = get_table_filename(strategy_dir, "CSW21_pegadjust");
  char *solved = get_string_from_file_or_die(solved_filename);
  assert_table_rows(solved, PEG_ADJUST_STANDARD_VALUES_LENGTH);

  // Self-play with a table fits against equities without it, and names it.
  load_and_exec_config_or_die(config, "pegfit 2 1 0 fit_quackle -threads 2 "
                                      "-pegadjust quackle_pegadjust");
  char *quackle_filename = get_table_filename(strategy_dir, "fit_quackle");
  char *quackle = get_string_from_file_or_die(quackle_filename);
  assert(strstr(quackle, "playing table quackle_pegadjust") != NULL);
  assert_table_rows(quackle, PEG_ADJUST_STANDARD_VALUES_LENGTH);
  assert_config_exec_status(config, "pegfit 1 1 0 -pegadjust2 none",
                            ERROR_STATUS_PEG_FIT_DIFFERENT_TABLES);
  load_and_exec_config_or_die(config, "set -pegadjust none");

  assert_config_exec_status(config, "pegfit 0 1 0",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "pegfit 1 x 0",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "pegfit 1 1 99",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);

  free(quackle);
  free(quackle_filename);
  free(solved);
  free(solved_filename);
  free(static_3);
  free(static_1);
  free(static_3_filename);
  free(static_1_filename);
  config_destroy(config);
  free(set_cmd);
  free(strategy_dir);
  test_platform_remove_directory_tree(tmp_dir);
  free(tmp_dir);
}

void test_peg_fit(void) {
  test_peg_fit_solve_exact();
  test_peg_fit_solve_noise();
  test_peg_fit_command();
}

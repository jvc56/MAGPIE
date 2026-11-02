#include "peg_fit_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/static_eval_defs.h"
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
};

// Positions whose values are exactly a position term plus a class term, each
// with a different set of classes, give back the class terms.
static void test_peg_fit_solve_exact(void) {
  double class_terms[PEG_FIT_NUMBER_OF_CLASSES];
  for (int class_idx = 0; class_idx < PEG_FIT_BASE_CLASS; class_idx++) {
    class_terms[class_idx] = (double)((class_idx * 7) % 11) - 5.0;
  }
  class_terms[PEG_FIT_BASE_CLASS] = 0;
  PegFitPosition positions[PEG_FIT_TEST_POSITIONS];
  int expected_observations[PEG_FIT_NUMBER_OF_CLASSES] = {0};
  for (int pos_idx = 0; pos_idx < PEG_FIT_TEST_POSITIONS; pos_idx++) {
    PegFitPosition *position = &positions[pos_idx];
    *position = (PegFitPosition){.game_index = pos_idx, .turn = 0};
    const double position_term = (double)((pos_idx * 13) % 17) - 8.0;
    for (int class_idx = 1; class_idx < PEG_FIT_NUMBER_OF_CLASSES;
         class_idx++) {
      // Every position has the base class and some of the others.
      if (class_idx != PEG_FIT_BASE_CLASS && (pos_idx + class_idx) % 3 == 0) {
        continue;
      }
      position->has_class[class_idx] = true;
      position->value[class_idx] = position_term + class_terms[class_idx];
      expected_observations[class_idx]++;
    }
  }
  PegFitResult result;
  peg_fit_solve(positions, PEG_FIT_TEST_POSITIONS, &result);
  for (int class_idx = 0; class_idx < PEG_FIT_NUMBER_OF_CLASSES; class_idx++) {
    assert(result.observations[class_idx] == expected_observations[class_idx]);
    if (class_idx == 0) {
      // No position has class 0, so its term stays 0.
      assert(result.terms[class_idx] == 0);
      continue;
    }
    assert(fabs(result.terms[class_idx] - class_terms[class_idx]) < 1e-6);
    assert(result.standard_errors[class_idx] < 1e-6);
  }
}

// Noise of mean 0 in one class's values leaves the other terms alone and
// shows in that class's standard error.
static void test_peg_fit_solve_noise(void) {
  PegFitPosition positions[PEG_FIT_TEST_POSITIONS];
  const int noisy_class = 9;
  for (int pos_idx = 0; pos_idx < PEG_FIT_TEST_POSITIONS; pos_idx++) {
    PegFitPosition *position = &positions[pos_idx];
    *position = (PegFitPosition){.game_index = pos_idx, .turn = 0};
    position->has_class[PEG_FIT_BASE_CLASS] = true;
    position->value[PEG_FIT_BASE_CLASS] = 0;
    position->has_class[noisy_class] = true;
    position->value[noisy_class] = 10.0 + (pos_idx % 2 == 0 ? 1.0 : -1.0);
  }
  PegFitResult result;
  peg_fit_solve(positions, PEG_FIT_TEST_POSITIONS, &result);
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

// A table has a row for each index from 1 to PEG_ADJUST_VALUES_LENGTH - 1.
static void assert_table_rows(const char *table) {
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
  assert(expected_index == PEG_ADJUST_VALUES_LENGTH);
  string_splitter_destroy(lines);
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
  assert_table_rows(static_1);

  // The default name is the lexicon's.
  load_and_exec_config_or_die(config, "pegfit 2 1 1 -threads 2 -seed 7");
  char *solved_filename = get_table_filename(strategy_dir, "CSW21_pegadjust");
  char *solved = get_string_from_file_or_die(solved_filename);
  assert_table_rows(solved);

  // Self-play with a table fits against equities without it, and names it.
  load_and_exec_config_or_die(config, "pegfit 2 1 0 fit_quackle -threads 2 "
                                      "-pegadjust quackle_pegadjust");
  char *quackle_filename = get_table_filename(strategy_dir, "fit_quackle");
  char *quackle = get_string_from_file_or_die(quackle_filename);
  assert(strstr(quackle, "playing table quackle_pegadjust") != NULL);
  assert_table_rows(quackle);
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

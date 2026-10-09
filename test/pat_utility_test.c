#include "pat_utility_test.h"

#include "../src/def/config_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_utility.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_test_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static Equity pat_utility_test_correction(const PATWeights *pat, int bag,
                                          int margin) {
  return pat->utility_table[pat_utility_index(pat, bag, margin)];
}

// pat_prepare_utility reads winpct_<ld> for weights that carry a
// correction, only once per table, and leaves weights without one alone.
static void test_pat_utility_prepare(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  ErrorStack *error_stack = error_stack_create();

  PATWeights *without = pat_create_zeroed("without_utility");
  pat_prepare_utility(without, DEFAULT_TEST_DATA_PATH, ld, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(without->utility_table == NULL);
  pat_destroy(without);

  PATWeights *pat = pat_create_zeroed("with_utility");
  pat->utility_adjust = 2000.0;
  pat_prepare_utility(pat, DEFAULT_TEST_DATA_PATH, ld, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pat->utility_table != NULL);
  assert(pat->utility_suffix_max != NULL);
  char *expected_name =
      get_formatted_string("%s%s", DEFAULT_WIN_PCT_PREFIX, ld_get_name(ld));
  assert(strings_equal(pat->utility_win_pct_name, expected_name));
  free(expected_name);
  const Equity *table = pat->utility_table;
  pat_prepare_utility(pat, DEFAULT_TEST_DATA_PATH, ld, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pat->utility_table == table);
  // Setting the correction to zero drops the tables.
  pat_set_utility_adjust(pat, 0.0, NULL);
  assert(pat->utility_table == NULL);
  assert(pat->utility_suffix_max == NULL);
  pat_destroy(pat);

  // A data path without the table is an error, not a crash, and leaves
  // the weights without tables.
  char *empty_dir = create_temp_pat_data_dir();
  PATWeights *missing = pat_create_zeroed("missing_utility");
  missing->utility_adjust = 2000.0;
  pat_prepare_utility(missing, empty_dir, ld, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_PAT_UTILITY_WIN_PCT);
  error_stack_reset(error_stack);
  assert(missing->utility_table == NULL);
  pat_destroy(missing);
  free(empty_dir);

  error_stack_destroy(error_stack);
  config_destroy(config);
}

// The correction is half the file's weight times kappa = -U''/U' of the
// win/spread utility: positive where the mover leads, negative where it
// trails, rising through a close game, and at the margin limit only the
// spread term is left, which is odd in the margin. The suffix maxima bound
// every correction at or above a margin.
static void test_pat_utility_table_shape(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -winpct winpct_english");
  PATWeights *pat = pat_create_zeroed("utility_shape");
  pat_set_utility_adjust(pat, 2000.0, config_get_win_pcts(config));
  const int max_bag = pat->utility_max_bag;
  assert(max_bag > 50);
  const int bags[] = {20, 50, max_bag};
  for (size_t bag_idx = 0; bag_idx < sizeof(bags) / sizeof(bags[0]);
       bag_idx++) {
    const int bag = bags[bag_idx];
    assert(pat_utility_test_correction(pat, bag, 100) > 0);
    assert(pat_utility_test_correction(pat, bag, -100) < 0);
    assert(pat_utility_test_correction(pat, bag, PAT_UTILITY_MARGIN_LIMIT) ==
           -pat_utility_test_correction(pat, bag, -PAT_UTILITY_MARGIN_LIMIT));
    for (int margin = -50; margin < 100; margin += 25) {
      assert(pat_utility_test_correction(pat, bag, margin) <
             pat_utility_test_correction(pat, bag, margin + 25));
    }
  }
  for (int bag = 0; bag <= max_bag; bag++) {
    for (int margin = -PAT_UTILITY_MARGIN_LIMIT;
         margin <= PAT_UTILITY_MARGIN_LIMIT; margin++) {
      const size_t index = pat_utility_index(pat, bag, margin);
      assert(pat->utility_suffix_max[index] >= pat->utility_table[index]);
      if (margin < PAT_UTILITY_MARGIN_LIMIT) {
        assert(pat->utility_suffix_max[index] >=
               pat->utility_suffix_max[index + 1]);
      } else {
        assert(pat->utility_suffix_max[index] == pat->utility_table[index]);
      }
    }
  }
  // Out-of-range bags and margins read the nearest edge.
  assert(pat_utility_index(pat, -5, 0) == pat_utility_index(pat, 0, 0));
  assert(pat_utility_index(pat, max_bag + 10, 0) ==
         pat_utility_index(pat, max_bag, 0));
  assert(pat_utility_index(pat, 3, 10 * PAT_UTILITY_MARGIN_LIMIT) ==
         pat_utility_index(pat, 3, PAT_UTILITY_MARGIN_LIMIT));
  assert(pat_utility_index(pat, 3, -10 * PAT_UTILITY_MARGIN_LIMIT) ==
         pat_utility_index(pat, 3, -PAT_UTILITY_MARGIN_LIMIT));
  // The correction scales with the file's weight.
  const Equity at_2000 = pat_utility_test_correction(pat, 50, 100);
  pat_set_utility_adjust(pat, 1000.0, config_get_win_pcts(config));
  const Equity at_1000 = pat_utility_test_correction(pat, 50, 100);
  assert(at_1000 > 0 && at_1000 < at_2000);
  assert(abs(2 * at_1000 - at_2000) <= 1);
  pat_destroy(pat);
  config_destroy(config);
}

void test_pat_utility(void) {
  test_pat_utility_prepare();
  test_pat_utility_table_shape();
}

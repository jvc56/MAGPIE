#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/klv.h"
#include "../src/ent/klv_csv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

void test_small_klv(void) {
  Config *config = config_create_or_die("set -lex CSW21 -ld english_small");
  const LetterDistribution *ld = config_get_ld(config);
  assert(ld_get_size(ld) == 3);
  const char *data_path = DEFAULT_TEST_DATA_PATH;
  const char *klv_name = "small";
  ErrorStack *error_stack = error_stack_create();
  char *leaves_filename = data_filepaths_get_writable_filename(
      data_path, klv_name, DATA_FILEPATH_TYPE_LEAVES, error_stack);
  assert(error_stack_is_empty(error_stack));

  KLV *small_klv = klv_create_empty(ld, klv_name);
  assert(klv_get_number_of_leaves(small_klv) == 11);

  set_klv_leave_value(small_klv, ld, "?", double_to_equity(1.0));
  set_klv_leave_value(small_klv, ld, "?A", double_to_equity(2.0));
  set_klv_leave_value(small_klv, ld, "?AAB", double_to_equity(3.0));
  set_klv_leave_value(small_klv, ld, "AAB", double_to_equity(4.0));

  klv_write_to_csv_or_die(small_klv, ld, data_path, klv_name);

  char *leaves_file_string = get_string_from_file_or_die(leaves_filename);

  assert_strings_equal(leaves_file_string,
                       "?,1.000000\nA,0.000000\nB,0.000000\n?A,2.000000\n?B,0."
                       "000000\nAA,0.000000\nAB,0.000000\n?AA,0.000000\n?AB,0."
                       "000000\nAAB,4.000000\n?AAB,3.000000\n");
  free(leaves_file_string);

  KLV *small_klv_copy = klv_read_from_csv_or_die(ld, data_path, klv_name);

  assert_klvs_equal(small_klv, small_klv_copy);

  klv_write_or_die(small_klv_copy, data_path, klv_name);

  KLV *small_klv_copy2 = klv_create_or_die(data_path, klv_name);

  assert_klvs_equal(small_klv, small_klv_copy2);
  assert_klvs_equal(small_klv_copy, small_klv_copy2);

  free(leaves_filename);
  klv_destroy(small_klv_copy2);
  klv_destroy(small_klv_copy);
  klv_destroy(small_klv);
  config_destroy(config);
  error_stack_destroy(error_stack);
}

void test_contextual_klv_adjustments(void) {
  Config *config = config_create_or_die("set -lex CSW21 -ld english_small");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = klv_create_empty(ld, "contextual-small");
  klv->context_alphabet_size = ld_get_size(ld);
  klv->context_num_pool_bins = 2;
  klv->context_pool_bin_upper_bounds = malloc_or_die(2 * sizeof(uint16_t));
  klv->context_pool_bin_upper_bounds[0] = 4;
  klv->context_pool_bin_upper_bounds[1] = 100;
  const size_t num_biases = 2 * KLV3_DRAW_COUNT_HEADS * (size_t)ld_get_size(ld);
  const size_t num_weights =
      KLV3_DRAW_COUNT_HEADS * (size_t)ld_get_size(ld) * (size_t)ld_get_size(ld);
  klv->context_biases = calloc_or_die(num_biases, sizeof(Equity));
  klv->context_weights = calloc_or_die(num_weights, sizeof(Equity));

  const MachineLetter a = ld_hl_to_ml(ld, "A");
  const MachineLetter b = ld_hl_to_ml(ld, "B");
  klv->context_biases[klv3_bias_index(klv, 0, 1, a)] =
      double_to_equity(0.75);
  klv->context_biases[klv3_bias_index(klv, 1, 1, a)] =
      double_to_equity(0.75);
  klv->context_biases[klv3_bias_index(klv, 0, 4, a)] =
      double_to_equity(-0.25);
  klv->context_biases[klv3_bias_index(klv, 1, 4, a)] =
      double_to_equity(-0.25);
  klv->context_biases[klv3_bias_index(klv, 0, 2, a)] = double_to_equity(1.5);
  klv->context_biases[klv3_bias_index(klv, 1, 2, a)] = double_to_equity(2.5);
  klv->context_weights[klv3_weight_index(klv, 2, a, a)] = double_to_equity(2.0);
  klv->context_weights[klv3_weight_index(klv, 2, a, b)] = double_to_equity(6.0);

  Rack leave;
  rack_set_to_string(ld, &leave, "AA");
  const uint32_t leave_index = klv_get_word_index(klv, &leave);
  assert(leave_index != KLV_UNFOUND_INDEX);

  Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE];
  int unseen_counts[MACHINE_LETTER_MAX_VALUE] = {0};
  unseen_counts[a] = 1;
  unseen_counts[b] = 3;
  klv3_compute_tile_adjustments(klv, unseen_counts, 4, adjustments);
  assert(equity_to_double(adjustments[2][a]) == 6.5);
  assert(equity_to_double(klv_get_contextual_indexed_leave_value(
             klv, leave_index, &leave, 2, adjustments)) == 13.0);
  // The no-draw head stays exactly rack-only.
  assert(klv_get_contextual_indexed_leave_value(klv, leave_index, &leave, 0,
                                                adjustments) ==
         klv_get_indexed_leave_value(klv, leave_index));

  unseen_counts[a] = 2;
  unseen_counts[b] = 3;
  klv3_compute_tile_adjustments(klv, unseen_counts, 5, adjustments);
  assert(equity_to_double(adjustments[2][a]) == 6.9);

  Equity rack_adjustments[KLV3_DRAW_COUNT_HEADS]
                         [MACHINE_LETTER_MAX_VALUE];
  klv3_compute_rack_tile_adjustments(klv, unseen_counts, 5, &leave,
                                     rack_adjustments);
  for (int draw_count = 0; draw_count < KLV3_DRAW_COUNT_HEADS; draw_count++) {
    // Present tile types are byte-for-byte identical to the full calculation.
    assert(rack_adjustments[draw_count][a] ==
           adjustments[draw_count][a]);
    // Absent tile types remain zero and cannot be read by a leave subrack.
    assert(rack_adjustments[draw_count][b] == 0);
  }
  // With rack AA and two tiles available to draw, the possible adjustments
  // are 0 (empty/full rack) and one A adjustment for the one-tile leave.
  assert(klv3_get_rack_adjustment_range(&leave, 2, rack_adjustments) ==
         double_to_equity(0.75));
  assert(klv3_get_rack_adjustment_range(&leave, 0, rack_adjustments) == 0);
  assert(klv3_sample_rack_adjustment_magnitude(
             klv, unseen_counts, 5, &leave, 5) == double_to_equity(0.5));

  klv_destroy(klv);
  config_destroy(config);
}

void test_normal_klv(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  assert(ld_get_size(ld) == 27);
  const char *data_path = "testdata";
  const char *klv_name = "normal";
  ErrorStack *error_stack = error_stack_create();
  assert(error_stack_is_empty(error_stack));

  KLV *normal_klv = klv_create_empty(ld, "normal");
  assert(klv_get_number_of_leaves(normal_klv) == 914624);

  set_klv_leave_value(normal_klv, ld, "?", double_to_equity(1.0));
  set_klv_leave_value(normal_klv, ld, "?Z", double_to_equity(2.0));
  set_klv_leave_value(normal_klv, ld, "YYZ", double_to_equity(3.0));
  set_klv_leave_value(normal_klv, ld, "WWXYYZ", double_to_equity(4.0));

  klv_write_to_csv_or_die(normal_klv, ld, data_path, klv_name);

  KLV *normal_klv_copy = klv_read_from_csv_or_die(ld, data_path, klv_name);

  assert_klvs_equal(normal_klv, normal_klv_copy);

  klv_write_or_die(normal_klv_copy, data_path, klv_name);

  KLV *normal_klv_copy2 = klv_create_or_die(data_path, klv_name);

  assert_klvs_equal(normal_klv, normal_klv_copy2);
  assert_klvs_equal(normal_klv_copy, normal_klv_copy2);

  klv_destroy(normal_klv_copy2);
  klv_destroy(normal_klv_copy);
  klv_destroy(normal_klv);
  config_destroy(config);
  error_stack_destroy(error_stack);
}

void test_klv(void) {
  test_small_klv();
  test_contextual_klv_adjustments();
  test_normal_klv();
}

#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/klv.h"
#include "../src/ent/klv_csv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
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
  test_normal_klv();
}
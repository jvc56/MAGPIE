#include <assert.h>

#include "../../src/ent/conversion_results.h"

#include "../../src/impl/config.h"
#include "../../src/impl/convert.h"

#include "test_util.h"

void test_convert_error(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  ConversionResults *conversion_results = conversion_results_create();
  ConversionArgs args;

  args.conversion_type_string = "bad conversion type";
  args.data_paths = NULL;
  args.input_name = NULL;
  args.output_name = NULL;
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_UNRECOGNIZED_CONVERSION_TYPE);

  args.conversion_type_string = "text2dawg";
  assert(convert(&args, conversion_results) == CONVERT_STATUS_INPUT_FILE_ERROR);

  args.input_name = "some not null name";
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_too_long";
  args.output_name = "CSW21_too_long";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_too_long_2";
  args.output_name = "CSW21_too_long_2";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_LONG);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_invalid_letter";
  args.output_name = "CSW21_invalid_letter";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_invalid_blank_letter";
  args.output_name = "CSW21_invalid_blank_letter";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_TEXT_CONTAINS_INVALID_LETTER);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_too_short";
  args.output_name = "CSW21_too_short";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_TEXT_CONTAINS_WORD_TOO_SHORT);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_name = "CSW21_small";
  args.output_name = "you/really/gotta/win/those/CSW21_small.kwg";
  args.ld = config_get_ld(config);
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_OUTPUT_FILE_NOT_WRITABLE);

  conversion_results_destroy(conversion_results);
  config_destroy(config);
}

void test_convert_success(void) {}

void test_convert(void) {
  test_convert_error();
  test_convert_success();
}
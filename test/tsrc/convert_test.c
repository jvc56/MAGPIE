#include <assert.h>

#include "../../src/ent/conversion_results.h"
#include "../../src/ent/validated_move.h"

#include "../../src/impl/config.h"
#include "../../src/impl/convert.h"

#include "test_constants.h"
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
  args.ld = NULL;
  assert(convert(&args, conversion_results) ==
         CONVERT_STATUS_MISSING_LETTER_DISTRIBUTION);

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

  conversion_results_destroy(conversion_results);
  config_destroy(config);
}

void test_convert_success(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  ValidatedMoves *vms;
  Game *game;

  load_and_exec_config_or_die(
      config, "convert text2dawg CSW21_dawg_only CSW21_dawg_only");
  load_and_exec_config_or_die(config, "set -lex CSW21_dawg_only");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  game = config_get_game(config);
  vms = validated_moves_create(game, 0, "H8.BRAVO", false, false, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(
      config, "convert text2dawg CSW21_gaddag_only CSW21_gaddag_only");
  load_and_exec_config_or_die(config, "set -lex CSW21_gaddag_only");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  game = config_get_game(config);
  vms = validated_moves_create(game, 0, "H8.CHARLIE", false, false, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(
      config, "convert text2dawg CSW21_dawg_and_gaddag CSW21_dawg_and_gaddag");
  load_and_exec_config_or_die(config, "set -lex CSW21_dawg_and_gaddag");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  game = config_get_game(config);
  vms = validated_moves_create(game, 0, "H8.QUEBEC", false, false, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config,
                              "convert text2wordmap CSW21_small CSW21_small");

  load_and_exec_config_or_die(config, "set -ld english_small");
  load_and_exec_config_or_die(config,
                              "convert csv2klv CSW21_small CSW21_small");
  load_and_exec_config_or_die(
      config, "set -k1 CSW21_small -k2 CSW21_small -ld english_small");
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, "AAB");
  assert(within_epsilon_for_equity(klv_get_leave_value(klv, leave), 4.0));
  rack_destroy(leave);

  load_and_exec_config_or_die(config,
                              "convert klv2csv CSW21_small CSW21_small");
  char *leaves_file_string =
      get_string_from_file("testdata/leaves/CSW21_small.csv");
  assert_strings_equal(leaves_file_string,
                       "?,1.000000\nA,0.000000\nB,0.000000\n?A,2.000000\n?B,0."
                       "000000\nAA,0.000000\nAB,0.000000\n?AA,0.000000\n?AB,0."
                       "000000\nAAB,4.000000\n?AAB,3.000000\n");
  free(leaves_file_string);

  config_destroy(config);
}

void test_convert(void) {
  test_convert_error();
  test_convert_success();
}
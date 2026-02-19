#include "../src/ent/conversion_results.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/convert.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

void convert_and_assert_status(const ConversionArgs *args,
                               ConversionResults *results,
                               error_code_t expected_status) {
  ErrorStack *error_stack = error_stack_create();
  convert(args, results, error_stack);
  if (error_stack_top(error_stack) != expected_status) {
    if (error_stack_top(error_stack) == ERROR_STATUS_SUCCESS) {
      log_fatal("expected error status %d but got success", expected_status);
    } else {
      error_stack_print_and_reset(error_stack);
    }
    assert(0);
  }
  error_stack_destroy(error_stack);
}

void test_convert_error(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  ConversionResults *conversion_results = conversion_results_create();
  ConversionArgs args;
  args.conversion_type_string = NULL;
  args.data_paths = NULL;
  args.input_and_output_name = NULL;
  args.ld_name = NULL;

  args.conversion_type_string = "bad conversion type";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_UNRECOGNIZED_CONVERSION_TYPE);

  args.conversion_type_string = "text2dawg";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_INPUT_FILE_ERROR);

  args.input_and_output_name = "some not null name";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_LD_LEXICON_DEFAULT_NOT_FOUND);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_and_output_name = "CSW21_too_long";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_LONG);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_and_output_name = "CSW21_too_long_2";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_LONG);
  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_and_output_name = "CSW21_invalid_letter";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_and_output_name = "CSW21_invalid_blank_letter";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_TEXT_CONTAINS_INVALID_LETTER);

  args.conversion_type_string = "text2dawg";
  args.data_paths = DEFAULT_TEST_DATA_PATH;
  args.input_and_output_name = "CSW21_too_short";
  convert_and_assert_status(&args, conversion_results,
                            ERROR_STATUS_CONVERT_TEXT_CONTAINS_WORD_TOO_SHORT);

  conversion_results_destroy(conversion_results);
  config_destroy(config);
}

void test_convert_success(void) {
  ErrorStack *error_stack = error_stack_create();
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp false -s1 equity -s2 equity "
                           "-r1 all -r2 all -numplays 1");
  ValidatedMoves *vms;
  const Game *game;

  load_and_exec_config_or_die(config, "convert text2dawg CSW21_dawg_only");
  load_and_exec_config_or_die(config, "set -lex CSW21_dawg_only");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 BRAVO/ 0/0 0 "
              "-lex CSW21;");
  game = config_get_game(config);
  vms = validated_moves_create_and_assert_status(game, 0, "H8 BRAVO", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, "convert text2dawg CSW21_gaddag_only");
  load_and_exec_config_or_die(config, "set -lex CSW21_gaddag_only");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 CHARLIE/ 0/0 0 "
              "-lex CSW21;");
  game = config_get_game(config);
  vms = validated_moves_create_and_assert_status(game, 0, "H8 CHARLIE", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config,
                              "convert text2dawg CSW21_dawg_and_gaddag");
  load_and_exec_config_or_die(config, "set -lex CSW21_dawg_and_gaddag");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 QUEBEC/ 0/0 0 "
              "-lex CSW21;");
  game = config_get_game(config);
  vms = validated_moves_create_and_assert_status(game, 0, "H8 QUEBEC", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, "convert text2wordmap CSW21_small");

  load_and_exec_config_or_die(config, "set -ld english_small");
  load_and_exec_config_or_die(config, "convert csv2klv CSW21_small");
  load_and_exec_config_or_die(
      config, "set -k1 CSW21_small -k2 CSW21_small -ld english_small");
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, "AAB");
  assert_equal_at_equity_resolution(
      equity_to_double(klv_get_leave_value(klv, leave)), 4.0);
  rack_destroy(leave);

  load_and_exec_config_or_die(config,
                              "convert klv2csv CSW21_small english_small");

  char *leaves_filename = data_filepaths_get_readable_filename(
      DEFAULT_TEST_DATA_PATH, "CSW21_small", DATA_FILEPATH_TYPE_LEAVES,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to read input file '%s'\n", leaves_filename);
  }

  char *leaves_file_string = get_string_from_file_or_die(leaves_filename);

  assert_strings_equal(leaves_file_string,
                       "?,1.000000\nA,0.000000\nB,0.000000\n?A,2.000000\n?B,0."
                       "000000\nAA,0.000000\nAB,0.000000\n?AA,0.000000\n?AB,0."
                       "000000\nAAB,4.000000\n?AAB,3.000000\n");
  free(leaves_file_string);
  free(leaves_filename);

  config_destroy(config);
  error_stack_destroy(error_stack);
}

void test_convert(void) {
  test_convert_error();
  test_convert_success();
}
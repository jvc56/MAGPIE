#include "../../src/impl/config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/def/autoplay_defs.h"
#include "../../src/def/config_defs.h"
#include "../../src/def/game_defs.h"
#include "../../src/def/move_defs.h"
#include "../../src/def/players_data_defs.h"
#include "../../src/def/validated_move_defs.h"
#include "../../src/ent/game.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/players_data.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/thread_control.h"
#include "../../src/impl/cgp.h"
#include "../../src/str/rack_string.h"
#include "../../src/util/io_util.h"
#include "../../src/util/string_util.h"

#include "config_test.h"
#include "test_constants.h"
#include "test_util.h"

void test_config_load_error(Config *config, const char *cmd,
                            error_code_t expected_status,
                            ErrorStack *error_stack) {
  config_load_command(config, cmd, error_stack);
  error_code_t actual_status = error_stack_top(error_stack);
  if (actual_status != expected_status) {
    printf("config status mismatched:\nexpected: %d\nactual: %d\n>%s<\n",
           expected_status, actual_status, cmd);
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  error_stack_reset(error_stack);
}

void test_config_load_error_cases(void) {
  Config *config = config_create_default_test();
  ErrorStack *error_stack = error_stack_create();
  test_config_load_error(config, "endgame",
                         ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
                         error_stack);
  test_config_load_error(config, "sim -lex CSW21 -iter 1000 -plies 10 1",
                         ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
                         error_stack);
  test_config_load_error(config, "sim -plies 3 -plies 4",
                         ERROR_STATUS_CONFIG_LOAD_DUPLICATE_ARG, error_stack);
  test_config_load_error(config, "sim -it 1000 -infer",
                         ERROR_STATUS_CONFIG_LOAD_MISPLACED_COMMAND,
                         error_stack);
  test_config_load_error(config, "sim -i 1000",
                         ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                         error_stack);
  test_config_load_error(config, "sim -it 1000 -l2 CSW21",
                         ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING, error_stack);
  test_config_load_error(config, "set -mode uci",
                         ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_EXEC_MODE,
                         error_stack);
  test_config_load_error(config, "set -gp on",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
                         error_stack);
  test_config_load_error(config, "set -gp off",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
                         error_stack);
  test_config_load_error(config, "set -hr on",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
                         error_stack);
  test_config_load_error(config, "set -hr off",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
                         error_stack);
  test_config_load_error(config, "set -seed -2",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -lex CSW21 -it 1000 -plies",
                         ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
                         error_stack);
  test_config_load_error(config, "cgp 1 2 3",
                         ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
                         error_stack);
  test_config_load_error(config, "create klv",
                         ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
                         error_stack);
  test_config_load_error(config, "sim -bdn invalid_number_of_rows15",
                         ERROR_STATUS_CONFIG_LOAD_BOARD_LAYOUT_ERROR,
                         error_stack);
  test_config_load_error(config, "sim -var Lonify",
                         ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_GAME_VARIANT,
                         error_stack);
  test_config_load_error(config, "sim -bb 3b4",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -s1 random",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_SORT_TYPE,
                         error_stack);
  test_config_load_error(config, "sim -s2 none",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_SORT_TYPE,
                         error_stack);
  test_config_load_error(config, "sim -r1 top",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_RECORD_TYPE,
                         error_stack);
  test_config_load_error(config, "sim -r2 3",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_RECORD_TYPE,
                         error_stack);
  test_config_load_error(config, "sim -numplays three",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -numplays 123R456",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -numplays -2",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -plies two",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -plies -3",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -iter six",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -it -6",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -it 0",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -scond -95",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -scond 102",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -scond F",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "sim -eq 23434.32433.4324",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "sim -eq -3",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -eq -4.5",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -eq none",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "sim -seed zero",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -threads many",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -threads 0",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -threads -100",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -pfreq x",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -pfreq -40",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -l1 CSW21",
                         ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING, error_stack);
  test_config_load_error(config, "sim -l1 CSW21 -l2 DISC2",
                         ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
                         error_stack);
  test_config_load_error(config, "sim -l1 OSPS49 -l2 DISC2",
                         ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
                         error_stack);
  test_config_load_error(config, "sim -l1 NWL20 -l2 OSPS49",
                         ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
                         error_stack);
  test_config_load_error(config, "sim -l1 NWL20 -l2 NWL20 -k2 DISC2",
                         ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
                         error_stack);
  test_config_load_error(
      config, "sim -l1 NWL20 -l2 CSW21 -ld german",
      ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LETTER_DISTRIBUTION, error_stack);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_config_load_success(void) {
  Config *config = config_create_default_test();

  // Loading with whitespace should not fail
  load_and_exec_config_or_die(config, "           ");

  // Loading with no lexicon data should not fail
  load_and_exec_config_or_die(config, "set -plies 3");

  const char *ld_name = "english";
  int bingo_bonus = 73;
  const char *game_variant = "wordsmog";
  const char *p1 = "Alice";
  const char *p2 = "Bob";
  const char *l1 = "CSW21";
  const char *l2 = "NWL20";
  const char *s1 = "score";
  const char *r1 = "all";
  const char *s2 = "equity";
  const char *r2 = "best";
  double equity_margin = 4.6;
  int num_plays = 10;
  int plies = 4;
  int max_iterations = 400;
  int stopping_cond = 98;
  int seed = 101;
  int number_of_threads = 6;
  int print_info = 200;

  StringBuilder *test_string_builder = string_builder_create();
  string_builder_add_formatted_string(
      test_string_builder,
      "set -ld %s -bb %d -var %s -l1 %s -l2 %s -s1 %s -r1 "
      "%s -s2 %s -r2 %s -eq %0.2f -numplays %d "
      "-plies %d -it "
      "%d -scond %d -seed %d -threads %d -pfreq %d -gp true -hr true "
      "-p1 %s "
      "-p2 "
      "%s",
      ld_name, bingo_bonus, game_variant, l1, l2, s1, r1, s2, r2, equity_margin,
      num_plays, plies, max_iterations, stopping_cond, seed, number_of_threads,
      print_info, p1, p2);

  load_and_exec_config_or_die(config, string_builder_peek(test_string_builder));

  assert(config_get_game_variant(config) == GAME_VARIANT_WORDSMOG);
  assert(players_data_get_move_sort_type(config_get_players_data(config), 0) ==
         MOVE_SORT_SCORE);
  assert(players_data_get_move_record_type(config_get_players_data(config),
                                           0) == MOVE_RECORD_ALL);
  assert(players_data_get_move_sort_type(config_get_players_data(config), 1) ==
         MOVE_SORT_EQUITY);
  assert(players_data_get_move_record_type(config_get_players_data(config),
                                           1) == MOVE_RECORD_BEST);
  assert(config_get_bingo_bonus(config) == bingo_bonus);
  assert(within_epsilon(config_get_equity_margin(config), equity_margin));
  assert(config_get_num_plays(config) == num_plays);
  assert(config_get_plies(config) == plies);
  assert(config_get_max_iterations(config) == max_iterations);
  assert(within_epsilon(config_get_stop_cond_pct(config), 98));
  assert(thread_control_get_threads(config_get_thread_control(config)) ==
         number_of_threads);
  assert(thread_control_get_print_info_interval(
             config_get_thread_control(config)) == print_info);
  assert(config_get_use_game_pairs(config));
  assert(config_get_human_readable(config));

  // Change some fields, confirm that
  // other fields retain their value.
  ld_name = "english";
  bingo_bonus = 22;
  l1 = "NWL20";
  l2 = "CSW21";
  s1 = "equity";
  r1 = "best";
  s2 = "score";
  r2 = "all";
  plies = 123;
  max_iterations = 6;
  number_of_threads = 9;
  print_info = 850;

  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(
      test_string_builder,
      "set -ld %s -bb %d -l1 %s -l2 %s  -s1 "
      "%s -r1 %s -s2 %s -r2 %s -plies %d -it %d "
      "-threads %d "
      "-pfreq %d -gp false -hr false",
      ld_name, bingo_bonus, l1, l2, s1, r1, s2, r2, plies, max_iterations,
      number_of_threads, print_info);

  load_and_exec_config_or_die(config, string_builder_peek(test_string_builder));

  assert(config_get_game_variant(config) == GAME_VARIANT_WORDSMOG);
  assert(players_data_get_move_sort_type(config_get_players_data(config), 0) ==
         MOVE_SORT_EQUITY);
  assert(players_data_get_move_record_type(config_get_players_data(config),
                                           0) == MOVE_RECORD_BEST);
  assert(players_data_get_move_sort_type(config_get_players_data(config), 1) ==
         MOVE_SORT_SCORE);
  assert(players_data_get_move_record_type(config_get_players_data(config),
                                           1) == MOVE_RECORD_ALL);
  assert(config_get_bingo_bonus(config) == bingo_bonus);
  assert(within_epsilon(config_get_equity_margin(config), equity_margin));
  assert(config_get_num_plays(config) == num_plays);
  assert(config_get_plies(config) == plies);
  assert(config_get_max_iterations(config) == max_iterations);
  assert(within_epsilon(config_get_stop_cond_pct(config), 98));
  assert(thread_control_get_threads(config_get_thread_control(config)) ==
         number_of_threads);
  assert(thread_control_get_print_info_interval(
             config_get_thread_control(config)) == print_info);
  assert(!config_get_use_game_pairs(config));
  assert(!config_get_human_readable(config));

  string_builder_destroy(test_string_builder);
  config_destroy(config);
}

void assert_lexical_data(Config *config, const char *cmd, const char *l1,
                         const char *l2, const char *k1, const char *k2,
                         const char *ld) {
  load_and_exec_config_or_die(config, cmd);
  const PlayersData *pd = config_get_players_data(config);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KWG, 0),
                       l1);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KWG, 1),
                       l2);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KLV, 0),
                       k1);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KLV, 1),
                       k2);
  assert_strings_equal(ld_get_name(config_get_ld(config)), ld);
}

void test_config_lexical_data(void) {
  Config *config = config_create_default_test();
  // Check that defaults are set correctly
  assert_lexical_data(config, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "english");
  // Check that lexicons, leaves, and ld change change
  // successfully if they belong to the same ld type.
  assert_lexical_data(config, "set -l2 NWL20 -ld english_blank_is_5 -k1 NWL20",
                      "CSW21", "NWL20", "NWL20", "CSW21", "english_blank_is_5");
  // The leaves and ld should stay the same since they are
  // the same ld type.
  assert_lexical_data(config, "set -lex CSW21", "CSW21", "CSW21", "NWL20",
                      "CSW21", "english_blank_is_5");
  // Check that the leaves arg behaves as expected
  assert_lexical_data(config, "set -leaves CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "english_blank_is_5");
  // Check that the leaves arg behaves as expected
  assert_lexical_data(config, "set -leaves NWL20", "CSW21", "CSW21", "NWL20",
                      "NWL20", "english_blank_is_5");
  // Check that defaults are set correctly when switching to a new language
  assert_lexical_data(config, "set -lex FRA20", "FRA20", "FRA20", "FRA20",
                      "FRA20", "french");
  config_destroy(config);
}

void assert_config_exec_status(Config *config, const char *cmd,
                               error_code_t expected_error_code) {
  ErrorStack *error_stack = error_stack_create();
  config_load_command(config, cmd, error_stack);
  error_code_t status = error_stack_top(error_stack);
  if (status != ERROR_STATUS_SUCCESS) {
    log_fatal("load config failed with status %d: %s\n", status, cmd);
  }

  config_execute_command(config, error_stack);

  error_code_t actual_error_code = error_stack_top(error_stack);

  if (actual_error_code != expected_error_code) {
    printf("config exec error types do not match:\nexpected: %d\nactual: "
           "%d\n>%s<\n",
           expected_error_code, actual_error_code, cmd);
    abort();
  }
  error_stack_destroy(error_stack);
}

void test_config_exec_parse_args(void) {
  Config *config = config_create_default_test();

  // Ensure all commands that require game data fail correctly
  assert_config_exec_status(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "addmoves 1",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "gen",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "sim",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "infer 0 3",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "autoplay game 10",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);

  // CGP
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 1/2 0/0 0 -lex CSW21",
      ERROR_STATUS_CGP_PARSE_MALFORMED_RACK_LETTERS);
  assert_config_exec_status(config, "cgp " VS_OXY, ERROR_STATUS_SUCCESS);

  // Adding moves
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(
      config, "add 8A.HADJI -lex CSW21",
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED);
  assert_config_exec_status(config, "add 8D.HADJI -lex CSW21",
                            ERROR_STATUS_SUCCESS);

  // Setting the rack
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack 0 ABC",
                            ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "rack 3 ABC",
                            ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "rack 1 AB3C",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack 1 ABCZZZ",

                            ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG);
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack 1 FF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack 1 ZYYABCF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack 2 CC",
                            ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG);

  // Generating moves
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);

  // Simulation
  assert_config_exec_status(config, "gen -numplays 2", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim AEIN3R -it 1",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim -it 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim AEINR -it 1", ERROR_STATUS_SUCCESS);

  // Inference
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer 0 ABC 14",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 3 ABC 14",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 AB3C 14",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "infer 1 ABC 1R4",

                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 -4",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "infer 1 8",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 ABC",

                            ERROR_STATUS_CONFIG_LOAD_MISSING_ARG);
  // Autoplay
  assert_config_exec_status(config,
                            "autoplay move 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  assert_config_exec_status(config,
                            "autoplay ,,, 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_AUTOPLAY_EMPTY_OPTIONS);
  assert_config_exec_status(config,
                            "autoplay game -10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES);
  assert_config_exec_status(config,
                            "autoplay game 10a -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES);
  assert_config_exec_status(config,
                            "autoplay game h -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_AUTOPLAY_MALFORMED_NUM_GAMES);
  assert_config_exec_status(config,
                            "autoplay game 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_SUCCESS);
  // Create
  assert_config_exec_status(
      config, "create klx CSW50 english",
      ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_CREATE_DATA_TYPE);
  config_destroy(config);
  config = config_create_default_test();

  // Leave Gen
  assert_config_exec_status(config, "leavegen 2 0",

                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);

  load_and_exec_config_or_die(config, "set -l1 CSW21 -l2 NWL20");
  assert_config_exec_status(config, "leavegen 2 0",
                            ERROR_STATUS_LEAVE_GEN_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 NWL20");
  assert_config_exec_status(config, "leavegen 2 0",
                            ERROR_STATUS_LEAVE_GEN_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 NWL20");
  assert_config_exec_status(config, "leavegen 2 0",
                            ERROR_STATUS_LEAVE_GEN_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 CSW21");
  assert_config_exec_status(config, "leavegen 1 -1",

                            ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(
      config, "leavegen 1,,1 0",
      ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS);
  assert_config_exec_status(
      config, "leavegen 1,2,3,h 0",
      ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS);
  assert_config_exec_status(
      config, "leavegen 1,2,3,-4 0",
      ERROR_STATUS_AUTOPLAY_MALFORMED_MINIMUM_LEAVE_TARGETS);
  assert_config_exec_status(config, "autoplay games,winpct 10000 -gp true",
                            ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  assert_config_exec_status(config, "autoplay games,leaves 10000 -gp true",
                            ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  assert_config_exec_status(config,
                            "autoplay winpct,games,leaves 10000 -gp true",
                            ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  config_destroy(config);
}

void test_config_wmp(void) {
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const PlayersData *players_data = config_get_players_data(config);
  WMP *wmp1 = NULL;
  WMP *wmp2 = NULL;

  // Players start off with no wmp
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 1000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Both players should share the same wmp
  test_config_load_error(config, "set -wmp true", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) ==
         players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1));
  wmp1 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0);
  wmp2 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 1000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) ==
         players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1));
  assert(wmp1 == players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0));
  assert(wmp2 == players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1));

  // Unset the wmp for player one
  test_config_load_error(config, "set -w1 false", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == wmp1);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 100", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == wmp1);

  // Unset the wmp for player two
  test_config_load_error(config, "set -w2 false", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 1000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Set the wmp for player one
  test_config_load_error(config, "set -w1 true", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // wmp_create should have been called, but this does not guarantee that the
  // wmp pointers differ from what they were previously. We need some other way
  // to validate that data was reloaded, but until then commenting this out
  // because it breaks tests in release mode.

  // assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != wmp1);
  // assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != wmp1);

  wmp1 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 100000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != wmp1);

  // Change lexicons
  test_config_load_error(config, "set -lex NWL20", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);
  // The wmp should be a different pointer now
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != wmp1);
  wmp1 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 100000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != wmp1);

  // Set the wmp for player two
  test_config_load_error(config, "set -w2 true", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == wmp1);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 100000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == wmp1);

  // Change lexicons
  test_config_load_error(config, "set -lex CSW21", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) ==
         players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1));

  config_destroy(config);
  error_stack_destroy(error_stack);
}

void test_config(void) {
  test_config_load_error_cases();
  test_config_load_success();
  test_config_lexical_data();
  test_config_exec_parse_args();
  test_config_wmp();
}
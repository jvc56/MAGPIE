#include "config_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/trie.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_GCG_FILENAME "a.gcg"

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
  test_config_load_error(config, "playfortricks",
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
  test_config_load_error(config, "set -leaves FRA20",
                         ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING, error_stack);
  test_config_load_error(config, "set -k1 NWL20",
                         ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING, error_stack);
  test_config_load_error(config, "set -k2 CSW21",
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
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                         error_stack);
  test_config_load_error(config, "sim -it 0",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -minp 0",
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
  test_config_load_error(config, "sim -im 23434.32433.4324",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "sim -im -3",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -im -4.5",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -im none",
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
  const char *l1 = "CSW21";
  const char *l2 = "NWL20";
  const char *s1 = "score";
  const char *r1 = "all";
  const char *s2 = "equity";
  const char *r2 = "best";
  int num_plays = 10;
  int plies = 4;
  uint64_t max_iterations = 400;
  int stopping_cond = 98;
  int seed = 101;
  int number_of_threads = 6;
  int print_info = 200;

  StringBuilder *test_string_builder = string_builder_create();
  string_builder_add_formatted_string(
      test_string_builder,
      "set -ld %s -bb %d -var %s -l1 %s -l2 %s -s1 %s -r1 "
      "%s -s2 %s -r2 %s  -numplays %d "
      "-plies %d -it "
      "%lu -scond %d -seed %d -threads %d -pfreq %d -gp true -hr true ",
      ld_name, bingo_bonus, game_variant, l1, l2, s1, r1, s2, r2, num_plays,
      plies, max_iterations, stopping_cond, seed, number_of_threads,
      print_info);

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
  assert(config_get_num_plays(config) == num_plays);
  assert(config_get_plies(config) == plies);
  assert(config_get_max_iterations(config) == max_iterations);
  assert(within_epsilon(config_get_stop_cond_pct(config), 98));
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
  plies = 23;
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
  assert(config_get_num_plays(config) == num_plays);
  assert(config_get_plies(config) == plies);
  assert(config_get_max_iterations(config) == max_iterations);
  assert(within_epsilon(config_get_stop_cond_pct(config), 98));
  assert(!config_get_use_game_pairs(config));
  assert(!config_get_human_readable(config));

  string_builder_destroy(test_string_builder);
  config_destroy(config);
}

void assert_lexical_data(Config *config, const char *cmd, const char *l1_name,
                         const char *l2_name, const char *k1_name,
                         const char *k2_name, const char *w1_name,
                         const char *w2_name, const char *ld_name) {
  load_and_exec_config_or_die(config, cmd);
  const PlayersData *pd = config_get_players_data(config);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KWG, 0),
                       l1_name);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KWG, 1),
                       l2_name);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KLV, 0),
                       k1_name);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_KLV, 1),
                       k2_name);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_WMP, 0),
                       w1_name);
  assert_strings_equal(players_data_get_data_name(pd, PLAYERS_DATA_TYPE_WMP, 1),
                       w2_name);
  const LetterDistribution *ld = config_get_ld(config);
  if (!ld) {
    assert_strings_equal(ld_name, NULL);
  } else {
    assert_strings_equal(ld_get_name(ld), ld_name);
  }
}

void test_config_lexical_data(void) {
  Config *config = config_create_default_test();
  // Check that defaults are set correctly
  assert_lexical_data(config, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", "CSW21", "english");
  // Check that lexicons, leaves, and ld change change
  // successfully if they belong to the same ld type.
  assert_lexical_data(config, "set -l2 NWL20 -ld english_blank_is_5 -k1 NWL20",
                      "CSW21", "NWL20", "NWL20", "CSW21", "CSW21", "NWL20",
                      "english_blank_is_5");
  // The leaves and ld should stay the same since they are
  // the same ld type.
  assert_lexical_data(config, "set -lex CSW21", "CSW21", "CSW21", "NWL20",
                      "CSW21", "CSW21", "CSW21", "english_blank_is_5");
  // Check that the leaves arg behaves as expected
  assert_lexical_data(config, "set -leaves CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", "CSW21", "english_blank_is_5");
  // Check that the leaves arg behaves as expected
  assert_lexical_data(config, "set -leaves NWL20", "CSW21", "CSW21", "NWL20",
                      "NWL20", "CSW21", "CSW21", "english_blank_is_5");
  // Check that defaults are set correctly when switching to a new language
  // and that settings are preserved across commands
  assert_lexical_data(config, "set -lex FRA20", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", "french");
  assert_lexical_data(config, "set -minp 10", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", "french");
  assert_lexical_data(config, "set -wmp false", "FRA20", "FRA20", "FRA20",
                      "FRA20", NULL, NULL, "french");
  assert_lexical_data(config, "set -minp 100", "FRA20", "FRA20", "FRA20",
                      "FRA20", NULL, NULL, "french");
  assert_lexical_data(config, "set -wmp false -w1 true", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", NULL, "french");
  assert_lexical_data(config, "set -minp 20", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", NULL, "french");
  assert_lexical_data(config, "set -wmp true -w1 false", "FRA20", "FRA20",
                      "FRA20", "FRA20", NULL, "FRA20", "french");
  assert_lexical_data(config, "set -minp 30", "FRA20", "FRA20", "FRA20",
                      "FRA20", NULL, "FRA20", "french");
  assert_lexical_data(config, "set -w1 true -w2 false", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", NULL, "french");
  assert_lexical_data(config, "set -minp 40", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", NULL, "french");
  assert_lexical_data(config, "set -wmp true", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", "french");
  assert_lexical_data(config, "set -minp 50", "FRA20", "FRA20", "FRA20",
                      "FRA20", "FRA20", "FRA20", "french");
  config_destroy(config);

  // Test default use when available settings for WMP
  Config *config2 = config_create_default_test();
  assert_lexical_data(config2, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config2, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", "CSW21", "english");
  config_destroy(config2);

  Config *config3 = config_create_default_test();
  assert_lexical_data(config3, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config3, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", NULL, NULL, "english");
  config_destroy(config3);

  Config *config4 = config_create_default_test();
  assert_lexical_data(config4, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config4, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config4, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", NULL, NULL, "english");
  config_destroy(config4);

  Config *config5 = config_create_default_test();
  assert_lexical_data(config5, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config5, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config5, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", "CSW21", "english");
  config_destroy(config5);

  Config *config6 = config_create_default_test();
  assert_lexical_data(config6, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config6, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config6, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config6, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", "CSW21", "english");
  config_destroy(config6);

  Config *config7 = config_create_default_test();
  assert_lexical_data(config7, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config7, "set -wmp true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config7, "set -wmp false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config7, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", NULL, NULL, "english");
  config_destroy(config7);

  Config *config8 = config_create_default_test();
  assert_lexical_data(config8, "set -wmp false -w1 true", NULL, NULL, NULL,
                      NULL, NULL, NULL, NULL);
  assert_lexical_data(config8, "set -w2 true", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config8, "set -w2 false", NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL);
  assert_lexical_data(config8, "set -lex CSW21", "CSW21", "CSW21", "CSW21",
                      "CSW21", "CSW21", NULL, "english");
  config_destroy(config8);
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
  assert_config_exec_status(config, "rack AB3C",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack .ABC",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack AB.C",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack ABC.",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack ABCDEFGH",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack ABCZZZ",
                            ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG);
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack  FF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack  ZYYABCF", ERROR_STATUS_SUCCESS);

  // Generating moves
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);

  // Simulation
  assert_config_exec_status(config, "gen -numplays 2", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -sinfer true",
                            ERROR_STATUS_SIM_GAME_HISTORY_MISSING);
  assert_config_exec_status(config, "set -sinfer false", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim -it 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim AEINR -it 1", ERROR_STATUS_SUCCESS);
  // Check the opp known rack is set correctly
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t ABCDEFG", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gsim -it 1", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "");
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com h8 CABFD", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gsim -it 1", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "ABCDF");
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com h8 CABFD", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gsim -it 1", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "");

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
  assert_config_exec_status(config, "load testdata/gcgs/muzaks_empyrean.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh ABCDE 13",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh ABCDE 13 ABCD",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh ABCDE 13 ABCD EFG",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE EFG",
                            ERROR_STATUS_SUCCESS);
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

  // Load

  Config *config2 = config_create_default_test();
  assert_config_exec_status(
      config2, "load", ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES);
  assert_config_exec_status(config2, "load sheets.google.com",
                            ERROR_STATUS_GCG_PARSE_GAME_EVENT_BEFORE_PLAYER);
  assert_config_exec_status(config2, "load testdata/gcgs/lexicon_missing.gcg",
                            ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED);
  assert_config_exec_status(config2, "load testdata/gcgs/success_standard.gcg",
                            ERROR_STATUS_SUCCESS);
  config_destroy(config2);

  // Show

  Config *config3 = config_create_default_test();
  assert_config_exec_status(config3, "shgame",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  config_destroy(config3);

  Config *config4 = config_create_default_test();
  assert_config_exec_status(config4, "load testdata/gcgs/success_standard.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config4, "shgame", ERROR_STATUS_SUCCESS);
  config_destroy(config4);

  // Next, previous, goto
  Config *config6 = config_create_default_test();

  // Failure case: game not loaded
  assert_config_exec_status(config6, "previous",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config6, "next",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config6, "goto 28",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);

  // Out-of-range failures and expected success behavior
  assert_config_exec_status(config6, "load testdata/gcgs/success_standard.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config6, "previous",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config6, "next", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config6, "goto 28000",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config6, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config6, "next",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config6, "previous", ERROR_STATUS_SUCCESS);
  config_destroy(config6);

  config_destroy(config);
}

void test_config_wmp(void) {
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const PlayersData *players_data = config_get_players_data(config);
  WMP *wmp1 = NULL;
  const WMP *wmp2 = NULL;
  const char *invalid_wmp_name = "invalid wmp name";

  // Players start off with wmp by default
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) != NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) ==
         players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1));
  wmp1 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0);
  wmp2 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 1000", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == wmp1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == wmp2);

  // Turn off the wmp for both players
  test_config_load_error(config, "set -wmp false", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Setting some unrelated fields shouldn't change the status of wmp
  test_config_load_error(config, "set -pfreq 500", ERROR_STATUS_SUCCESS,
                         error_stack);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0) == NULL);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 1) == NULL);

  // Both players should share the same wmp
  test_config_load_error(config, "set -wmp true -pfreq 1000",
                         ERROR_STATUS_SUCCESS, error_stack);
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

  // Update the name of wmp1 to confirm that the update is not persisted when
  // the wmp is reloaded later in this test.
  free(wmp1->name);
  wmp1->name = string_duplicate(invalid_wmp_name);

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

  // The wmp should have been reloaded from the lexicon name.
  wmp1 = players_data_get_data(players_data, PLAYERS_DATA_TYPE_WMP, 0);
  assert_strings_equal(wmp1->name, "CSW21");

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

void test_trie(void) {
  // The Trie struct is used to find the shortest unambiguous strings for each
  // command
  Trie *trie = trie_create();
  assert(trie_get_shortest_unambiguous_index(trie, "anything") == 0);
  assert(trie_get_shortest_unambiguous_index(trie, "apron") == 0);
  assert(trie_get_shortest_unambiguous_index(trie, "banana") == 0);
  trie_add_word(trie, "apple");
  trie_add_word(trie, "banana");
  trie_add_word(trie, "apron");
  trie_add_word(trie, "carrot");
  assert(trie_get_shortest_unambiguous_index(trie, "apron") == 3);
  assert(trie_get_shortest_unambiguous_index(trie, "apple") == 3);
  assert(trie_get_shortest_unambiguous_index(trie, "banana") == 1);
  assert(trie_get_shortest_unambiguous_index(trie, "carrot") == 1);
  trie_add_word(trie, "carry");
  assert(trie_get_shortest_unambiguous_index(trie, "carrot") == 5);
  trie_destroy(trie);
}

void display_whole_game(const char *game_to_load) {
  Config *config = config_create_default_test();
  StringBuilder *cmd_sb = string_builder_create();
  string_builder_add_formatted_string(cmd_sb, "load %s -lex CSW21",
                                      game_to_load);
  assert_config_exec_status(config, string_builder_peek(cmd_sb),
                            ERROR_STATUS_SUCCESS);
  string_builder_clear(cmd_sb);
  const GameHistory *game_history = config_get_game_history(config);
  const int num_events = game_history_get_num_events(game_history);
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j <= num_events; j++) {
      string_builder_add_formatted_string(cmd_sb, "goto %d", j);
      error_code_t error_code =
          get_config_exec_status(config, string_builder_peek(cmd_sb));
      string_builder_clear(cmd_sb);
      if (error_code == ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE) {
        break;
      }
      if (error_code != ERROR_STATUS_SUCCESS) {
        log_fatal("display game test encountered unexpected error: %d",
                  error_code);
      }
    }
    assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
    assert_config_exec_status(config, "set -pretty true", ERROR_STATUS_SUCCESS);
  }
  string_builder_destroy(cmd_sb);
  config_destroy(config);
}

void test_game_display(void) {
  display_whole_game(TESTDATA_FILEPATH "gcgs/success.gcg");
  display_whole_game(TESTDATA_FILEPATH "gcgs/success_standard.gcg");
  display_whole_game(TESTDATA_FILEPATH "gcgs/success_six_pass.gcg");
  display_whole_game(TESTDATA_FILEPATH "gcgs/success_just_last_rack.gcg");
  display_whole_game(TESTDATA_FILEPATH "gcgs/success_long_game.gcg");
}

void test_config_anno(void) {
  // Commit and challenge
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "set -iterations 100",
                            ERROR_STATUS_SUCCESS);

  // Test the help command
  assert_config_exec_status(config, "help holp",
                            ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG);
  assert_config_exec_status(config, "help l",
                            ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND);
  assert_config_exec_status(config, "help set", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "help", ERROR_STATUS_SUCCESS);

  const char *p1_name = "Alice Lastname-Jones";
  const char *p1_nickname = "Alice_Lastname-Jones";
  const char *p2_name = "Bob Lastname-Jones";
  const char *p2_nickname = "Bob_Lastname-Jones";
  StringBuilder *name_sb = string_builder_create();
  assert_config_exec_status(config, "sw",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "note",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "note ",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "note a",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "note a b",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);

  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "sw", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);

  string_builder_add_formatted_string(name_sb, "p1 %s", p1_name);
  assert_config_exec_status(config, string_builder_peek(name_sb),
                            ERROR_STATUS_SUCCESS);
  string_builder_clear(name_sb);
  string_builder_add_formatted_string(name_sb, "p2 %s", p2_name);
  assert_config_exec_status(config, string_builder_peek(name_sb),
                            ERROR_STATUS_SUCCESS);
  string_builder_clear(name_sb);

  assert_config_exec_status(config, "note a b",
                            ERROR_STATUS_NOTE_NO_GAME_EVENTS);

  const GameHistory *game_history = config_get_game_history(config);
  assert_config_exec_status(config, "sw", ERROR_STATUS_SUCCESS);
  assert_strings_equal(game_history_player_get_name(game_history, 0), p2_name);
  assert_strings_equal(game_history_player_get_nickname(game_history, 0),
                       p2_nickname);
  assert_strings_equal(game_history_player_get_name(game_history, 1), p1_name);
  assert_strings_equal(game_history_player_get_nickname(game_history, 1),
                       p1_nickname);

  assert_config_exec_status(config, "sw", ERROR_STATUS_SUCCESS);
  assert_strings_equal(game_history_player_get_name(game_history, 0), p1_name);
  assert_strings_equal(game_history_player_get_nickname(game_history, 0),
                       p1_nickname);
  assert_strings_equal(game_history_player_get_name(game_history, 1), p2_name);
  assert_strings_equal(game_history_player_get_nickname(game_history, 1),
                       p2_nickname);

  p1_name = "a";
  p2_name = "b";
  string_builder_add_formatted_string(name_sb, "p1 %s", p1_name);
  assert_config_exec_status(config, string_builder_peek(name_sb),
                            ERROR_STATUS_SUCCESS);
  string_builder_clear(name_sb);
  string_builder_add_formatted_string(name_sb, "p2 %s", p2_name);
  assert_config_exec_status(config, string_builder_peek(name_sb),
                            ERROR_STATUS_SUCCESS);
  string_builder_clear(name_sb);

  const Game *game = config_get_game(config);
  const Bag *bag = game_get_bag(game);
  const int bag_initial_total = bag_get_letters(bag);

  // Generating moves with 0, 1, and 2 letters should complete without error
  assert_config_exec_status(config, "set -numplays 15", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "r A", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "r AB", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack ABC", ERROR_STATUS_SUCCESS);
  assert(bag_initial_total == bag_get_letters(bag) + 3);
  assert_config_exec_status(config, "rack ABCDEFG", ERROR_STATUS_SUCCESS);
  assert(bag_initial_total == bag_get_letters(bag) + 7);

  // Test error cases
  assert_config_exec_status(config, "com 1",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "com pass ABC",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com pass ABC EFG",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com 8d FADGE XYZ",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 100",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "com pass ABC",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com pass ABC EFG",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com 8d FADGE XYZ",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  // Sim should work normally even after commit errors
  assert_config_exec_status(config, "sim -seed 1 -iterations 100 -minp 50",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gsim -seed 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 0",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "com 100",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "com 1 ABC",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com 1 ABC DEF",
                            ERROR_STATUS_COMMIT_EXTRANEOUS_ARG);
  assert_config_exec_status(config, "com ex",
                            ERROR_STATUS_COMMIT_MISSING_EXCHANGE_OR_PLAY);
  assert_config_exec_status(config, "com 8d",
                            ERROR_STATUS_COMMIT_MISSING_EXCHANGE_OR_PLAY);
  assert_config_exec_status(
      config, "ov a -10",
      ERROR_STATUS_TIME_PENALTY_NO_PREVIOUS_CUMULATIVE_SCORE);

  game = config_get_game(config);
  bag = game_get_bag(game);

  assert(game_get_player_on_turn_index(game) == 0);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 8d FADGE", ERROR_STATUS_SUCCESS);
  assert(bag_initial_total == bag_get_letters(bag) + 5);

  // Check that the note command works

  game_history = config_get_game_history(config);
  assert_config_exec_status(config, "note a", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), "a");

  assert_config_exec_status(config, "note", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), NULL);

  assert_config_exec_status(config, "note  a b", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), " a b");

  assert_config_exec_status(config, "note  a b ", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), " a b ");

  assert_config_exec_status(config, "note", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), NULL);

  assert_config_exec_status(config, "note  ", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), " ");

  assert_config_exec_status(config, "note ", ERROR_STATUS_SUCCESS);
  assert_strings_equal(
      game_history_get_note_for_most_recent_event(game_history), NULL);

  // Test an overtime error case
  assert_config_exec_status(
      config, "ov a -10",
      ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END);

  game = config_get_game(config);
  bag = game_get_bag(game);

  assert_config_exec_status(config, "r XEQUIES", ERROR_STATUS_SUCCESS);
  assert(bag_initial_total == bag_get_letters(bag) + 12);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gsim", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));
  assert_config_exec_status(config, "sim", ERROR_STATUS_SIM_NO_MOVES);

  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "rack JANIZAR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(153));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "chal 7", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "rack DISLINK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "c i9 IDS", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(159));

  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "r SINATE?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "c 11d ANTIQuES", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(274));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "r DISLINK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex LKNSD", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(274));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "rack AAEEOT?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex AAEEOT", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(274));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "rack IIIOOPY", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com PaSs", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(274));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // Test game nav with challenge bonuses

  // Test next
  assert_config_exec_status(config, "goto 0", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(0));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  // The next command should play the turn and the challenge bonus
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // The next command should play the turn and the challenge bonus
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // The next command should play the turn but not the phony tiles returned
  // event
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(159));

  // The next command should play the phony tiles returned event
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(274));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // Test previous
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(159));

  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(0));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  assert_config_exec_status(config, "p",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(0));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  // Test that goto correctly goes to the challenge bonus
  // Both goto 2 and goto 3 should go to the same challenge bonus
  assert_config_exec_status(config, "goto 2", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "goto 3", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // Both goto 4 and goto 5 should go to the same challenge bonus
  assert_config_exec_status(config, "goto 4", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  assert_config_exec_status(config, "goto 5", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // goto the IDS play before it was challenged off
  assert_config_exec_status(config, "goto 6", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(159));

  // goto the IDS play after it was challenged off
  assert_config_exec_status(config, "goto 7", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(130));

  // Remove the challenge on EXEQUIES
  assert_config_exec_status(config, "goto 3", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "unchal", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(28));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim", ERROR_STATUS_SIM_NO_MOVES);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(154));

  // Now that a challenge has been removed, the game event indexes
  // shift down by 1 and goto 3 is now the next play.
  assert_config_exec_status(config, "goto 3", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "goto 4", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(160));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "unchal", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(153));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  // Now that another challenge has been removed, the game event indexes
  // shift down by 1 and goto 3 is now the next play.
  assert_config_exec_status(config, "goto 3", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(153));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "goto EnD", ERROR_STATUS_SUCCESS);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "next",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "goto StARt", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev",
                            ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE);

  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BONSOIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack DISLINK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "rack BONSOIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(258));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(113));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  // Make sure time penalty works after six pass
  assert_config_exec_status(config, "ov a -8", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(250));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(113));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(113));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(113));

  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(258));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(113));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  // **************************************
  // *** Test some six pass error cases ***
  // **************************************
  assert_config_exec_status(config, "rack DISLINK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex DISLINK OVERHOTB",
                            ERROR_STATUS_COMMIT_INVALID_PASS_OUT_RACK);
  assert_config_exec_status(config, "com ex DISLINK OVE4OTB",
                            ERROR_STATUS_COMMIT_INVALID_PASS_OUT_RACK);
  assert_config_exec_status(config, "com ex DISLINK ZZZZZZZ",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BONSOIR", ERROR_STATUS_SUCCESS);
  // This error is from trying to draw ZZZZZZZ after the previous exchange
  assert_config_exec_status(config, "com ex BONSOIR EEEEGGP",
                            ERROR_STATUS_COMMIT_PASS_OUT_RACK_NOT_IN_BAG);
  // Go back one turn to redo the exchange
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack DISLINK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  // In this scenario where the 5th 0 score turn is a pass with DISLINK
  // and the 6th 0 score turn is an exchange drawing into the K, there
  // should be an error thrown because the K is always on the rack
  // of the player who passed holding DISLINK.
  assert_config_exec_status(config, "rack BONSOIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex BONSOIR EEEEGGK",
                            ERROR_STATUS_COMMIT_PASS_OUT_RACK_NOT_IN_BAG);
  // Go back one turn to redo the exchange
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);

  // **************************************
  // ********* Resume normal play *********
  // **************************************
  assert_config_exec_status(config, "com ex DISLINK ?BFHUVY",
                            ERROR_STATUS_SUCCESS);
  // Game was restored from backup after errors
  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(267));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(125));

  assert_config_exec_status(config, "rack BONSOIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex BONSOIR EEEEGGP",
                            ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(256));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(105));

  // Rewind past the six pass and commit a new tile placement move to
  // start towards a standard game end.
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack bonSOIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack DISlink", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack undeway", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack ccttuu?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack gveaway", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack BERLEED", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack OVERHOT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack PLENIPO", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack MAMALIG", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "g", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);

  assert_config_exec_status(config, "rack FOOTROE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "HIRT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)),
                            "EFOOORT");
  assert_config_exec_status(config, "com 2K FOP", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "HIRT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)), "EOORT");
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "HIRT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)),
                            "EFOOORT");
  assert_config_exec_status(config, "com C1 FORDO", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "HIRT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)), "EOT");
  assert_config_exec_status(config, "com N1 HI", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "RT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)), "EOT");
  assert_config_exec_status(config, "com J3 TO", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "RT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)), "E");
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);

  // Commit FOOTROPE
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "rack THIR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(
      config, "com 1", ERROR_STATUS_COMMIT_WAITING_FOR_PASS_OR_CHALLENGE_BONUS);
  // This error triggers a restore from backup, so the game pointer and game
  // history has changed
  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(515));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "rack FOOTROE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "chal 10", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(619));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(605));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(515));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "next", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(605));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "unchal", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  // Test an overtime error case
  assert_config_exec_status(config, "ov a -10",
                            ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY);

  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  // Test overtime cases
  assert_config_exec_status(
      config, "ov c -10",
      ERROR_STATUS_TIME_PENALTY_UNRECOGNIZED_PLAYER_NICKNAME);
  assert_config_exec_status(config, "ov a 10",
                            ERROR_STATUS_TIME_PENALTY_INVALID_VALUE);

  assert_config_exec_status(config, "ov a -11", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(720));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "ov a -10",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  assert_config_exec_status(config, "ov a -100",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);

  assert_config_exec_status(config, "ov b -9", ERROR_STATUS_SUCCESS);
  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(720));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(600));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "ov a -10",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  assert_config_exec_status(config, "ov a -100",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  assert_config_exec_status(config, "ov b -10",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  assert_config_exec_status(config, "ov b -100",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);

  // Go backwards 3 times (past 2 time penalty and one rack end points)
  // to re-commit and remove the time penalties
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(720));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  // Overtime penalties can only be added at the very end of the game
  assert_config_exec_status(config, "ov a -10",
                            ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  game = config_get_game(config);

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(595));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(609));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  game = config_get_game(config);

  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer",
                            ERROR_STATUS_NO_INFERENCE_TO_SHOW);
  assert_config_exec_status(config, "shendgame",
                            ERROR_STATUS_NO_ENDGAME_TO_SHOW);
  // Passing a rack to the top commit should commit the best static move
  assert_config_exec_status(config, "t BARCHAN", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // if a rack is present but there are no moves, moves should be automatically
  // generated to find the top play
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "rack BARCHAN", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "rack BARCHAN -numplays 7",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t BARCHAN", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BARCHAN", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  // Allow the sim to hit the stopping threshold
  assert_config_exec_status(config, "sim -seed 1 -iterations 100 -threads 10",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t", ERROR_STATUS_SUCCESS);
  // No rack was given to the top commit command, so it should commit the best
  // simmed play which should be anything except for 8H BARCHAN.
  const Equity p0_score = player_get_score(game_get_player(game, 0));
  assert(p0_score > int_to_equity(80) && p0_score < int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BARCHAN", ERROR_STATUS_SUCCESS);
  // Generate and sim
  assert_config_exec_status(config, "gsim -seed 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 2", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 5", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t", ERROR_STATUS_SUCCESS);
  // No rack was given to the top commit command, so it should commit the best
  // simmed play 8D BARCHAN should sim best
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(84));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));

  // The generated plays from the previous turn should be cleared.
  assert_config_exec_status(config, "t", ERROR_STATUS_COMMIT_EMPTY_RACK);
  assert_config_exec_status(config, "com 1",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "com 10",
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);

  assert_config_exec_status(config, "rack VVWUUUI -s2 score",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t", ERROR_STATUS_SUCCESS);
  game = config_get_game(config);
  // This tests that
  // - the previously simmed plays are not used
  // - the top commit command uses the player sort type
  // Since player two is sorting by score, they will play E5 VIV(A)
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(84));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(20));

  // Test that E5 VIV(A) is committed using the commit command
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack VVWUUUI -s2 score",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(84));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(20));

  assert_config_exec_status(config, "rack EIINOOX -iterations 100",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com K4 NIXIE", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(123));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(20));

  assert_config_exec_status(config, "rack CEEUUUW", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shendgame",
                            ERROR_STATUS_NO_ENDGAME_TO_SHOW);
  assert_config_exec_status(config, "gsim", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shinfer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shendgame",
                            ERROR_STATUS_NO_ENDGAME_TO_SHOW);

  assert_config_exec_status(config, "load testdata/gcgs/success_standard.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(516));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(362));
  assert_config_exec_status(config, "endgame", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shendgame", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shendgame", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "rack ABCDEFG", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "set -lex FRA20 -ld french",
                            ERROR_STATUS_SUCCESS);
  // Changing the letter distribution to invalidate all of the results
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer",
                            ERROR_STATUS_NO_INFERENCE_TO_SHOW);
  assert_config_exec_status(config, "shendgame",
                            ERROR_STATUS_NO_ENDGAME_TO_SHOW);

  string_builder_destroy(name_sb);
  config_destroy(config);
}

void test_config_export(void) {
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "ex",
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);
  assert_config_exec_status(config, "ex " TEST_GCG_FILENAME,
                            ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING);

  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "ex", ERROR_STATUS_EXPORT_NO_GAME_EVENTS);
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "ex", ERROR_STATUS_EXPORT_NO_GAME_EVENTS);
  assert_config_exec_status(config, "p1 Alice A", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p2 Bob B", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "ex", ERROR_STATUS_EXPORT_NO_GAME_EVENTS);

  assert_config_exec_status(config, "rack ABCDEFG", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(28));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));

  assert_config_exec_status(config, "ex " TEST_GCG_FILENAME,
                            ERROR_STATUS_SUCCESS);

  assert(access(game_history_get_gcg_filename(config_get_game_history(config)),
                F_OK) == 0);

  assert_config_exec_status(config, "rack HIJKLMN", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(28));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(32));

  // This should export with the same "a.gcg" name using the export hotkey
  assert_config_exec_status(config, "e", ERROR_STATUS_SUCCESS);

  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(0));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));

  assert_config_exec_status(config, "load " TEST_GCG_FILENAME,
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(28));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(32));

  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack OPQRSTU", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(52));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));

  // This should use a default name
  assert_config_exec_status(config, "ex", ERROR_STATUS_SUCCESS);

  char *default_name_1 = string_duplicate(
      game_history_get_gcg_filename(config_get_game_history(config)));
  assert(access(default_name_1, F_OK) == 0);

  // Names has not updated, so the "first try" default name will clash with the
  // existing file and it will have to be incremented.
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack ATALAYA", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(78));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));

  // This should use a default name
  assert_config_exec_status(config, "ex", ERROR_STATUS_SUCCESS);

  char *default_name_2 = string_duplicate(
      game_history_get_gcg_filename(config_get_game_history(config)));
  assert(access(default_name_2, F_OK) == 0);

  // Names has not updated, so the "first try" and "second try" default names
  // will clash with the existing files and it will have to be incremented.
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BEZIQUE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(124));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));

  // This should use a default name
  assert_config_exec_status(config, "ex", ERROR_STATUS_SUCCESS);

  char *default_name_3 = string_duplicate(
      game_history_get_gcg_filename(config_get_game_history(config)));
  assert(access(default_name_3, F_OK) == 0);

  StringBuilder *sb_load_cmd = string_builder_create();

  string_builder_add_formatted_string(sb_load_cmd, "load %s", default_name_1);
  assert_config_exec_status(config, string_builder_peek(sb_load_cmd),
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(52));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));
  string_builder_clear(sb_load_cmd);

  string_builder_add_formatted_string(sb_load_cmd, "load %s", default_name_2);
  assert_config_exec_status(config, string_builder_peek(sb_load_cmd),
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(78));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));
  string_builder_clear(sb_load_cmd);

  string_builder_add_formatted_string(sb_load_cmd, "load %s", default_name_3);
  assert_config_exec_status(config, string_builder_peek(sb_load_cmd),
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(124));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(0));
  string_builder_clear(sb_load_cmd);

  remove_or_die(TEST_GCG_FILENAME);
  remove_or_die(default_name_3);
  remove_or_die(default_name_2);
  remove_or_die(default_name_1);

  string_builder_destroy(sb_load_cmd);
  free(default_name_3);
  free(default_name_2);
  free(default_name_1);
  config_destroy(config);
}

void test_config(void) {
  test_game_display();
  test_trie();
  test_config_anno();
  test_config_export();
  test_config_load_error_cases();
  test_config_load_success();
  test_config_exec_parse_args();
  test_config_lexical_data();
  test_config_wmp();
}
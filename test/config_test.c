#include "config_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/trie.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/str/move_string.h"
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
  test_config_load_error(config, "set -pc1 -2",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "set -pc2 nope",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "set -otpenalty -1",
                         ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "set -otperiod 0",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
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
  test_config_load_error(config, "sim -ima 23434.32433.4324",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);
  test_config_load_error(config, "sim -ima -3",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -ima -4.5",
                         ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
                         error_stack);
  test_config_load_error(config, "sim -ima none",
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
  assert_config_exec_status(config,
                            "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
                            "HADJI/ 0/0 0 -lex CSW21;",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(
      config, "add 8A HADJI",
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED);
  assert_config_exec_status(config, "add 8D HADJI", ERROR_STATUS_SUCCESS);

  // Setting the rack
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack AB3C",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  // "." is an alias for "?" (blank tile), so these are well-formed racks.
  assert_config_exec_status(config, "rack .ABC", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack AB.C", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack ABC.", ERROR_STATUS_SUCCESS);
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
  assert_config_exec_status(config, "sim -sinfer true", ERROR_STATUS_SUCCESS);
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
  assert_config_exec_status(config, "rgs RETINAS -it 1", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "");
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com h8 CABFD", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rgs RETINAS -it 1", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "ABCDF");
  assert_config_exec_status(config, "rgs RETINAS - -it 1",
                            ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(
      config_get_ld(config),
      sim_results_get_known_opp_rack(config_get_sim_results(config)), "");
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com h8 CABFD", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rgs RETINAS -it 1", ERROR_STATUS_SUCCESS);
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
  assert_config_exec_status(config, "infer josh ABCDE 13 - EFG",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE EFG",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "infer josh 3 ABCDE -",
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

// Confirms the "rg" command sets the player rack and generates moves (like
// "rack" followed by "generate"), without running a simulation like
// "rgsimulate"/"rgs" does. Also confirms "rg" resolves to itself rather
// than being treated as an ambiguous abbreviation of "rgsimulate": the full
// command name is "rg" so the exact-match check in get_token_from_string
// wins before "rg" is ever considered a prefix of "rgsimulate".
void test_config_rack_and_gen(void) {
  Config *config = config_create_default_test();

  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_SUCCESS);

  // Malformed and unavailable racks are rejected the same way "rack"
  // rejects them.
  assert_config_exec_status(config, "rg AB3C",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rg ABCZZZ",
                            ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG);

  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rg AEINRST", ERROR_STATUS_SUCCESS);

  const Game *game = config_get_game(config);
  const Rack *player_on_turn_rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  assert_rack_equals_string(config_get_ld(config), player_on_turn_rack,
                            "AEINRST");
  assert(move_list_get_count(config_get_move_list(config)) > 0);
  // Unlike "rgsimulate"/"rgs", "rg" must not run a simulation.
  assert(sim_results_get_number_of_plays(config_get_sim_results(config)) == 0);

  config_destroy(config);
}

// Runs the given opponent known rack argument through the "sim", "gsim",
// and "rgs" commands and asserts each returns expected_status. The game is
// reloaded from OPENING_CGP before every invocation so that each command
// sees the same bag/rack state regardless of what a previous simulation
// left behind.
//
// Under OPENING_CGP, player 1 (the opponent) holds HIJKLM? and the bag
// holds everything else: A:8 B:1 C:1 D:3 E:11 F:1 G:2 H:1 I:8 J:0 K:0 L:3
// M:1 N:6 O:8 P:2 Q:1 R:6 S:4 T:6 U:4 V:2 W:2 X:1 Y:2 Z:1 blank:1 (opp_rack
// counts add back on top of the bag when checking drawability, so e.g. H is
// drawable even though the bag alone only has 1 left of the 2 total).
static void assert_sim_family_opp_rack_status(Config *config,
                                              const char *opp_rack_arg,
                                              error_code_t expected_status) {
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen -numplays 2", ERROR_STATUS_SUCCESS);
  char *sim_cmd = get_formatted_string("sim %s -it 1", opp_rack_arg);
  assert_config_exec_status(config, sim_cmd, expected_status);
  free(sim_cmd);

  // gsimulate and rgsimulate generate their own moves, so no prior "gen" is
  // needed.
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  char *gsim_cmd = get_formatted_string("gsim %s -it 1", opp_rack_arg);
  assert_config_exec_status(config, gsim_cmd, expected_status);
  free(gsim_cmd);

  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_SUCCESS);
  char *rgs_cmd = get_formatted_string("rgs RETINAS %s -it 1", opp_rack_arg);
  assert_config_exec_status(config, rgs_cmd, expected_status);
  free(rgs_cmd);
}

// Confirms that an opponent rack specified for the "sim"/"gsimulate"/
// "rgsimulate" commands is validated against the bag the same way the
// player's own rack is, across an empty, partially known, and fully known
// opponent rack. Before this check existed, an opponent rack that was not
// actually available in the bag (e.g. requesting tiles already held by the
// player or already exhausted from the bag) would reach set_random_rack()
// during simulation and log_fatal(), crashing the process instead of
// returning an error.
void test_config_sim_opp_rack_not_in_bag(void) {
  Config *config = config_create_default_test();

  // Empty: '-' forces an unknown/random opponent rack, which skips the bag
  // check entirely, so it must always succeed.
  assert_sim_family_opp_rack_status(config, "-", ERROR_STATUS_SUCCESS);

  // Partially known (fewer than RACK_SIZE letters).
  assert_sim_family_opp_rack_status(config, "AB", ERROR_STATUS_SUCCESS);
  // Only a single Z exists in the English tile distribution, and
  // OPENING_CGP does not place one on the board or in either player's
  // rack, so asking for two Z's in the opponent's rack is impossible.
  assert_sim_family_opp_rack_status(config, "ZZ",
                                    ERROR_STATUS_SIM_OPP_RACK_NOT_IN_BAG);

  // Fully known (RACK_SIZE letters).
  // A rack the bag can actually supply.
  assert_sim_family_opp_rack_status(config, "BCFGHIL", ERROR_STATUS_SUCCESS);
  // The opponent's own currently held rack is trivially drawable.
  assert_sim_family_opp_rack_status(config, "HIJKLM?", ERROR_STATUS_SUCCESS);
  // Only 1 Z exists in total, so 7 of them can never be drawn.
  assert_sim_family_opp_rack_status(config, "ZZZZZZZ",
                                    ERROR_STATUS_SIM_OPP_RACK_NOT_IN_BAG);

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

  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 2G TOOFROPE", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_STANDARD);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(607));
  assert(game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));

  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(731));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(515));
  assert(!game_history_get_waiting_for_final_pass_or_challenge(
      config_get_game_history(config)));
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "HIRT");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)),
                            "EFOOORT");

  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack FOOTROE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
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
  assert_config_exec_status(config, "shpeg", ERROR_STATUS_NO_PEG_TO_SHOW);
  // The -pegoutcomes boolean setting (drives the peg/shpeg outcomes column)
  // parses and loads in both states.
  assert_config_exec_status(config, "set -pegoutcomes true",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "set -pegoutcomes false",
                            ERROR_STATUS_SUCCESS);
  // Passing a rack to the top commit should commit the best static move
  assert_config_exec_status(config, "t BARCHAN", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  // We're now at the live frontier, past the turn that was just
  // committed: nothing's been generated for the new on-turn player yet,
  // and there's no future turn recorded to fall back to, so shmoves
  // correctly shows nothing rather than that committed turn's own
  // (now stale, wrong-rack) analysis.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // if a rack is present but there are no moves, moves should be automatically
  // generated to find the top play
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  // Back at the position the BARCHAN turn above was decided from: its
  // auto-generated move_list (used to pick that top play) is shown again.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BARCHAN", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  // Same as above: we're past the just-committed turn again, with
  // nothing generated for the new position, so nothing to show.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  // Shows the (re-auto-generated) move_list from the "t" just above,
  // saved on this same position.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack BARCHAN -numplays 7",
                            ERROR_STATUS_SUCCESS);
  // Setting a fresh rack to explore this same position again clears the
  // live move list, but doesn't touch the saved historical one, so it's
  // still shown until something new is actually generated.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
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
  assert_config_exec_status(config, "shm 1 -shplies 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 2 -shplies 25", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 5 -shplies 3", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 8d", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm BAR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm 8e BAR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm -", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shm - AB", ERROR_STATUS_SUCCESS);
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
  const Game *old_game = config_get_game(config);
  assert_config_exec_status(config, "set -lex FRA20 -ld french",
                            ERROR_STATUS_SUCCESS);
  // Changing the letter distribution should have triggered a game recreation
  assert(old_game != config_get_game(config));
  // Changing the letter distribution to invalidate all of the results
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);
  assert_config_exec_status(config, "shinfer",
                            ERROR_STATUS_NO_INFERENCE_TO_SHOW);
  assert_config_exec_status(config, "shendgame",
                            ERROR_STATUS_NO_ENDGAME_TO_SHOW);

  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(
      config, "load testdata/gcgs/success_five_point_challenge.gcg",
      ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim", ERROR_STATUS_SIM_GAME_OVER);
  assert_config_exec_status(config, "t", ERROR_STATUS_COMMIT_GAME_OVER);

  // Test autosave
  assert_config_exec_status(config, "newgame au1.gcg", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t RETINAS", ERROR_STATUS_SUCCESS);
  assert(access("au1.gcg", F_OK) != 0);
  assert_config_exec_status(config, "e", ERROR_STATUS_SUCCESS);
  assert(access("au1.gcg", F_OK) == 0);
  char *au1_contents_1 = get_string_from_file_or_die("au1.gcg");
  assert_config_exec_status(config, "t CAZIQUE", ERROR_STATUS_SUCCESS);
  char *au1_contents_2 = get_string_from_file_or_die("au1.gcg");
  assert_strings_equal(au1_contents_1, au1_contents_2);
  assert_config_exec_status(config, "e", ERROR_STATUS_SUCCESS);
  char *au1_contents_3 = get_string_from_file_or_die("au1.gcg");
  assert_strings_ne(au1_contents_1, au1_contents_3);
  remove_or_die("au1.gcg");
  free(au1_contents_1);
  free(au1_contents_2);
  free(au1_contents_3);

  assert_config_exec_status(config, "set -autosave true", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "newgame au2.gcg", ERROR_STATUS_SUCCESS);
  assert(access("au2.gcg", F_OK) == 0);
  char *au2_contents_1 = get_string_from_file_or_die("au2.gcg");
  assert_config_exec_status(config, "t CAZIQUE", ERROR_STATUS_SUCCESS);
  char *au2_contents_2 = get_string_from_file_or_die("au2.gcg");
  assert_strings_ne(au2_contents_1, au2_contents_2);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  char *au2_contents_3 = get_string_from_file_or_die("au2.gcg");
  assert_strings_ne(au2_contents_2, au2_contents_3);
  remove_or_die("au2.gcg");
  free(au2_contents_1);
  free(au2_contents_2);
  free(au2_contents_3);

  string_builder_destroy(name_sb);
  config_destroy(config);
}

void test_config_anno_challenge(void) {
  // Commit and challenge
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "newgame -lex CSW24", ERROR_STATUS_SUCCESS);
  const Game *game = config_get_game(config);
  assert_config_exec_status(config, "t OAKMOSS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t ATALAYA", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t NGLEDUG", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(172));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(86));
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(86));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(91));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(177));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(86));
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(91));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(91));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(86));
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(177));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(86));

  assert_config_exec_status(config, "newgame -lex CSW24", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t OULDGYF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(66));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(71));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(0));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(0));
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(71));
  assert(player_get_score(game_get_player(game, 1)) == int_to_equity(30));

  assert_config_exec_status(config, "newgame -lex CSW24", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r WECH", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 8G WECH", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r HEW", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 8G HEW", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert(bag_get_letters(game_get_bag(config_get_game(config))) == 96);
  assert(bag_get_letter(game_get_bag(config_get_game(config)),
                        ld_hl_to_ml(config_get_ld(config), "C")) == 1);

  assert_config_exec_status(config, "lo testdata/gcgs/success_standard.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 2g TTU", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "EIU");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)),
                            "IOQSTTU");
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 2g TT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 0)), "EIU");
  assert_rack_equals_string(config_get_ld(config),
                            player_get_rack(game_get_player(game, 1)),
                            "IOQSTTU");

  config_destroy(config);
}

void test_config_anno_endgame_rack(void) {
  Config *config = config_create_default_test();

  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r GILLIE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 8D GILLIE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r WOF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 9I WOF", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r VIBED", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 10F VIBED", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINA?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 11H ARENITe", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r EXDRA", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com N9 EXeDRA", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r AMEIOSS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com E2 AMEIOSIS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r VAMP", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com D1 VAMP", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RHUS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com O8 RHUS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r DDGGQT?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex DGGQT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r ZRCON", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com L10 ZIRCON", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r YED", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 15L NYED", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r ADUNC", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 12D ADUNC", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r QI", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 9C QIS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r FUGUE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 7I FUGUE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r ?AYGONE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 13A wAYGONE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r WOK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com F4 WOK", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r JEIE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com A11 JEwIE", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r TTT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com B12 TATT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r TOPHES", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com N2 TOPHES", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r AALORRT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 14D AALROT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r AALORRT", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com M13 ARY", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r ABEILNO", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r BEIINNR", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "s", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r ALOLOTA",
                            ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG);

  config_destroy(config);
}

void test_config_load_incomplete(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const Game *game;

  load_and_exec_config_or_die(config,
                              "load testdata/gcgs/incomplete_after_pass");
  load_and_exec_config_or_die(config, "goto end");
  game = config_get_game(config);
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 360);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 232);
  assert_rack_equals_string(
      game_get_ld(game), player_get_rack(game_get_player(game, 0)), "CDEEFGO");

  load_and_exec_config_or_die(config,
                              "load testdata/gcgs/incomplete_after_exchange");
  load_and_exec_config_or_die(config, "goto end");
  game = config_get_game(config);
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 0);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 0);
  assert_rack_equals_string(
      game_get_ld(game), player_get_rack(game_get_player(game, 1)), "AAENRSZ");

  load_and_exec_config_or_die(
      config, "load testdata/gcgs/incomplete_after_five_point_challenge");
  load_and_exec_config_or_die(config, "goto end");
  game = config_get_game(config);
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 245);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 398);
  assert_rack_equals_string(
      game_get_ld(game), player_get_rack(game_get_player(game, 0)), "AEIOOST");

  assert(game_history_get_num_events(config_get_game_history(config)) > 0);
  load_and_exec_config_or_die(config, "set -ld english_small");
  assert(game_history_get_num_events(config_get_game_history(config)) == 0);

  config_destroy(config);
}

// The game ends on six passes with an empty bag. The tiles each player has to
// draw back for the end rack penalties are sitting on the other player's rack,
// so both racks must be returned to the bag before either player draws.
void test_config_six_pass_empty_bag(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  load_and_exec_config_or_die(config, "load testdata/gcgs/success_standard");
  // Rewind to the position just before the final outplay, where the bag is
  // empty, HastyBot holds I and RightBehindYou holds OST.
  load_and_exec_config_or_die(config, "goto 26");
  const Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 0);
  assert_rack_equals_string(game_get_ld(game),
                            player_get_rack(game_get_player(game, 0)), "I");
  assert_rack_equals_string(game_get_ld(game),
                            player_get_rack(game_get_player(game, 1)), "OST");
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 516);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 362);

  for (int pass_index = 0; pass_index < 6; pass_index++) {
    assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  }

  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  // Both players keep the rack they passed with and are penalized for it.
  assert_rack_equals_string(game_get_ld(game),
                            player_get_rack(game_get_player(game, 0)), "I");
  assert_rack_equals_string(game_get_ld(game),
                            player_get_rack(game_get_player(game, 1)), "OST");
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 515);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 359);
  // Six passes plus the two end rack penalty events.
  assert(game_history_get_num_events(config_get_game_history(config)) == 34);

  config_destroy(config);
}

// The game ends on six passes with fewer than RACK_SIZE tiles left in the bag,
// so the two racks do not account for every unseen tile.
void test_config_six_pass_partial_bag(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  load_and_exec_config_or_die(config, "load testdata/gcgs/success_standard");
  // Rewind to a position with 5 tiles left in the bag.
  load_and_exec_config_or_die(config, "goto 22");
  // RightBehindYou is on turn holding ILOOPQR. The other twelve unseen tiles
  // are still in the bag object, seven of which are HastyBot's unknown rack,
  // leaving five tiles that neither player will hold.
  const Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 12);

  for (int pass_index = 0; pass_index < 6; pass_index++) {
    if (pass_index % 2 == 0) {
      assert_config_exec_status(config, "rack ILOOPQR", ERROR_STATUS_SUCCESS);
    } else {
      assert_config_exec_status(config, "rack EEIIPRU", ERROR_STATUS_SUCCESS);
    }
    assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);
  }

  game = config_get_game(config);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(equity_to_int(player_get_score(game_get_player(game, 0))) == 472);
  assert(equity_to_int(player_get_score(game_get_player(game, 1))) == 271);

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

void test_config_challenge_rack(void) {
  Config *config = config_create_default_test();
  // This triggered a segfault in prod at one point
  assert_config_exec_status(config,
                            "load testdata/gcgs/malformed_challenge_rack.gcg",
                            ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "chal", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t ABC", ERROR_STATUS_SUCCESS);
  config_destroy(config);
}

static void assert_note_with_move_ref(Config *config, int move_idx) {
  const MoveList *ml = config_get_move_list(config);
  const Move *move = move_list_get_move(ml, move_idx - 1);
  const Game *game = config_get_game(config);

  StringBuilder *expected_sb = string_builder_create();
  string_builder_add_move(expected_sb, game_get_board(game), move,
                          config_get_ld(config), false);

  StringBuilder *cmd_sb = string_builder_create();
  string_builder_add_formatted_string(cmd_sb, "note $%d", move_idx);
  assert_config_exec_status(config, string_builder_peek(cmd_sb),
                            ERROR_STATUS_SUCCESS);

  const GameHistory *gh = config_get_game_history(config);
  assert_strings_equal(game_history_get_note_for_most_recent_event(gh),
                       string_builder_peek(expected_sb));

  string_builder_destroy(expected_sb);
  string_builder_destroy(cmd_sb);
}

void test_config_note_move_interpolation(void) {
  // Adding a note after navigating back to the start should not crash.
  Config *config = config_create_or_die("set -lex CSW21");
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com h8 RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "note test",
                            ERROR_STATUS_NOTE_NO_GAME_EVENTS);
  config_destroy(config);

  // No moves: game has events but no moves have been generated.
  config = config_create_or_die("set -lex CSW21");
  assert_config_exec_status(config, "load testdata/gcgs/success.gcg",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "note $1",
                            ERROR_STATUS_NOTE_NO_GAME_EVENTS);
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "n", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "note $1",
                            ERROR_STATUS_NOTE_NO_MOVES_TO_INTERPOLATE);
  config_destroy(config);

  // Set up a game with generated moves for the remaining tests.
  // Use numplays 150 with RETINAS after one play to guarantee >= 100 moves.
  config = config_create_or_die("set -lex CSW21 -numplays 150");
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p1 a", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "p2 b", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "t RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "r RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);

  // Move index out of range.
  const MoveList *ml = config_get_move_list(config);
  const int count = move_list_get_count(ml);
  StringBuilder *cmd_sb = string_builder_create();
  string_builder_add_formatted_string(cmd_sb, "note $%d", count + 1);
  assert_config_exec_status(config, string_builder_peek(cmd_sb),
                            ERROR_STATUS_NOTE_MOVE_INDEX_OUT_OF_RANGE);
  string_builder_destroy(cmd_sb);

  // 1-digit index.
  assert_note_with_move_ref(config, 1);

  // 2-digit index.
  assert_note_with_move_ref(config, 10);

  // 3-digit index.
  assert(count >= 100);
  assert_note_with_move_ref(config, 100);

  // Multiple move references in a single note.
  const Game *game = config_get_game(config);
  StringBuilder *expected_sb = string_builder_create();
  string_builder_add_string(expected_sb, "should have played ");
  string_builder_add_move(expected_sb, game_get_board(game),
                          move_list_get_move(ml, 0), config_get_ld(config),
                          false);
  string_builder_add_string(expected_sb, " instead of ");
  string_builder_add_move(expected_sb, game_get_board(game),
                          move_list_get_move(ml, 2), config_get_ld(config),
                          false);
  assert_config_exec_status(config, "note should have played $1 instead of $3",
                            ERROR_STATUS_SUCCESS);
  const GameHistory *gh = config_get_game_history(config);
  assert_strings_equal(game_history_get_note_for_most_recent_event(gh),
                       string_builder_peek(expected_sb));
  string_builder_destroy(expected_sb);

  config_destroy(config);
}

void test_config_fg_required(void) {
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);

  // fgrequired defaults to false in test config, so newgame without a
  // filename should succeed.
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);

  // Enable fgrequired; newgame without a filename should now fail.
  assert_config_exec_status(config, "set -fgrequired true",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "newgame",
                            ERROR_STATUS_CONFIG_LOAD_MISSING_ARG);
  config_destroy(config);
}

void test_config_exchange_blank(void) {
  Config *config = config_create_default_test();
  assert_config_exec_status(config, "set -lex CSW21", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);

  // "com ex ?" when the rack has no blank should return a validation
  // error rather than crashing. BLANK_MACHINE_LETTER == PLAYED_THROUGH_MARKER
  // == 0, so the blank was previously skipped when building tiles_played_rack,
  // causing the rack-subtract check to pass silently and later triggering an
  // assert in rack_take_letter.
  assert_config_exec_status(config, "rack RETINAS", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(
      config, "com ex ?",
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK);

  // "com ex u" (lowercase) should unblank to uppercase U and exchange the U
  // tile rather than storing a blanked machine letter in the move and causing
  // an out-of-bounds rack access. Only '?' may denote the blank in an exchange.
  assert_config_exec_status(config, "rack QUIOEU?", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex u", ERROR_STATUS_SUCCESS);

  // Exchange both blanks from a rack with 5 non-blank tiles and 2 blanks.
  // impl_set_rack_internal returns the off-turn player's tiles to the bag
  // before drawing, so after "rack ABCDE??" the bag holds 93 tiles with
  // 0 blanks. execute_exchange_move draws new tiles before returning the
  // exchanged tiles to the bag, so both drawn tiles are guaranteed non-blank.
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack ABCDE??", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex ??", ERROR_STATUS_SUCCESS);
  const Game *game = config_get_game(config);

  // After game_play_n_events replays the history, the live player rack is
  // empty: set_after_game_event_racks has no future events from which to
  // infer the post-exchange rack for the last event. Verify the exchange
  // is recorded correctly through the game history event instead. The
  // pre-exchange rack stored in the event must have the 5 non-blank tiles
  // plus the 2 blanks that were exchanged, and the validated move must
  // store exactly 2 blank machine letters as the exchanged tiles.
  const GameHistory *game_history = config_get_game_history(config);
  const GameEvent *last_event = game_history_get_event(
      game_history, game_history_get_num_played_events(game_history) - 1);
  const Rack *rack = game_event_get_const_rack(last_event);
  const LetterDistribution *ld = game_get_ld(game);
  assert(rack_get_total_letters(rack) == RACK_SIZE);
  assert(rack_get_letter(rack, BLANK_MACHINE_LETTER) == 2);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "A")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "B")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "C")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "D")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "E")) == 1);
  const Move *exchange_move =
      validated_moves_get_move(game_event_get_vms(last_event), 0);
  assert(exchange_move->tiles_played == 2);
  assert(exchange_move->tiles[0] == BLANK_MACHINE_LETTER);
  assert(exchange_move->tiles[1] == BLANK_MACHINE_LETTER);

  // Same test but with 1 blank
  assert_config_exec_status(config, "newgame", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "rack ABCDE??", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com ex ?", ERROR_STATUS_SUCCESS);
  game = config_get_game(config);

  game_history = config_get_game_history(config);
  last_event = game_history_get_event(
      game_history, game_history_get_num_played_events(game_history) - 1);
  rack = game_event_get_const_rack(last_event);
  ld = game_get_ld(game);
  assert(rack_get_total_letters(rack) == RACK_SIZE);
  assert(rack_get_letter(rack, BLANK_MACHINE_LETTER) == 2);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "A")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "B")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "C")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "D")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "E")) == 1);
  exchange_move = validated_moves_get_move(game_event_get_vms(last_event), 0);
  assert(exchange_move->tiles_played == 1);
  assert(exchange_move->tiles[0] == BLANK_MACHINE_LETTER);

  config_destroy(config);
}

void test_config_utility_blend(void) {
  ErrorStack *error_stack = error_stack_create();

  // Defaults: (1.0, 0.5, 100.0), fanned out identically to both players.
  {
    Config *config = config_create_default_test();
    assert(within_epsilon(config_get_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_utility_w_spread(config), 0.5));
    assert(within_epsilon(config_get_utility_spread_scale(config), 100.0));
    assert(within_epsilon(config_get_p1_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_p1_utility_w_spread(config), 0.5));
    assert(within_epsilon(config_get_p1_utility_spread_scale(config), 100.0));
    assert(within_epsilon(config_get_p2_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_p2_utility_w_spread(config), 0.5));
    assert(within_epsilon(config_get_p2_utility_spread_scale(config), 100.0));
    config_destroy(config);
  }

  // Global -uwin / -uspread / -uspreadscale fans out to both players.
  {
    Config *config = config_create_default_test();
    load_and_exec_config_or_die(config,
                                "set -uwin 0.7 -uspread 0.3 -uspreadscale 80");
    assert(within_epsilon(config_get_utility_w_winpct(config), 0.7));
    assert(within_epsilon(config_get_utility_w_spread(config), 0.3));
    assert(within_epsilon(config_get_utility_spread_scale(config), 80.0));
    assert(within_epsilon(config_get_p1_utility_w_winpct(config), 0.7));
    assert(within_epsilon(config_get_p1_utility_w_spread(config), 0.3));
    assert(within_epsilon(config_get_p1_utility_spread_scale(config), 80.0));
    assert(within_epsilon(config_get_p2_utility_w_winpct(config), 0.7));
    assert(within_epsilon(config_get_p2_utility_w_spread(config), 0.3));
    assert(within_epsilon(config_get_p2_utility_spread_scale(config), 80.0));
    config_destroy(config);
  }

  // Per-player flags override the global on the matching player only.
  {
    Config *config = config_create_default_test();
    load_and_exec_config_or_die(config,
                                "set -uwin 0.5 -uspread 0.5 -uspreadscale 100 "
                                "-uwin1 1 -uspread1 2 -uspreadscale1 50 "
                                "-uwin2 1 -uspread2 0");
    // Global retains what was set.
    assert(within_epsilon(config_get_utility_w_winpct(config), 0.5));
    assert(within_epsilon(config_get_utility_w_spread(config), 0.5));
    assert(within_epsilon(config_get_utility_spread_scale(config), 100.0));
    // P1 overridden on all three.
    assert(within_epsilon(config_get_p1_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_p1_utility_w_spread(config), 2.0));
    assert(within_epsilon(config_get_p1_utility_spread_scale(config), 50.0));
    // P2 overridden on w_winpct/w_spread, scale inherited from global.
    assert(within_epsilon(config_get_p2_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_p2_utility_w_spread(config), 0.0));
    assert(within_epsilon(config_get_p2_utility_spread_scale(config), 100.0));
    config_destroy(config);
  }

  // Per-player flags alone (no global) override only the matching player;
  // the other player keeps the defaults.
  {
    Config *config = config_create_default_test();
    load_and_exec_config_or_die(config, "set -uwin1 0.4 -uspread1 0.6");
    assert(within_epsilon(config_get_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_utility_w_spread(config), 0.5));
    assert(within_epsilon(config_get_p1_utility_w_winpct(config), 0.4));
    assert(within_epsilon(config_get_p1_utility_w_spread(config), 0.6));
    assert(within_epsilon(config_get_p2_utility_w_winpct(config), 1.0));
    assert(within_epsilon(config_get_p2_utility_w_spread(config), 0.5));
    config_destroy(config);
  }

  // Validation cases: reuse one config across all (test_config_load_error
  // calls error_stack_reset, so subsequent calls aren't affected).
  Config *err_config = config_create_default_test();

  // Both global weights zero is rejected.
  test_config_load_error(err_config, "set -uwin 0 -uspread 0",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);

  // P1 weights summing to zero (via per-player overrides) is rejected
  // even when the global is positive.
  test_config_load_error(err_config, "set -uwin1 0 -uspread1 0",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);

  // P2 weights summing to zero is rejected.
  test_config_load_error(err_config, "set -uwin2 0 -uspread2 0",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);

  // Non-numeric value is rejected.
  test_config_load_error(err_config, "set -uwin abc",
                         ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                         error_stack);

  config_destroy(err_config);
  error_stack_destroy(error_stack);
}

// "shmoves" should still be able to show (and filter/limit into) the
// move_list results of a "gen" command after a commit changes the
// position: they're duplicated onto the GameEvent the commit created
// (see config_save_live_results_to_game_event), and shown as a fallback
// once nothing's live. The saved move_list is a real object, not a
// frozen rendering, so ordinary "shmoves" filter args keep working
// against it, and viewing it doesn't consume/clear it.
void test_config_move_list_saved_to_game_event(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -mode sync");
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? "
      "0/0 0",
      ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert(move_list_get_count(config_get_move_list(config)) > 1);

  // Committing changes the position and clears the live move list right
  // away, same as always.
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(move_list_get_count(config_get_move_list(config)) == 0);

  // The move_list that was live just before the commit is now saved on
  // the GameEvent that commit created -- it belongs to the turn that was
  // just decided (and the rack that turn was decided from), not to the
  // new position that's on screen now.
  const GameHistory *game_history = config_get_game_history(config);
  const GameEvent *committed_event = game_history_get_event(
      game_history, game_history_get_num_played_events(game_history) - 1);
  const MoveList *saved_move_list = game_event_get_move_list(committed_event);
  assert(saved_move_list && move_list_get_count(saved_move_list) > 1);
  assert(!game_event_get_sim_results(committed_event));

  // Right after the commit, nothing is live for the new on-turn player
  // (nothing generated yet for their rack), and there's no future event to
  // fall back to either, so shmoves correctly finds nothing to show.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // Navigating back to the position the committed move was actually chosen
  // from finds its saved move_list, as a real object, so ordinary filter
  // args (a max-count filter, here) still work against it...
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves 1", ERROR_STATUS_SUCCESS);

  // ...and viewing it doesn't consume/clear it, so a second (unfiltered)
  // "shmoves" still shows it too.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  config_destroy(config);
}

// A move committed by rank ("com 9") must resolve against whatever's
// actually being shown right now, exactly like "shmoves" does: after
// navigating back to a past position with nothing freshly generated, the
// move at that rank should come from the position's own saved move_list
// instead of failing with "no generated moves".
void test_config_commit_by_index_uses_saved_move_list(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -mode sync");
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? "
      "0/0 0",
      ERROR_STATUS_SUCCESS);

  // Turn 1: gen (many possible openings for ABCDEFG on an empty board),
  // then commit a pass instead of one of the generated moves. The
  // generated move_list -- not the pass that was actually played -- is
  // what gets saved onto this turn's event.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  const int num_turn_1_moves =
      move_list_get_count(config_get_move_list(config));
  assert(num_turn_1_moves >= 9);
  // Pick a non-top rank so this can't pass by accident via some other
  // "always commit rank 1" code path.
  const int chosen_rank = 9;
  Move chosen_move;
  move_copy(&chosen_move,
            move_list_get_move(config_get_move_list(config), chosen_rank - 1));
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);

  // Right after committing, nothing is live and we're at the live
  // frontier, so committing by index fails cleanly.
  StringBuilder *commit_cmd_sb = string_builder_create();
  string_builder_add_formatted_string(commit_cmd_sb, "com %d", chosen_rank);
  char *commit_cmd = string_builder_dump(commit_cmd_sb, NULL);
  string_builder_destroy(commit_cmd_sb);
  assert_config_exec_status(config, commit_cmd,
                            ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE);

  // Navigate back to the position turn 1 was decided from (the start of
  // the game): nothing is live here either, but its own saved move_list
  // is available (the same one "shmoves" would fall back to).
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert(move_list_get_count(config_get_move_list(config)) == 0);

  const Game *game = config_get_game(config);
  const int player_on_turn = game_get_player_on_turn_index(game);
  const Equity score_before =
      player_get_score(game_get_player(game, player_on_turn));

  // Committing by that same rank now commits the move from that saved
  // list rather than failing with "no generated moves".
  assert_config_exec_status(config, commit_cmd, ERROR_STATUS_SUCCESS);
  const Equity score_after =
      player_get_score(game_get_player(game, player_on_turn));
  assert(score_after == score_before + move_get_score(&chosen_move));
  free(commit_cmd);

  config_destroy(config);
}

// The SimResults saved onto a GameEvent is a full deep copy (see
// sim_results_duplicate), not just the move_list: it survives the live
// sim_results being invalidated on commit, supports the usual shmoves
// filter args, and is completely independent of (doesn't alias) the
// live, now-invalid sim_results.
void test_config_sim_results_saved_to_game_event(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 5 -mode sync");
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? "
      "0/0 0",
      ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -iterations 100",
                            ERROR_STATUS_SUCCESS);
  const SimResults *live_sim_results = config_get_sim_results(config);
  assert(sim_results_get_valid_for_current_game_state(live_sim_results));
  const int num_simmed_plays =
      sim_results_get_number_of_plays(live_sim_results);
  assert(num_simmed_plays > 1);

  // Committing changes the position and invalidates the live sim_results
  // right away, same as always; the object itself is untouched (its
  // identity never changes), just marked invalid.
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);
  assert(config_get_sim_results(config) == live_sim_results);
  assert(!sim_results_get_valid_for_current_game_state(live_sim_results));

  // The sim_results that were live and valid just before the commit are
  // now saved (as an independent deep copy, not the same object) on the
  // GameEvent that commit created -- again, the turn that was just
  // decided, not the new position now on screen.
  const GameHistory *game_history = config_get_game_history(config);
  const GameEvent *committed_event = game_history_get_event(
      game_history, game_history_get_num_played_events(game_history) - 1);
  const SimResults *saved_sim_results =
      game_event_get_sim_results(committed_event);
  assert(saved_sim_results && saved_sim_results != live_sim_results);
  assert(sim_results_get_number_of_plays(saved_sim_results) ==
         num_simmed_plays);

  // Right after the commit, nothing is live for the new on-turn player and
  // there's no future event to fall back to, so shmoves finds nothing.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // Navigating back to the position the sim was actually run from finds
  // the saved sim results, a real independent SimResults object, so an
  // ordinary shmoves max-count filter still works against them.
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves 1", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  config_destroy(config);
}

// The whole point of saving results per GameEvent rather than in a
// single "last turn" slot: navigating game history backward/forward
// shows the analysis that was actually done for whichever turn is about
// to be played from the current position -- i.e. the turn whose rack
// matches what's actually on screen -- not whatever was most recently
// committed and not the turn that was just played to get here.
void test_config_game_event_results_follow_navigation(void) {
  // -sinfer false: this test is only about move_list/sim_results, and
  // sim-with-inference (the default) can fail outright once a turn passes
  // with no tiles played/exchanged, which "com 1" may do depending on
  // what's in the generated move list.
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -mode sync -sinfer false");
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? "
      "0/0 0",
      ERROR_STATUS_SUCCESS);

  // Turn 1: only "gen" (no sim) before committing.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  // Turn 2: "gen" then "sim" before committing.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -iterations 100",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com pass", ERROR_STATUS_SUCCESS);

  const GameHistory *game_history = config_get_game_history(config);
  const GameEvent *event_1 = game_history_get_event(game_history, 0);
  const GameEvent *event_2 = game_history_get_event(game_history, 1);
  // Turn 1 only ever generated moves, so only a move_list was saved.
  assert(game_event_get_move_list(event_1));
  assert(!game_event_get_sim_results(event_1));
  // Turn 2 simmed, so its sim_results were saved (in addition to its
  // move_list).
  assert(game_event_get_sim_results(event_2));

  // Right after committing turn 2, we're at the live frontier (no turn 3
  // recorded yet) with nothing freshly generated, so shmoves finds
  // nothing -- turn 2's own results belong to the position it was
  // decided from, not to the new position on screen now.
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // Stepping back to the position turn 2 was actually decided from shows
  // turn 2's (sim) results: that's the turn about to be played here.
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  // Stepping back once more, to the position turn 1 was decided from,
  // shows turn 1's (plain move_list) results instead of turn 2's.
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  // "goto start" lands on that same position (before turn 1 was played),
  // so it shows turn 1's results too, not nothing.
  assert_config_exec_status(config, "goto start", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  // "goto end" returns to the live frontier, where there's nothing to
  // fall back to again.
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  // Turn 3: "gen" (no sim) before committing.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  // Turn 4: "gen" then a quick "sim" before committing. Only a couple of
  // moves/iterations: this isn't testing anything about the sim itself,
  // just that its saved results are still reachable after navigating more
  // than 2 turns away in either direction.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -numplays 2 -iterations 2",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  // Turn 5: "gen" (no sim) before committing.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  // Turn 6: "gen" then another quick "sim" before committing.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -numplays 2 -iterations 2",
                            ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  const GameEvent *event_4 = game_history_get_event(game_history, 3);
  assert(game_event_get_sim_results(event_4));

  // Currently positioned at the live frontier, just after committing turn
  // 6. "goto 3" jumps back to the position turn 4 was decided from (3
  // turns already played, turn 4 next): turn 4's sim results are shown,
  // matching the rack actually on screen at that position.
  assert_config_exec_status(config, "goto 3", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);

  // From there, "goto end" jumps forward to the live frontier again,
  // past turn 6 (the last recorded turn), where there's nothing to fall
  // back to.
  assert_config_exec_status(config, "goto end", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_NO_MOVES_TO_SHOW);

  config_destroy(config);
}

// A sim that runs inference internally (-sinfer true) leaves
// inference_results's own "valid for current game state" flag restored to
// its pre-sim state, so a naive check of that flag alone would skip saving
// the inference onto the GameEvent. config_save_live_results_to_game_event
// also checks sim_used_valid_inference for exactly this case, so both the
// sim_results and the inference_results it used end up saved on the same
// GameEvent.
void test_config_sim_with_inference_results_saved_to_game_event(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 2 -mode sync -sinfer true");
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/HIJKLM? "
      "0/0 0",
      ERROR_STATUS_SUCCESS);

  // Turn 1: a played event must already exist in the history for a sim to
  // use inference at all (it infers the opponent's leave from their move).
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  // Turn 2: "gen" then a quick sim that runs inference internally.
  assert_config_exec_status(config, "gen", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "sim -iterations 2", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "com 1", ERROR_STATUS_SUCCESS);

  const GameHistory *game_history = config_get_game_history(config);
  const GameEvent *event_2 = game_history_get_event(game_history, 1);
  assert(game_event_get_sim_results(event_2));
  assert(game_event_get_inference_results(event_2));

  // Stepping back to the position turn 2 was decided from shows both:
  // shmoves for the sim results, and shinfer falling back to the
  // inference results saved alongside them.
  assert_config_exec_status(config, "prev", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shmoves", ERROR_STATUS_SUCCESS);
  assert_config_exec_status(config, "shinfer", ERROR_STATUS_SUCCESS);

  config_destroy(config);
}

void test_config(void) {
  test_game_display();
  test_trie();
  test_config_anno();
  test_config_anno_challenge();
  test_config_anno_endgame_rack();
  test_config_load_incomplete();
  test_config_six_pass_empty_bag();
  test_config_six_pass_partial_bag();
  test_config_challenge_rack();
  test_config_export();
  test_config_load_error_cases();
  test_config_load_success();
  test_config_exec_parse_args();
  test_config_rack_and_gen();
  test_config_sim_opp_rack_not_in_bag();
  test_config_lexical_data();
  test_config_wmp();
  test_config_note_move_interpolation();
  test_config_fg_required();
  test_config_exchange_blank();
  test_config_utility_blend();
  test_config_move_list_saved_to_game_event();
  test_config_commit_by_index_uses_saved_move_list();
  test_config_sim_results_saved_to_game_event();
  test_config_game_event_results_follow_navigation();
  test_config_sim_with_inference_results_saved_to_game_event();
}

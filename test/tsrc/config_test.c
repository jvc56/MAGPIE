#include "../../src/impl/config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/def/autoplay_defs.h"
#include "../../src/def/config_defs.h"
#include "../../src/def/game_defs.h"
#include "../../src/def/leave_gen_defs.h"
#include "../../src/def/move_defs.h"
#include "../../src/def/players_data_defs.h"
#include "../../src/def/simmer_defs.h"
#include "../../src/def/validated_move_defs.h"
#include "../../src/ent/game.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/players_data.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/thread_control.h"
#include "../../src/impl/cgp.h"
#include "../../src/str/rack_string.h"
#include "../../src/util/string_util.h"
#include "config_test.h"
#include "test_constants.h"
#include "test_util.h"

void test_config_load_error(Config *config, const char *cmd,
                            config_load_status_t expected_status) {
  config_load_status_t actual_status = config_load_command(config, cmd);
  if (actual_status != expected_status) {
    printf("config status mismatched:\nexpected: %d\nactual: %d\n>%s<\n",
           expected_status, actual_status, cmd);
    assert(0);
  }
}

void test_config_load_error_cases(void) {
  Config *config = config_create_default_test();
  test_config_load_error(config, "endgame",
                         CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_load_error(config, "sim -lex CSW21 -iter 1000 -plies 10 1",
                         CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_load_error(config, "sim -plies 3 -plies 4",
                         CONFIG_LOAD_STATUS_DUPLICATE_ARG);
  test_config_load_error(config, "sim -it 1000 -infer",
                         CONFIG_LOAD_STATUS_MISPLACED_COMMAND);
  test_config_load_error(config, "sim -i 1000",
                         CONFIG_LOAD_STATUS_AMBIGUOUS_COMMAND);
  test_config_load_error(config, "sim -it 1000 -l2 CSW21",
                         CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_load_error(config, "set -mode uci",
                         CONFIG_LOAD_STATUS_UNRECOGNIZED_EXEC_MODE);
  test_config_load_error(config, "set -gp on",
                         CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG);
  test_config_load_error(config, "set -gp off",
                         CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG);
  test_config_load_error(config, "set -hr on",
                         CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG);
  test_config_load_error(config, "set -hr off",
                         CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG);
  test_config_load_error(config, "set -seed -2",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -lex CSW21 -it 1000 -plies",
                         CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_load_error(config, "cgp 1 2 3",
                         CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_load_error(config, "create klv CSW50",
                         CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_load_error(config, "sim -bdn invalid_number_of_rows15",
                         CONFIG_LOAD_STATUS_BOARD_LAYOUT_ERROR);
  test_config_load_error(config, "sim -var Lonify",
                         CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT);
  test_config_load_error(config, "sim -bb 3b4",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -s1 random",
                         CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_load_error(config, "sim -s2 none",
                         CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_load_error(config, "sim -r1 top",
                         CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_load_error(config, "sim -r2 3",
                         CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_load_error(config, "sim -numplays three",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -numplays 123R456",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -numplays -2",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -plies two",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -plies -3",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -iter six",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -it -6",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -it 0",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -scond -95",
                         CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -scond 102",
                         CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -scond F",
                         CONFIG_LOAD_STATUS_MALFORMED_DOUBLE_ARG);
  test_config_load_error(config, "sim -eq 23434.32433.4324",
                         CONFIG_LOAD_STATUS_MALFORMED_DOUBLE_ARG);
  test_config_load_error(config, "sim -eq -3",
                         CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -eq -4.5",
                         CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -eq none",
                         CONFIG_LOAD_STATUS_MALFORMED_DOUBLE_ARG);
  test_config_load_error(config, "sim -seed zero",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -threads many",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -threads 0",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -threads -100",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -pfreq x",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -pfreq -40",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -cfreq z",
                         CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_load_error(config, "sim -cfreq -90",
                         CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  test_config_load_error(config, "sim -l1 CSW21",
                         CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_load_error(config, "sim -l1 CSW21 -l2 DISC2",
                         CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_load_error(config, "sim -l1 OSPS49 -l2 DISC2",
                         CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_load_error(config, "sim -l1 NWL20 -l2 OSPS49",
                         CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_load_error(config, "sim -l1 NWL20 -l2 NWL20 -k2 DISC2",
                         CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_load_error(config, "sim -l1 NWL20 -l2 CSW21 -ld german",
                         CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION);
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
  int check_stop = 700;

  StringBuilder *test_string_builder = string_builder_create();
  string_builder_add_formatted_string(
      test_string_builder,
      "set -ld %s -bb %d -var %s -l1 %s -l2 %s -s1 %s -r1 "
      "%s -s2 %s -r2 %s -eq %0.2f -numplays %d "
      "-plies %d -it "
      "%d -scond %d -seed %d -threads %d -pfreq %d -cfreq %d -gp true -hr true "
      "-p1 %s "
      "-p2 "
      "%s",
      ld_name, bingo_bonus, game_variant, l1, l2, s1, r1, s2, r2, equity_margin,
      num_plays, plies, max_iterations, stopping_cond, seed, number_of_threads,
      print_info, check_stop, p1, p2);

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
  assert(thread_control_get_check_stop_interval(
             config_get_thread_control(config)) == check_stop);
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
  assert(thread_control_get_check_stop_interval(
             config_get_thread_control(config)) == check_stop);
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
                               error_status_t expected_error_status_type,
                               int expected_error_status_code) {
  config_load_status_t status = config_load_command(config, cmd);
  if (status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_fatal("load config failed with status %d: %s\n", status, cmd);
  }

  config_execute_command(config);

  error_status_t actual_error_status_type =
      error_status_get_type(config_get_error_status(config));

  bool mismatch = false;

  if (actual_error_status_type != expected_error_status_type) {
    printf("config exec error types do not match:\nexpected: %d\nactual: "
           "%d\n>%s<\n",
           expected_error_status_type, actual_error_status_type, cmd);
    mismatch = true;
  }

  int actual_error_status_code =
      error_status_get_code(config_get_error_status(config));

  if (actual_error_status_code != expected_error_status_code) {
    printf("config exec error codes do not match:\nexpected: %d\nactual: "
           "%d\n>%s<\n",
           expected_error_status_code, actual_error_status_code, cmd);
    mismatch = true;
  }

  if (mismatch) {
    abort();
  }
}

void test_config_exec_parse_args(void) {
  Config *config = config_create_default_test();

  // Ensure all commands that require game data fail correctly
  assert_config_exec_status(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_TYPE_CONFIG_LOAD, CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
  assert_config_exec_status(config, "addmoves 1", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
  assert_config_exec_status(config, "gen", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
  assert_config_exec_status(config, "sim", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
  assert_config_exec_status(config, "infer 0 3", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
  assert_config_exec_status(config, "autoplay game 10",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);

  // CGP
  assert_config_exec_status(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 1/2 0/0 0 -lex CSW21",
      ERROR_STATUS_TYPE_CGP_LOAD, CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS);
  assert_config_exec_status(config, "cgp " VS_OXY, ERROR_STATUS_TYPE_NONE, 0);

  // Adding moves
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "add 8A.HADJI -lex CSW21",
                            ERROR_STATUS_TYPE_MOVE_VALIDATION,
                            MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED);
  assert_config_exec_status(config, "add 8D.HADJI -lex CSW21",
                            ERROR_STATUS_TYPE_NONE, 0);

  // Setting the rack
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "rack 0 ABC", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "rack 3 ABC", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "rack 1 AB3C",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "rack 1 ABCZZZ",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_RACK_NOT_IN_BAG);
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "rack 1 FF", ERROR_STATUS_TYPE_NONE, 0);
  assert_config_exec_status(config, "rack 1 ZYYABCF", ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "rack 2 CC", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_RACK_NOT_IN_BAG);

  // Generating moves
  assert_config_exec_status(config, "cgp " OPENING_CGP, ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "gen", ERROR_STATUS_TYPE_NONE, 0);

  // Simulation
  assert_config_exec_status(config, "gen -numplays 2", ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "sim AEIN3R -it 1",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "sim -it 1", ERROR_STATUS_TYPE_NONE, 0);
  assert_config_exec_status(config, "sim AEINR -it 1", ERROR_STATUS_TYPE_NONE,
                            0);

  // Inference
  assert_config_exec_status(config, "cgp " EMPTY_CGP, ERROR_STATUS_TYPE_NONE,
                            0);
  assert_config_exec_status(config, "infer 0 ABC 14",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 3 ABC 14",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 AB3C 14",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "infer 1 ABC 1R4",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 -4", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG);
  assert_config_exec_status(config, "infer 1 8", ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  assert_config_exec_status(config, "infer 1 ABC",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_MISSING_ARG);
  // Autoplay
  assert_config_exec_status(
      config, "autoplay move 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
      ERROR_STATUS_TYPE_AUTOPLAY, AUTOPLAY_STATUS_INVALID_OPTIONS);
  assert_config_exec_status(
      config, "autoplay ,,, 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
      ERROR_STATUS_TYPE_AUTOPLAY, AUTOPLAY_STATUS_EMPTY_OPTIONS);
  assert_config_exec_status(config,
                            "autoplay game 10 -l1 CSW21 -l2 NWL20 -r1 b -r2 b",
                            ERROR_STATUS_TYPE_NONE, 0);

  // Create
  assert_config_exec_status(config, "create klx CSW50 english",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_UNRECOGNIZED_CREATE_DATA_TYPE);
  config_destroy(config);
  config = config_create_default_test();

  // Leave Gen
  assert_config_exec_status(config, "leavegen 2 20 1 0",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_GAME_DATA_MISSING);

  load_and_exec_config_or_die(config, "set -l1 CSW21 -l2 NWL20");
  assert_config_exec_status(config, "leavegen 2 20 1 0",
                            ERROR_STATUS_TYPE_LEAVE_GEN,
                            LEAVE_GEN_STATUS_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 NWL20");
  assert_config_exec_status(config, "leavegen 2 20 1 0",
                            ERROR_STATUS_TYPE_LEAVE_GEN,
                            LEAVE_GEN_STATUS_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 NWL20");
  assert_config_exec_status(config, "leavegen 2 20 1 0",
                            ERROR_STATUS_TYPE_LEAVE_GEN,
                            LEAVE_GEN_STATUS_DIFFERENT_LEXICA_OR_LEAVES);

  load_and_exec_config_or_die(config,
                              "set -l1 CSW21 -l2 CSW21 -k1 CSW21 -k2 CSW21");

  assert_config_exec_status(config, "leavegen 0 20 1 0",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);

  assert_config_exec_status(config, "leavegen 2 0 1 5",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);

  assert_config_exec_status(config, "leavegen 2 20 0 60",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);

  assert_config_exec_status(config, "leavegen 2 20 1 -1",
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS);

  config_destroy(config);
}

void test_config(void) {
  test_config_load_error_cases();
  test_config_load_success();
  test_config_lexical_data();
  test_config_exec_parse_args();
}
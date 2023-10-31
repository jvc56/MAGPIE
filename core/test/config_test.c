#include <assert.h>
#include <stddef.h>

#include "../src/config.h"
#include "../src/log.h"
#include "../src/string_util.h"

#include "config_test.h"
#include "test_constants.h"
#include "test_util.h"

void test_config_error(Config *config, const char *cmd,
                       config_load_status_t expected_status) {
  config_load_status_t actual_status = load_config(config, cmd, false);
  if (actual_status != expected_status) {
    printf("config status mismatched: %d != %d\n>%s<\n", expected_status,
           actual_status, cmd);
    assert(0);
  }
}

void load_config_or_fail(Config *config, const char *cmd) {
  config_load_status_t actual_status = load_config(config, cmd, false);
  if (actual_status != CONFIG_LOAD_STATUS_SUCCESS) {
    printf("config load failed with error code %d\n>%s<\n", actual_status, cmd);
    assert(0);
  }
}

void test_config_error_cases() {
  Config *config = create_default_config();
  test_config_error(config, "position gcg",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim lex CSW21 i 1000 plies 10 1",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim sim", CONFIG_LOAD_STATUS_DUPLICATE_ARG);
  test_config_error(config, "sim go", CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND);
  test_config_error(config, "go sim i 1000 infer",
                    CONFIG_LOAD_STATUS_MISPLACED_COMMAND);
  test_config_error(config, "go sim i 1000",
                    CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_error(config, "go sim lex CSW21 i 1000 plies",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_error(config, "position cgp 1 2 3",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_error(config, "go sim bdn Scrimbool",
                    CONFIG_LOAD_STATUS_UNKNOWN_BOARD_LAYOUT);
  test_config_error(config, "go sim var Lonify",
                    CONFIG_LOAD_STATUS_UNKNOWN_GAME_VARIANT);
  test_config_error(config, "go sim bb 3b4",
                    CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS);
  test_config_error(config, "go sim s1 random",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_error(config, "go sim s2 none",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_error(config, "go sim r1 top",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_error(config, "go sim r2 3",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_error(config, "go sim lex CSW21 rack ABCD1EF",
                    CONFIG_LOAD_STATUS_MALFORMED_RACK);
  test_config_error(config, "go sim lex CSW21 rack 6",
                    CONFIG_LOAD_STATUS_MALFORMED_RACK);
  test_config_error(config, "go sim numplays three",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim numplays 123R456",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim numplays -2",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim plies two",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim plies -3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim i six",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim i -6",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim stop 96",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim stop -95",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim stop NO",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim pindex 3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim pindex -1",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim pindex one",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim score over9000",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim score -11",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim eq 23434.32433.4324",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq -3",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq -4.5",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq none",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim exch five",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED);
  test_config_error(config, "go sim exch -4",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED);
  test_config_error(config, "go sim rs zero",
                    CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED);
  test_config_error(config, "go sim rs -4",
                    CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED);
  test_config_error(config, "go sim threads many",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim threads -100",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim info x",
                    CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL);
  test_config_error(config, "go sim info -40",
                    CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL);
  test_config_error(config, "go sim check z",
                    CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL);
  test_config_error(config, "go sim check -90",
                    CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL);
  test_config_error(config, "go sim l1 CSW21 l2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 OSPS44 l2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 NWL20 l2 OSPS44",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  destroy_config(config);
}

void test_config_success() {
  Config *config = create_default_config();

  const char *ld_name = "french";
  int bingo_bonus = 73;
  const char *game_variant = "wordsmog";
  const char *l1 = "CSW21";
  const char *l2 = "NWL20";
  const char *s1 = "score";
  const char *r1 = "all";
  const char *s2 = "equity";
  const char *r2 = "best";
  // The rack must be in alphabetical
  // order for testing convenience.
  const char *rack = "BISTUUU";
  int pindex = 1;
  int score = 15;
  int number_exch = 3;
  double equity_margin = 4.6;
  int num_plays = 10;
  int plies = 4;
  int max_iterations = 400;
  int stopping_cond = 98;
  int random_seed = 101;
  int number_of_threads = 6;
  int print_info = 200;
  int check_stop = 700;

  StringBuilder *test_string_builder = create_string_builder();
  string_builder_add_formatted_string(
      test_string_builder,
      "setoptions ld %s bb %d var %s l1 %s l2 %s s1 %s r1 "
      "%s s2 %s r2 %s rack %s pindex %d score %d exch %d eq %0.2f numplays %d "
      "plies %d i "
      "%d stop %d rs %d threads %d info %d check %d static gp",
      ld_name, bingo_bonus, game_variant, l1, l2, s1, r1, s2, r2, rack, pindex,
      score, number_exch, equity_margin, num_plays, plies, max_iterations,
      stopping_cond, random_seed, number_of_threads, print_info, check_stop);

  load_config_or_fail(config, string_builder_peek(test_string_builder));

  assert(config->command_type == COMMAND_TYPE_SET_OPTIONS);
  assert(!config->command_set_cgp);
  assert(config->game_variant == GAME_VARIANT_WORDSMOG);
  assert(players_data_get_move_sort_type(config->players_data, 0) ==
         MOVE_SORT_SCORE);
  assert(players_data_get_move_record_type(config->players_data, 0) ==
         MOVE_RECORD_ALL);
  assert(players_data_get_move_sort_type(config->players_data, 1) ==
         MOVE_SORT_EQUITY);
  assert(players_data_get_move_record_type(config->players_data, 1) ==
         MOVE_RECORD_BEST);
  assert(config->bingo_bonus == bingo_bonus);
  assert(config->player_to_infer_index == pindex);
  assert(config->actual_score == score);
  assert(config->number_of_tiles_exchanged == number_exch);
  assert(within_epsilon(config->equity_margin, equity_margin));
  assert(config->num_plays == num_plays);
  assert(config->plies == plies);
  assert(config->max_iterations == max_iterations);
  assert(config->stopping_condition == SIM_STOPPING_CONDITION_98PCT);
  assert(config->random_seed == (uint64_t)random_seed);
  assert(config->thread_control->number_of_threads == number_of_threads);
  assert(config->thread_control->print_info_interval == print_info);
  assert(config->thread_control->check_stopping_condition_interval ==
         check_stop);
  assert(config->static_search_only);
  assert(config->use_game_pairs);

  assert(strings_equal(config->ld_name, ld_name));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 0),
                       l1));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 1),
                       l2));
  // KLVs should use the same name
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 0),
                       l1));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 1),
                       l2));

  string_builder_clear(test_string_builder);
  string_builder_add_rack(config->rack, config->letter_distribution,
                          test_string_builder);
  assert(strings_equal(rack, string_builder_peek(test_string_builder)));

  // Save KWG pointers as these shouldn't be reused
  KWG *p1_csw_kwg =
      players_data_get_data(config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  KWG *p2_nwl_kwg =
      players_data_get_data(config->players_data, PLAYERS_DATA_TYPE_KWG, 1);

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
  rack = "-";
  number_exch = 2;
  plies = 123;
  max_iterations = 6;
  number_of_threads = 900;
  print_info = 850;

  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(
      test_string_builder,
      "setoptions ld %s bb %d l1 %s l2 %s  s1 "
      "%s r1 %s s2 %s r2 %s rack %s exch %d plies %d i %d "
      "threads %d "
      "info %d nostatic nogp",
      ld_name, bingo_bonus, l1, l2, s1, r1, s2, r2, rack, number_exch, plies,
      max_iterations, number_of_threads, print_info);

  load_config_or_fail(config, string_builder_peek(test_string_builder));

  assert(config->command_type == COMMAND_TYPE_SET_OPTIONS);
  assert(!config->command_set_cgp);
  assert(config->game_variant == GAME_VARIANT_WORDSMOG);
  assert(players_data_get_move_sort_type(config->players_data, 0) ==
         MOVE_SORT_EQUITY);
  assert(players_data_get_move_record_type(config->players_data, 0) ==
         MOVE_RECORD_BEST);
  assert(players_data_get_move_sort_type(config->players_data, 1) ==
         MOVE_SORT_SCORE);
  assert(players_data_get_move_record_type(config->players_data, 1) ==
         MOVE_RECORD_ALL);
  assert(config->bingo_bonus == bingo_bonus);
  assert(config->player_to_infer_index == pindex);
  assert(config->actual_score == score);
  assert(config->number_of_tiles_exchanged == number_exch);
  assert(within_epsilon(config->equity_margin, equity_margin));
  assert(config->num_plays == num_plays);
  assert(config->plies == plies);
  assert(config->max_iterations == max_iterations);
  assert(config->stopping_condition == SIM_STOPPING_CONDITION_98PCT);
  assert(config->random_seed == (uint64_t)random_seed);
  assert(config->thread_control->number_of_threads == number_of_threads);
  assert(config->thread_control->print_info_interval == print_info);
  assert(config->thread_control->check_stopping_condition_interval ==
         check_stop);
  assert(!config->static_search_only);
  assert(!config->use_game_pairs);

  assert(strings_equal(config->ld_name, ld_name));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 0),
                       l1));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 1),
                       l2));
  // KLVs should use the same name
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 0),
                       l1));
  assert(strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 1),
                       l2));
  assert(config->rack->empty);

  // The players data should have swapped the lexicons
  // and not created or destroyed any new KWGs
  KWG *p1_nwl_kwg =
      players_data_get_data(config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  KWG *p2_csw_kwg =
      players_data_get_data(config->players_data, PLAYERS_DATA_TYPE_KWG, 1);
  assert(p1_csw_kwg == p2_csw_kwg);
  assert(p1_nwl_kwg == p2_nwl_kwg);

  // Test emptying the rack and move sort/record key words
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder,
                            "setoptions rack - s1 score r1 all", 0);
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  // Position CGP command
  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(test_string_builder, "position cgp %s",
                                      DOUG_V_EMELY_CGP);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert(config->command_type == COMMAND_TYPE_LOAD_CGP);
  assert(config->command_set_cgp);
  assert_strings_equal(config->cgp, "15/15/15/15/15/15/15/3WINDY7/15/15/15/15/"
                                    "15/15/15 ADEEGIL/AEILOUY 0/32 0");

  // Ensure that cgp arg works for all commands

  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(test_string_builder, "go sim cgp %s",
                                      NOAH_VS_MISHU_CGP);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert(config->command_type == COMMAND_TYPE_SIM);
  assert(config->command_set_cgp);
  assert_strings_equal(
      config->cgp, "15/15/15/15/15/15/12BOA/6VOX1ATONY/7FIVER3/11E3/8MOANED1/"
                   "7QI2C3/8MU1H3/15/15 AAIRSSU/AELORTT 76/120 0");

  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(test_string_builder, "go infer cgp %s",
                                      NOAH_VS_PETER_CGP);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert(config->command_type == COMMAND_TYPE_INFER);
  assert(config->command_set_cgp);
  assert_strings_equal(
      config->cgp,
      "15/9O5/7GIP5/7H1E3T1/7E5E1/5CUTTY3EF/7T6O/7OR4UP/7SEMINAL1/4AX2S4V1/"
      "2DILUTION3AE/8L5V/8D5I/8e5T/8R5E AFK/ABIMQSW 172/249 0");

  // Seting CGP for autoplay is useless but should still work
  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(test_string_builder, "go autoplay cgp %s",
                                      SOME_ISC_GAME_CGP);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert(config->command_type == COMMAND_TYPE_AUTOPLAY);
  assert(config->command_set_cgp);
  assert_strings_equal(
      config->cgp, "15/1V13/1O13/1D12P/1U12O/1NA2C7FE/2N2R7AM/OUTFLYING2AHIS/"
                   "2I1O2AILERON1/2R1O10/2I1I2D2WAG2/2O1EXPUNgES3/2t4C7/"
                   "6SKOALING1/7Y7 EEHMRTZ/AEIJQRU 264/306 0");

  // English and French should be able to play each other
  // with either distribution
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder,
                            "setoptions ld english l1 CSW21 l2 FRA20", 0);
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder,
                            "setoptions ld french l1 FRA20 l2 CSW21", 0);
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "setoptions lex NWL20", 0);
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 0),
                       "NWL20");
  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 1),
                       "NWL20");

  // Correctly set leave and letter distribution defaults
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "setoptions lex FRA20", 0);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 0),
                       "FRA20");
  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KWG, 1),
                       "FRA20");
  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 0),
                       "FRA20");
  assert_strings_equal(players_data_get_data_name(config->players_data,
                                                  PLAYERS_DATA_TYPE_KLV, 1),
                       "FRA20");
  assert_strings_equal(config->ld_name, "french");

  destroy_string_builder(test_string_builder);
  destroy_config(config);
}

void test_config() {
  test_config_error_cases();
  test_config_success();
}
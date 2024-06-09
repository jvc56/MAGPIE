#include "../../src/impl/config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../src/def/config_defs.h"
#include "../../src/def/game_defs.h"
#include "../../src/def/move_defs.h"
#include "../../src/def/players_data_defs.h"
#include "../../src/def/simmer_defs.h"
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

void test_config_error(Config *config, const char *cmd,
                       config_load_status_t expected_status) {
  config_load_status_t actual_status = config_load_command(config, cmd);
  if (actual_status != expected_status) {
    printf("config status mismatched: %d != %d\n>%s<\n", expected_status,
           actual_status, cmd);
    assert(0);
  }
}

void load_config_or_fail(Config *config, const char *cmd) {
  config_load_status_t actual_status = config_load_command(config, cmd);
  if (actual_status != CONFIG_LOAD_STATUS_SUCCESS) {
    printf("config load failed with error code %d\n>%s<\n", actual_status, cmd);
    assert(0);
  }
}

void test_config_error_cases() {
  Config *config = config_create_default();
  test_config_error(config, "position gcg",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim lex CSW21 i 1000 plies 10 1",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim sim", CONFIG_LOAD_STATUS_DUPLICATE_ARG);
  test_config_error(config, "go sim i 1000 infer",
                    CONFIG_LOAD_STATUS_MISPLACED_COMMAND);
  test_config_error(config, "go sim i 1000",
                    CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_error(config, "go sim i 1000 l2 CSW21",
                    CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_error(config, "go sim lex CSW21 i 1000 plies",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_error(config, "position cgp 1 2 3",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);

  const char *target = "../../test/testdata/invalid_number_of_rows15.txt";
  const char *link_name = "data/layouts/invalid_number_of_rows15.txt";

  if (symlink(target, link_name) != 0) {
    perror("symlink");
    log_fatal("Failed to create symlink: %s %s", target, link_name);
  }

  test_config_error(config, "go sim bdn invalid_number_of_rows15",
                    CONFIG_LOAD_STATUS_BOARD_LAYOUT_ERROR);

  if (unlink(link_name) != 0) {
    perror("unlink");
    log_fatal("Failed to destroy symlink: %s %s", target, link_name);
  }

  test_config_error(config, "go sim var Lonify",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT);
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
                    CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_error(config, "go sim numplays 123R456",
                    CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_error(config, "go sim numplays -2",
                    CONFIG_LOAD_STATUS_MALFORMED_INT_ARG);
  test_config_error(config, "go sim plies two",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim plies -3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim i six",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim i -6",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim i 0",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim cond 96",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim cond -95",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim cond NO",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim pindex 3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX);
  test_config_error(config, "go sim pindex -1",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX);
  test_config_error(config, "go sim pindex one",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX);
  test_config_error(config, "go sim score over9000",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim score -11",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim eq 23434.32433.4324",
                    CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG);
  test_config_error(config, "go sim eq -3",
                    CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG);
  test_config_error(config, "go sim eq -4.5",
                    CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG);
  test_config_error(config, "go sim eq none",
                    CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG);
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
  test_config_error(config, "go sim threads 0",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim threads -100",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim threads 9001",
                    CONFIG_LOAD_STATUS_EXCEEDED_MAX_NUMBER_OF_THREADS);
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
  test_config_error(config, "go sim l1 OSPS49 l2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 NWL20 l2 OSPS49",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 NWL20 l2 NWL20 k2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 NWL20 l2 CSW21 ld german",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION);
  test_config_error(config, "convert convtype kwg2exe",
                    CONFIG_LOAD_STATUS_MALFORMED_CONVERSION_TYPE);
  test_config_error(config, "go sim ucgi console",
                    CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES);
  config_destroy(config);
}

void test_config_success() {
  Config *config = config_create_default();
  Game *game = NULL;

  // Loading with whitespace should not fail
  load_config_or_fail(config, "           ");

  const char *ld_name = "french";
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

  StringBuilder *test_string_builder = create_string_builder();
  string_builder_add_formatted_string(
      test_string_builder,
      "ld %s bb %d var %s l1 %s l2 %s s1 %s r1 "
      "%s s2 %s r2 %s eq %0.2f numplays %d "
      "plies %d i "
      "%d cond %d rs %d threads %d info %d check %d gp p1 %s p2 %s",
      ld_name, bingo_bonus, game_variant, l1, l2, s1, r1, s2, r2, equity_margin,
      num_plays, plies, max_iterations, stopping_cond, seed, number_of_threads,
      print_info, check_stop, p1, p2);

  load_config_or_fail(config, string_builder_peek(test_string_builder));
  game = config_get_game(config);

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
  assert(config_get_seed(config) == (uint64_t)seed);
  assert(thread_control_get_threads(config_get_thread_control(config)) ==
         number_of_threads);
  assert(thread_control_get_print_info_interval(
             config_get_thread_control(config)) == print_info);
  assert(thread_control_get_check_stop_interval(
             config_get_thread_control(config)) == check_stop);
  assert(config_get_use_game_pairs(config));

  assert_strings_equal(p1, player_get_name(game_get_player(game, 0)));
  assert_strings_equal(p2, player_get_name(game_get_player(game, 1)));

  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KWG, 0),
                    l1));
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KWG, 1),
                    l2));
  // KLVs should use the same name
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KLV, 0),
                    l1));
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KLV, 1),
                    l2));

  // Save KWG pointers as these shouldn't be reused
  const KWG *p1_csw_kwg = players_data_get_data(config_get_players_data(config),
                                                PLAYERS_DATA_TYPE_KWG, 0);
  const KWG *p2_nwl_kwg = players_data_get_data(config_get_players_data(config),
                                                PLAYERS_DATA_TYPE_KWG, 1);

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
  string_builder_add_formatted_string(test_string_builder,
                                      "setoptions ld %s bb %d l1 %s l2 %s  s1 "
                                      "%s r1 %s s2 %s r2 %s plies %d i %d "
                                      "threads %d "
                                      "info %d nogp",
                                      ld_name, bingo_bonus, l1, l2, s1, r1, s2,
                                      r2, plies, max_iterations,
                                      number_of_threads, print_info);

  load_config_or_fail(config, string_builder_peek(test_string_builder));

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

  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KWG, 0),
                    l1));
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KWG, 1),
                    l2));
  // KLVs should use the same name
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KLV, 0),
                    l1));
  assert(
      strings_equal(players_data_get_data_name(config_get_players_data(config),
                                               PLAYERS_DATA_TYPE_KLV, 1),
                    l2));

  // The players data should have swapped the lexicons
  // and not created or destroyed any new KWGs
  const KWG *p1_nwl_kwg = players_data_get_data(config_get_players_data(config),
                                                PLAYERS_DATA_TYPE_KWG, 0);
  const KWG *p2_csw_kwg = players_data_get_data(config_get_players_data(config),
                                                PLAYERS_DATA_TYPE_KWG, 1);
  assert(p1_csw_kwg == p2_csw_kwg);
  assert(p1_nwl_kwg == p2_nwl_kwg);

  // Test move sort/record key words
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "s1 score r1 all");
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  // CGP command
  string_builder_clear(test_string_builder);
  string_builder_add_formatted_string(test_string_builder, "cgp %s",
                                      DOUG_V_EMELY_CGP);
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  char *cgp = game_get_cgp(config_get_game(config), true);
  assert_strings_equal(cgp, "15/15/15/15/15/15/15/3WINDY7/15/15/15/15/"
                            "15/15/15 ADEEGIL/AEILOUY 0/32 0");
  free(cgp);

  // English and French should be able to play each other
  // with either distribution
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder,
                            "ld english l1 CSW21 l2 FRA20");
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "ld french l1 FRA20 l2 CSW21");
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "lex NWL20");
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KWG, 0),
      "NWL20");
  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KWG, 1),
      "NWL20");

  // Correctly set leave and letter distribution defaults
  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder, "lex FRA20");
  load_config_or_fail(config, string_builder_peek(test_string_builder));
  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KWG, 0),
      "FRA20");
  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KWG, 1),
      "FRA20");
  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KLV, 0),
      "FRA20");
  assert_strings_equal(
      players_data_get_data_name(config_get_players_data(config),
                                 PLAYERS_DATA_TYPE_KLV, 1),
      "FRA20");

  string_builder_clear(test_string_builder);
  string_builder_add_string(test_string_builder,
                            "convert text2kwg csw21.txt csw21.kwg");
  load_config_or_fail(config, string_builder_peek(test_string_builder));

  destroy_string_builder(test_string_builder);
  config_destroy(config);
}

void test_config() {
  test_config_error_cases();
  test_config_success();
}
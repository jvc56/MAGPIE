#include "peg_adjust_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/best_move_carry.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/peg_adjust.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Quackle's table, in test/fixtures/strategy.
#define PEG_ADJUST_TEST_FIXTURES_PATH "./test/fixtures"

enum {
  PEG_ADJUST_TEST_GAMES = 4,
  // Quackle's table has a value for each index up to 12.
  PEG_ADJUST_TEST_QUACKLE_LENGTH = 13,
  // The length of the long tables, which reach the plays that leave up to two
  // racks in the bag.
  PEG_ADJUST_TEST_LONG_LENGTH = (RACK_SIZE * 3) + 1,
  // Lookups and bag classes are checked for the bags below this one, which
  // is past the reach of the longest table.
  PEG_ADJUST_TEST_BAG_LIMIT = PEG_ADJUST_MAX_VALUES_LENGTH + RACK_SIZE,
  PEG_ADJUST_TEST_MOVE_LIST_CAPACITY = 1024,
  // The capacity of the top-n recording checked against all moves.
  PEG_ADJUST_TEST_TOP_N = 10,
  // The margin of the within-x-of-best recording checked, in points.
  PEG_ADJUST_TEST_MARGIN = 15,
  // Every this-many-th move, and every exchange, is also entered as text.
  PEG_ADJUST_TEST_VALIDATE_STRIDE = 5,
  // A bonus that outweighs any difference in score and leave, in points.
  PEG_ADJUST_TEST_BONUS = 500,
  // Inferences run for each configuration, on moves of at least this many
  // tiles, so that few leaves are left to enumerate.
  PEG_ADJUST_TEST_INFERENCES = 6,
  PEG_ADJUST_TEST_INFERENCE_MIN_TILES = 4,
  PEG_ADJUST_TEST_INFERENCE_LEAVES = 10,
  // Sims run at each position of this many games with at most this many
  // tiles in the bag, where most replies to a candidate are within a
  // table's reach: of this many candidates, with this many rollouts each.
  PEG_ADJUST_TEST_SIM_GAMES = 4,
  PEG_ADJUST_TEST_SIM_MAX_BAG = RACK_SIZE + 2,
  PEG_ADJUST_TEST_SIM_CANDIDATES = 6,
  PEG_ADJUST_TEST_SIM_PLAY_ROLLOUTS = 20,
};

// Tables that stress the bounds differently: values of both signs, values
// that only raise equities (which a bound must include), values that only
// lower them (which must not overflow a sentinel), the largest values a file
// may hold, of both signs and all negative. These have the standard length.
// The last two are long (see PEG_ADJUST_TEST_LONG_LENGTH), so that bags above
// the standard length get adjustments too: values of both signs, and the most
// negative values.
typedef enum {
  PEG_ADJUST_PATTERN_ALTERNATING,
  PEG_ADJUST_PATTERN_POSITIVE,
  PEG_ADJUST_PATTERN_NEGATIVE,
  PEG_ADJUST_PATTERN_EXTREME,
  PEG_ADJUST_PATTERN_FLOOR,
  PEG_ADJUST_PATTERN_LONG_ALTERNATING,
  PEG_ADJUST_PATTERN_LONG_FLOOR,
  NUMBER_OF_PEG_ADJUST_PATTERNS,
} peg_adjust_pattern_t;

// Writes contents to <dir>/strategy/<name>.csv.
static void write_peg_adjust_file(const char *dir, const char *name,
                                  const char *contents) {
  char *strategy_dir = get_formatted_string("%s/strategy", dir);
  (void)cfile_mkdir(strategy_dir);
  char *filename = get_formatted_string("%s/%s.csv", strategy_dir, name);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(filename, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  free(filename);
  free(strategy_dir);
}

static void assert_peg_adjust_file_error(const char *dir, const char *name,
                                         const char *contents) {
  write_peg_adjust_file(dir, name, contents);
  ErrorStack *error_stack = error_stack_create();
  const PegAdjust *peg_adjust = peg_adjust_create(dir, name, error_stack);
  assert(peg_adjust == NULL);
  assert(error_stack_top(error_stack) == ERROR_STATUS_PEG_ADJUST_INVALID_ROW);
  error_stack_destroy(error_stack);
}

// The table's value at index, given that its values are the first
// number_of_values of expected, in points.
static Equity get_expected_value(const double *expected, int number_of_values,
                                 int index) {
  return index < number_of_values ? double_to_equity(expected[index]) : 0;
}

// peg_adjust has the first number_of_values values of expected and no more,
// and gives each tile placement the value at its index: the bag less the
// tiles played, plus RACK_SIZE.
static void assert_peg_adjust_values(const PegAdjust *peg_adjust,
                                     const double *expected,
                                     int number_of_values) {
  assert(peg_adjust_get_number_of_values(peg_adjust) == number_of_values);
  for (int index = 0; index < PEG_ADJUST_MAX_VALUES_LENGTH; index++) {
    assert(peg_adjust_get_value(peg_adjust, index) ==
           get_expected_value(expected, number_of_values, index));
  }
  for (int bag = 1; bag < PEG_ADJUST_TEST_BAG_LIMIT; bag++) {
    for (int tiles_played = 1; tiles_played <= RACK_SIZE; tiles_played++) {
      assert(peg_adjust_get_placement_value(peg_adjust, bag, tiles_played) ==
             get_expected_value(expected, number_of_values,
                                bag - tiles_played + RACK_SIZE));
    }
  }
}

// Loads <dir>/strategy/<name>.csv, written with contents, and checks its
// length and values.
static void assert_peg_adjust_file_values(const char *dir, const char *name,
                                          const char *contents,
                                          const double *expected,
                                          int number_of_values) {
  write_peg_adjust_file(dir, name, contents);
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(dir, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_peg_adjust_values(peg_adjust, expected, number_of_values);
  peg_adjust_destroy(peg_adjust);
  error_stack_destroy(error_stack);
}

static void test_peg_adjust_file(void) {
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(PEG_ADJUST_TEST_FIXTURES_PATH,
                                            "quackle_pegadjust", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(peg_adjust_get_name(peg_adjust), "quackle_pegadjust");
  const double quackle[PEG_ADJUST_TEST_QUACKLE_LENGTH] = {
      0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
  assert_peg_adjust_values(peg_adjust, quackle, PEG_ADJUST_TEST_QUACKLE_LENGTH);

  // A placement's index is the bag less the tiles played, plus RACK_SIZE.
  assert(peg_adjust_get_placement_value(peg_adjust, 1, 1) ==
         peg_adjust_get_value(peg_adjust, RACK_SIZE));
  assert(peg_adjust_get_placement_value(peg_adjust, 1, RACK_SIZE) ==
         double_to_equity(-8));
  assert(peg_adjust_get_placement_value(peg_adjust, 5, 1) ==
         peg_adjust_get_value(peg_adjust, RACK_SIZE + 4));
  // Plays that reach an index past the table's last, plays from an empty bag
  // and players without a table get none.
  assert(peg_adjust_get_placement_value(
             peg_adjust, PEG_ADJUST_TEST_QUACKLE_LENGTH - RACK_SIZE + 1, 1) ==
         0);
  assert(peg_adjust_get_placement_value(peg_adjust, 0, 1) == 0);
  assert(peg_adjust_get_placement_value(NULL, 1, 1) == 0);
  Equity by_tiles_played[RACK_SIZE + 1];
  peg_adjust_fill_by_tiles_played(peg_adjust, 3, by_tiles_played);
  assert(by_tiles_played[0] == 0);
  for (int tiles_played = 1; tiles_played <= RACK_SIZE; tiles_played++) {
    assert(by_tiles_played[tiles_played] ==
           peg_adjust_get_value(peg_adjust, 3 - tiles_played + RACK_SIZE));
  }
  peg_adjust_destroy(peg_adjust);
  error_stack_destroy(error_stack);

  char *dir = test_platform_make_temporary_directory("peg_adjust_test");
  // Blank lines and spaces are allowed, and the table ends at the highest
  // index given.
  const double partial[] = {0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1};
  assert_peg_adjust_file_values(dir, "partial",
                                "# index,value\n"
                                "1,-8\n2,0\n3,-0.5\n4,-2\n5,-3.5\n6,-2\n"
                                "\n   \n"
                                "7,2\n8,10\n  9 , 7 \n10,4\n11,-1\n",
                                partial, 12);
  // Windows line endings, signs, bare decimal points and the largest values.
  // An index left out below the highest is 0.
  const double edges[] = {0, 0, 3, 0.5, 0, 0, 0, 0, 1000, -1000};
  assert_peg_adjust_file_values(
      dir, "edges", "# crlf\r\n+2,3\r\n3,.5\r\n8,1000\r\n9,-1000.0\r\n", edges,
      10);
  const double empty[1] = {0};
  assert_peg_adjust_file_values(dir, "empty", "", empty, 0);
  assert_peg_adjust_file_values(dir, "comments", "# nothing\n#1,5\n", empty, 0);
  // A table is as long as its file makes it. The rows may come in any order,
  // and a last row of 0 counts.
  double long_table[PEG_ADJUST_TEST_LONG_LENGTH] = {0};
  long_table[PEG_ADJUST_STANDARD_VALUES_LENGTH] = -4;
  long_table[PEG_ADJUST_TEST_LONG_LENGTH - 2] = 3.5;
  char *long_contents = get_formatted_string(
      "%d,0\n%d,3.5\n%d,-4\n", PEG_ADJUST_TEST_LONG_LENGTH - 1,
      PEG_ADJUST_TEST_LONG_LENGTH - 2, PEG_ADJUST_STANDARD_VALUES_LENGTH);
  assert_peg_adjust_file_values(dir, "long", long_contents, long_table,
                                PEG_ADJUST_TEST_LONG_LENGTH);
  free(long_contents);
  // The largest index a file may give, and the one past it.
  double largest[PEG_ADJUST_MAX_VALUES_LENGTH] = {0};
  largest[PEG_ADJUST_MAX_VALUES_LENGTH - 1] = 2;
  char *largest_contents =
      get_formatted_string("%d,2\n", PEG_ADJUST_MAX_VALUES_LENGTH - 1);
  assert_peg_adjust_file_values(dir, "largest_index", largest_contents, largest,
                                PEG_ADJUST_MAX_VALUES_LENGTH);
  free(largest_contents);
  char *too_large_contents =
      get_formatted_string("%d,2\n", PEG_ADJUST_MAX_VALUES_LENGTH);
  assert_peg_adjust_file_error(dir, "large_index", too_large_contents);
  free(too_large_contents);

  assert_peg_adjust_file_error(dir, "three_fields", "1,2,3\n");
  assert_peg_adjust_file_error(dir, "trailing_comma", "1,2,\n");
  assert_peg_adjust_file_error(dir, "no_value", "1\n");
  assert_peg_adjust_file_error(dir, "blank_index", ",5\n");
  assert_peg_adjust_file_error(dir, "blank_value", "1,\n");
  assert_peg_adjust_file_error(dir, "space_value", "1, \n");
  assert_peg_adjust_file_error(dir, "bad_index", "x,2\n");
  assert_peg_adjust_file_error(dir, "fractional_index", "1.5,2\n");
  assert_peg_adjust_file_error(dir, "bad_value", "1,x\n");
  assert_peg_adjust_file_error(dir, "value_with_suffix", "1,2x\n");
  assert_peg_adjust_file_error(dir, "nan", "1,nan\n");
  assert_peg_adjust_file_error(dir, "inf", "1,inf\n");
  assert_peg_adjust_file_error(dir, "negative_inf", "1,-inf\n");
  assert_peg_adjust_file_error(dir, "huge", "1,1e9\n");
  assert_peg_adjust_file_error(dir, "too_large", "1,1000.5\n");
  assert_peg_adjust_file_error(dir, "too_small", "1,-1000.5\n");
  assert_peg_adjust_file_error(dir, "negative_index", "-1,2\n");
  assert_peg_adjust_file_error(dir, "overflowing_index",
                               "99999999999999999999,2\n");
  assert_peg_adjust_file_error(dir, "twice", "1,2\n1,3\n");
  assert_peg_adjust_file_error(dir, "twice_spelled_differently",
                               "8,2\n+08,3\n");
  assert_peg_adjust_file_error(dir, "byte_order_mark",
                               "\xEF\xBB\xBF"
                               "1,2\n");
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// Each bag below number_of_values has a class of its own and the rest share
// one, and bags of one class give every tile placement the same adjustment.
static void assert_peg_adjust_bag_classes(const PegAdjust *peg_adjust,
                                          int number_of_values) {
  for (int bag = 0; bag < PEG_ADJUST_TEST_BAG_LIMIT; bag++) {
    assert(peg_adjust_get_bag_class(peg_adjust, bag) ==
           (bag < number_of_values ? bag : number_of_values));
    for (int other_bag = 0; other_bag < bag; other_bag++) {
      if (peg_adjust_get_bag_class(peg_adjust, other_bag) !=
          peg_adjust_get_bag_class(peg_adjust, bag)) {
        continue;
      }
      for (int tiles_played = 1; tiles_played <= RACK_SIZE; tiles_played++) {
        assert(peg_adjust_get_placement_value(peg_adjust, bag, tiles_played) ==
               peg_adjust_get_placement_value(peg_adjust, other_bag,
                                              tiles_played));
      }
    }
  }
}

static void test_peg_adjust_bag_class(void) {
  // Without a table no play gets an adjustment: the bags that hold tiles
  // share a class, which the empty bag stays out of.
  assert_peg_adjust_bag_classes(NULL, 1);
  Equity values[PEG_ADJUST_MAX_VALUES_LENGTH];
  for (int index = 0; index < PEG_ADJUST_MAX_VALUES_LENGTH; index++) {
    values[index] = int_to_equity(index + 1);
  }
  const int lengths[] = {0,
                         1,
                         RACK_SIZE,
                         PEG_ADJUST_STANDARD_VALUES_LENGTH,
                         PEG_ADJUST_STANDARD_VALUES_LENGTH + 1,
                         PEG_ADJUST_TEST_LONG_LENGTH,
                         PEG_ADJUST_MAX_VALUES_LENGTH};
  for (size_t length_idx = 0; length_idx < sizeof(lengths) / sizeof(lengths[0]);
       length_idx++) {
    const int number_of_values = lengths[length_idx];
    PegAdjust *peg_adjust =
        peg_adjust_create_from_values("classes", values, number_of_values);
    // The table's own length decides, shorter than the standard or not. A
    // table with no value classes the bags as no table does.
    assert_peg_adjust_bag_classes(peg_adjust,
                                  number_of_values > 1 ? number_of_values : 1);
    peg_adjust_destroy(peg_adjust);
  }
}

static void assert_peg_adjust_names(const PlayersData *players_data,
                                    const char *first, const char *second) {
  const char *expected[2] = {first, second};
  for (int player_index = 0; player_index < 2; player_index++) {
    const PegAdjust *peg_adjust =
        players_data_get_peg_adjust(players_data, player_index);
    if (expected[player_index] == NULL) {
      assert(peg_adjust == NULL);
    } else {
      assert(peg_adjust != NULL);
      assert_strings_equal(peg_adjust_get_name(peg_adjust),
                           expected[player_index]);
    }
  }
}

static void test_peg_adjust_config(void) {
  char *dir = test_platform_make_temporary_directory("peg_adjust_config");
  write_peg_adjust_file(dir, "other", "8,5\n");
  write_peg_adjust_file(dir, "broken", "8,nan\n");
  // Loaded per player through the config, and unloaded with "none".
  char *set_cmd = get_formatted_string(
      "set -lex CSW21 -path %s:%s:%s -pegadjust1 quackle_pegadjust", dir,
      PEG_ADJUST_TEST_FIXTURES_PATH, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  const PlayersData *players_data = config_get_players_data(config);
  assert_peg_adjust_names(players_data, "quackle_pegadjust", NULL);
  load_and_exec_config_or_die(config, "set -pegadjust quackle_pegadjust");
  // Both players share one table.
  assert(players_data_get_peg_adjust(players_data, 0) ==
         players_data_get_peg_adjust(players_data, 1));
  assert_peg_adjust_names(players_data, "quackle_pegadjust",
                          "quackle_pegadjust");
  load_and_exec_config_or_die(config, "set -pegadjust2 NONE");
  assert_peg_adjust_names(players_data, "quackle_pegadjust", NULL);
  // A per-player option overrides the option for both.
  load_and_exec_config_or_die(config, "set -pegadjust other -pegadjust1 none");
  assert_peg_adjust_names(players_data, NULL, "other");
  // A table that is missing or does not load is an error.
  assert_config_exec_status(config, "set -pegadjust1 broken",
                            ERROR_STATUS_PEG_ADJUST_INVALID_ROW);
  load_and_exec_config_or_die(config, "set -pegadjust1 quackle_pegadjust");
  assert_config_exec_status(config, "set -pegadjust2 missing",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  load_and_exec_config_or_die(config, "set -pegadjust2 other");
  assert_peg_adjust_names(players_data, "quackle_pegadjust", "other");

  // The settings name the tables, and loading them back restores them.
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  Config *restored = config_create_or_die(string_builder_peek(settings));
  assert_peg_adjust_names(config_get_players_data(restored),
                          "quackle_pegadjust", "other");
  config_destroy(restored);
  string_builder_destroy(settings);

  config_destroy(config);
  free(set_cmd);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// Runs command on config, which must succeed, and writes what it prints to
// output (size bytes, terminated).
static void exec_capturing_output(Config *config, const char *command,
                                  char *output, size_t size) {
  FILE *captured = tmpfile();
  assert(captured);
  io_set_stream_out(captured);
  load_and_exec_config_or_die(config, command);
  io_reset_stream_out();
  const int seek_result = fseek(captured, 0, SEEK_SET);
  assert(seek_result == 0);
  const size_t length = fread(output, 1, size - 1, captured);
  output[length] = '\0';
  (void)fclose(captured);
}

// The live game's players play with the tables the players' data holds.
static void assert_game_peg_adjust(const Config *config) {
  const Game *game = config_get_game(config);
  assert(game);
  const PlayersData *players_data = config_get_players_data(config);
  for (int player_index = 0; player_index < 2; player_index++) {
    assert(player_get_peg_adjust(game_get_player(game, player_index)) ==
           players_data_get_peg_adjust(players_data, player_index));
  }
}

static void assert_settings_have(const Config *config, const char *first,
                                 const char *second) {
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  assert(strstr(string_builder_peek(settings), first) != NULL);
  assert(strstr(string_builder_peek(settings), second) != NULL);
  string_builder_destroy(settings);
}

// A player set to auto plays with its lexicon's own table, and changes
// tables with its lexicon, however the lexicon is loaded.
static void test_peg_adjust_auto(void) {
  char *dir = test_platform_make_temporary_directory("peg_adjust_auto");
  // Ahead in the path of the tables the data has for these lexica.
  write_peg_adjust_file(dir, "CSW21_pegadjust", "8,5\n");
  write_peg_adjust_file(dir, "NWL20_pegadjust", "8,7\n");
  write_peg_adjust_file(dir, "other", "8,3\n");
  write_peg_adjust_file(dir, "broken", "8,nan\n");
  char *set_cmd = get_formatted_string("set -lex CSW21 -wmp false -path %s:%s",
                                       dir, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  const PlayersData *players_data = config_get_players_data(config);
  // Tests state what they use: no table until one is asked for.
  assert_peg_adjust_names(players_data, NULL, NULL);
  assert_settings_have(config, " -pegadjust1 none", " -pegadjust2 none");
  load_and_exec_config_or_die(config, "set -pegadjust auto");
  assert_peg_adjust_names(players_data, "CSW21_pegadjust", "CSW21_pegadjust");
  assert(players_data_get_peg_adjust(players_data, 0) ==
         players_data_get_peg_adjust(players_data, 1));
  // A command that changes nothing loads nothing.
  const PegAdjust *loaded = players_data_get_peg_adjust(players_data, 0);
  load_and_exec_config_or_die(config, "set -threads 1");
  assert(players_data_get_peg_adjust(players_data, 0) == loaded);
  char output[4096];
  exec_capturing_output(config, "new", output, sizeof(output));
  assert_game_peg_adjust(config);

  // The table follows the lexicon, per player, and the live game follows
  // the table.
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_peg_adjust_names(players_data, "NWL20_pegadjust", "NWL20_pegadjust");
  assert_game_peg_adjust(config);
  load_and_exec_config_or_die(config, "set -l1 CSW21");
  assert_peg_adjust_names(players_data, "CSW21_pegadjust", "NWL20_pegadjust");
  assert_game_peg_adjust(config);

  // A name or none takes the player it is given to off auto, also when it
  // comes with a new lexicon.
  load_and_exec_config_or_die(config, "set -pegadjust2 other");
  assert_peg_adjust_names(players_data, "CSW21_pegadjust", "other");
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_peg_adjust_names(players_data, "NWL20_pegadjust", "other");
  load_and_exec_config_or_die(config, "set -lex CSW21 -pegadjust1 none");
  assert_peg_adjust_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_peg_adjust_names(players_data, NULL, "other");
  load_and_exec_config_or_die(config, "set -pegadjust1 AUTO");
  assert_peg_adjust_names(players_data, "NWL20_pegadjust", "other");

  // A named table that does not load is an error, which leaves every player
  // as it was: the first is still on auto.
  assert_config_exec_status(config, "set -pegadjust2 broken",
                            ERROR_STATUS_PEG_ADJUST_INVALID_ROW);
  assert_config_exec_status(config, "set -pegadjust missing",
                            ERROR_STATUS_FILEPATH_FILE_NOT_FOUND);
  assert_peg_adjust_names(players_data, "NWL20_pegadjust", "other");
  load_and_exec_config_or_die(config, "set -lex CSW21");
  assert_peg_adjust_names(players_data, "CSW21_pegadjust", "other");
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_game_peg_adjust(config);

  // The settings say auto rather than the table auto found, and loading
  // them back finds it again.
  assert_settings_have(config, " -pegadjust1 auto", " -pegadjust2 other");
  StringBuilder *settings = string_builder_create();
  config_add_settings_to_string_builder(config, settings);
  Config *restored = config_create_or_die(string_builder_peek(settings));
  string_builder_destroy(settings);
  assert_peg_adjust_names(config_get_players_data(restored), "NWL20_pegadjust",
                          "other");
  load_and_exec_config_or_die(restored, "set -lex CSW21 -pegadjust2 none");
  assert_peg_adjust_names(config_get_players_data(restored), "CSW21_pegadjust",
                          NULL);
  assert_settings_have(restored, " -pegadjust1 auto", " -pegadjust2 none");
  config_destroy(restored);

  // A game file that fails to load puts the lexicon back, and the table
  // with it; one that loads brings its lexicon's table.
  assert_config_exec_status(config,
                            "load testdata/gcgs/invalid_tile_placement.gcg",
                            ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  assert_peg_adjust_names(players_data, "NWL20_pegadjust", "other");
  assert_game_peg_adjust(config);
  exec_capturing_output(config, "load testdata/gcgs/success_standard.gcg",
                        output, sizeof(output));
  assert_peg_adjust_names(players_data, "CSW21_pegadjust", "other");
  assert_game_peg_adjust(config);

  config_destroy(config);
  free(set_cmd);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// How many times text occurs in output.
static int count_occurrences(const char *output, const char *text) {
  int count = 0;
  const char *next = output;
  while ((next = strstr(next, text)) != NULL) {
    count++;
    next += strlen(text);
  }
  return count;
}

// Every player is on auto where the tables are: in the command line program
// and the library. A lexicon without a table is played without one and
// nothing is said; a table that does not load is said once and left out; and
// the commands that train static values play without the table.
static void test_peg_adjust_auto_default(void) {
  char *dir = test_platform_make_temporary_directory("peg_adjust_default");
  // The trainers write here.
  char *lexica_dir = get_formatted_string("%s/lexica", dir);
  assert(cfile_mkdir(lexica_dir) == 0);
  free(lexica_dir);
  char *strategy_dir = get_formatted_string("%s/strategy", dir);
  assert(cfile_mkdir(strategy_dir) == 0);
  free(strategy_dir);
  char *data_paths = get_formatted_string("%s:%s", dir, DEFAULT_TEST_DATA_PATH);
  const ConfigArgs args = {.data_paths = data_paths,
                           .settings_filename = DEFAULT_TEST_SETTINGS_FILENAME,
                           .use_wmp = true,
                           .auto_tables = true};
  ErrorStack *error_stack = error_stack_create();
  Config *config = config_create(&args, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  const PlayersData *players_data = config_get_players_data(config);
  const char *table = "CSW21_ab_pegadjust";
  const char *said = "pre-endgame adjustments";
  write_peg_adjust_file(dir, "other", "8,3\n");
  char output[4096];
  // Only this test's directory has a table for this lexicon, and not yet.
  exec_capturing_output(config,
                        "set -lex CSW21_ab -ld english_ab -wmp false -wit "
                        "false -rit false -s1 equity -s2 equity -r1 best -r2 "
                        "best -numplays 1 -threads 1 -savesettings false",
                        output, sizeof(output));
  assert_peg_adjust_names(players_data, NULL, NULL);
  assert(strstr(output, said) == NULL);
  assert_settings_have(config, " -pegadjust1 auto", " -pegadjust2 auto");
  // The table is found once the data has it.
  write_peg_adjust_file(dir, table, "8,5\n");
  exec_capturing_output(config, "set -threads 1", output, sizeof(output));
  assert_peg_adjust_names(players_data, table, table);
  assert(strstr(output, said) == NULL);

  // A table that does not load is not an error: it is said once, for both
  // players, and passed over until its file changes.
  load_and_exec_config_or_die(config, "set -pegadjust none");
  assert_peg_adjust_names(players_data, NULL, NULL);
  write_peg_adjust_file(dir, table, "8,nan\n");
  exec_capturing_output(config, "set -pegadjust auto", output, sizeof(output));
  assert_peg_adjust_names(players_data, NULL, NULL);
  assert(
      count_occurrences(
          output,
          "playing without the pre-endgame adjustments CSW21_ab_pegadjust:") ==
      1);
  exec_capturing_output(config, "set -threads 1", output, sizeof(output));
  assert_peg_adjust_names(players_data, NULL, NULL);
  assert(strstr(output, said) == NULL);
  write_peg_adjust_file(dir, table, "8,5\n9,2\n");
  exec_capturing_output(config, "set -threads 1", output, sizeof(output));
  assert_peg_adjust_names(players_data, table, table);
  assert(strstr(output, said) == NULL);

  // The trainers' loads go without the table, and the settings stand: the
  // next command has it back.
  const char *trainers[3] = {
      "leavegen 1 0 -seed 3",
      "census 2x72,80 peg_adjust_census null -seed 3",
      "patgen 4,4 peg_adjust_patgen -seed 3",
  };
  for (int trainer_idx = 0; trainer_idx < 3; trainer_idx++) {
    exec_capturing_output(config, trainers[trainer_idx], output,
                          sizeof(output));
    assert_peg_adjust_names(players_data, NULL, NULL);
    assert_settings_have(config, " -pegadjust1 auto", " -pegadjust2 auto");
    exec_capturing_output(config, "set -threads 1", output, sizeof(output));
    assert_peg_adjust_names(players_data, table, table);
  }
  // A table a trainer is told to play with, it plays with.
  exec_capturing_output(config, "leavegen 1 0 -seed 3 -pegadjust1 other",
                        output, sizeof(output));
  assert_peg_adjust_names(players_data, "other", NULL);
  exec_capturing_output(config, "set -threads 1", output, sizeof(output));
  assert_peg_adjust_names(players_data, "other", table);

  config_destroy(config);
  free(data_paths);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

static MoveList *generate_moves_recorded(const Game *game, int capacity,
                                         move_record_t move_record_type,
                                         Equity eq_margin_movegen) {
  MoveList *move_list = move_list_create(capacity);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = move_record_type,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = eq_margin_movegen,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  return move_list;
}

// Every move, in a list grown until it holds them all.
static MoveList *generate_all_moves(const Game *game) {
  int capacity = PEG_ADJUST_TEST_MOVE_LIST_CAPACITY;
  while (true) {
    MoveList *move_list =
        generate_moves_recorded(game, capacity, MOVE_RECORD_ALL, 0);
    if (move_list_get_count(move_list) < capacity) {
      return move_list;
    }
    move_list_destroy(move_list);
    capacity *= 2;
  }
}

static Equity generate_best_move_equity(const Game *game) {
  MoveList *move_list = generate_moves_recorded(game, 1, MOVE_RECORD_BEST, 0);
  const Equity equity = move_get_equity(move_list_get_move(move_list, 0));
  move_list_destroy(move_list);
  return equity;
}

static int compare_equities_descending(const void *first, const void *second) {
  const Equity first_equity = *(const Equity *)first;
  const Equity second_equity = *(const Equity *)second;
  if (first_equity != second_equity) {
    return first_equity > second_equity ? -1 : 1;
  }
  return 0;
}

// The equities of a move list, highest first. The caller frees them.
static Equity *get_sorted_equities(const MoveList *move_list) {
  const int count = move_list_get_count(move_list);
  Equity *equities = malloc_or_die(sizeof(Equity) * ((size_t)count + 1));
  for (int move_idx = 0; move_idx < count; move_idx++) {
    equities[move_idx] =
        move_get_equity(move_list_get_move(move_list, move_idx));
  }
  qsort(equities, (size_t)count, sizeof(Equity), compare_equities_descending);
  return equities;
}

static void set_peg_adjust(PlayersData *players_data, const Game *game,
                           int player_index, PegAdjust *peg_adjust) {
  players_data_set_data(players_data, PLAYERS_DATA_TYPE_PEG_ADJUST,
                        player_index, peg_adjust);
  player_update(players_data, game_get_player(game, player_index));
}

// The equity the mover's validation of move's text gives it.
static Equity get_validated_equity(const Game *game, const Move *move) {
  StringBuilder *move_string = string_builder_create();
  string_builder_add_ucgi_move(move_string, move, game_get_board(game),
                               game_get_ld(game));
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *validated_moves = validated_moves_create(
      game, game_get_player_on_turn_index(game),
      string_builder_peek(move_string), true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(validated_moves) == 1);
  const Equity equity =
      move_get_equity(validated_moves_get_move(validated_moves, 0));
  validated_moves_destroy(validated_moves);
  error_stack_destroy(error_stack);
  string_builder_destroy(move_string);
  return equity;
}

static Equity get_expected_adjustment(const PegAdjust *peg_adjust, int bag,
                                      const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  return peg_adjust_get_placement_value(peg_adjust, bag,
                                        move_get_tiles_played(move));
}

// With the mover's table (and a different table for the opponent, which
// must not count):
// - every tile placement's equity is its equity without a table plus the
//   table's value for its bag and tiles, and every other move's is unchanged;
// - the best move recorded is the best of all moves, which checks that the
//   shadow and WMP bounds still bound every move;
// - a top-n recording holds the n best equities of all moves;
// - a within-x-of-best recording holds every move within x of the best;
// - entering a move as text adds the same adjustment.
static void assert_peg_adjust_equities(const Game *game,
                                       PlayersData *players_data,
                                       PegAdjust *peg_adjust,
                                       PegAdjust *decoy) {
  const int player_index = game_get_player_on_turn_index(game);
  const int bag = bag_get_letters(game_get_bag(game));

  set_peg_adjust(players_data, game, player_index, NULL);
  set_peg_adjust(players_data, game, 1 - player_index, decoy);
  MoveList *without = generate_all_moves(game);

  set_peg_adjust(players_data, game, player_index, peg_adjust);
  MoveList *with = generate_all_moves(game);
  const Equity best_equity = generate_best_move_equity(game);

  const int count = move_list_get_count(without);
  assert(count == move_list_get_count(with));
  for (int with_idx = 0; with_idx < count; with_idx++) {
    const Move *with_move = move_list_get_move(with, with_idx);
    const Move *without_move = NULL;
    for (int without_idx = 0; without_idx < count; without_idx++) {
      const Move *candidate = move_list_get_move(without, without_idx);
      if (compare_moves_without_equity(with_move, candidate, true) == -1) {
        without_move = candidate;
        break;
      }
    }
    assert(without_move != NULL);
    assert(move_get_equity(with_move) ==
           move_get_equity(without_move) +
               get_expected_adjustment(peg_adjust, bag, with_move));
  }

  Equity *all_equities = get_sorted_equities(with);
  assert(best_equity == all_equities[0]);

  MoveList *top =
      generate_moves_recorded(game, PEG_ADJUST_TEST_TOP_N, MOVE_RECORD_ALL, 0);
  const int top_count = move_list_get_count(top);
  assert(top_count ==
         (count < PEG_ADJUST_TEST_TOP_N ? count : PEG_ADJUST_TEST_TOP_N));
  Equity *top_equities = get_sorted_equities(top);
  for (int top_idx = 0; top_idx < top_count; top_idx++) {
    assert(top_equities[top_idx] == all_equities[top_idx]);
  }

  const Equity margin = int_to_equity(PEG_ADJUST_TEST_MARGIN);
  MoveList *within =
      generate_moves_recorded(game, PEG_ADJUST_TEST_MOVE_LIST_CAPACITY,
                              MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST, margin);
  int expected_within = 0;
  while (expected_within < count &&
         all_equities[expected_within] >= all_equities[0] - margin) {
    expected_within++;
  }
  assert(move_list_get_count(within) == expected_within);
  Equity *within_equities = get_sorted_equities(within);
  for (int within_idx = 0; within_idx < expected_within; within_idx++) {
    assert(within_equities[within_idx] == all_equities[within_idx]);
  }

  // Entered as text, by the mover with and then without the table.
  int number_to_validate = 0;
  const Move **to_validate = malloc_or_die(sizeof(Move *) * (size_t)count);
  for (int with_idx = 0; with_idx < count; with_idx++) {
    const Move *move = move_list_get_move(with, with_idx);
    const game_event_t type = move_get_type(move);
    if (type == GAME_EVENT_EXCHANGE ||
        (type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
         with_idx % PEG_ADJUST_TEST_VALIDATE_STRIDE == 0)) {
      to_validate[number_to_validate++] = move;
    }
  }
  Equity *validated_with =
      malloc_or_die(sizeof(Equity) * ((size_t)number_to_validate + 1));
  for (int move_idx = 0; move_idx < number_to_validate; move_idx++) {
    validated_with[move_idx] =
        get_validated_equity(game, to_validate[move_idx]);
  }
  set_peg_adjust(players_data, game, player_index, NULL);
  for (int move_idx = 0; move_idx < number_to_validate; move_idx++) {
    const Move *move = to_validate[move_idx];
    assert(validated_with[move_idx] ==
           get_validated_equity(game, move) +
               get_expected_adjustment(peg_adjust, bag, move));
  }

  set_peg_adjust(players_data, game, 1 - player_index, NULL);
  free(validated_with);
  free(to_validate);
  free(within_equities);
  move_list_destroy(within);
  free(top_equities);
  move_list_destroy(top);
  free(all_equities);
  move_list_destroy(without);
  move_list_destroy(with);
}

static PegAdjust *create_pattern_table(peg_adjust_pattern_t pattern, int sign) {
  const bool is_long = pattern == PEG_ADJUST_PATTERN_LONG_ALTERNATING ||
                       pattern == PEG_ADJUST_PATTERN_LONG_FLOOR;
  const int number_of_values =
      is_long ? PEG_ADJUST_TEST_LONG_LENGTH : PEG_ADJUST_STANDARD_VALUES_LENGTH;
  Equity values[PEG_ADJUST_TEST_LONG_LENGTH] = {0};
  for (int index = 0; index < number_of_values; index++) {
    const int alternating = index % 2 == 0 ? 1 : -1;
    double points = 0;
    switch (pattern) {
    case PEG_ADJUST_PATTERN_ALTERNATING:
    case PEG_ADJUST_PATTERN_LONG_ALTERNATING:
      points = alternating * 15 * index;
      break;
    case PEG_ADJUST_PATTERN_POSITIVE:
      points = 7 + 11 * index;
      break;
    case PEG_ADJUST_PATTERN_NEGATIVE:
      points = -7 - 11 * index;
      break;
    case PEG_ADJUST_PATTERN_EXTREME:
      points = alternating * PEG_ADJUST_MAX_ABS_VALUE;
      break;
    case PEG_ADJUST_PATTERN_FLOOR:
    case PEG_ADJUST_PATTERN_LONG_FLOOR:
      points = -PEG_ADJUST_MAX_ABS_VALUE;
      break;
    case NUMBER_OF_PEG_ADJUST_PATTERNS:
      log_fatal("invalid pattern");
      break;
    }
    values[index] = double_to_equity(sign * points);
  }
  return peg_adjust_create_from_values("pattern", values, number_of_values);
}

static void test_peg_adjust_movegen(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  Game *game = config_game_create(config);
  for (int pattern = 0; pattern < NUMBER_OF_PEG_ADJUST_PATTERNS; pattern++) {
    PegAdjust *peg_adjust = create_pattern_table(pattern, 1);
    PegAdjust *decoy = create_pattern_table(pattern, -1);
    int positions = 0;
    for (int game_idx = 0; game_idx < PEG_ADJUST_TEST_GAMES; game_idx++) {
      game_reset(game);
      game_seed(game, (uint64_t)game_idx + 1 + 100 * (uint64_t)pattern);
      draw_starting_racks(game);
      while (!game_over(game)) {
        const int bag = bag_get_letters(game_get_bag(game));
        // Only positions with fewer tiles in the bag than the table has
        // values can reach it.
        if (bag > 0 && bag < peg_adjust_get_number_of_values(peg_adjust)) {
          assert_peg_adjust_equities(game, players_data, peg_adjust, decoy);
          positions++;
        }
        play_top_n_equity_move(game, 0);
      }
    }
    assert(positions > 0);
    peg_adjust_destroy(decoy);
    peg_adjust_destroy(peg_adjust);
  }
  game_destroy(game);
  config_destroy(config);
}

// Whether a tile placement of tiles_played tiles from a bag of bag tiles
// gets the table's value at an index from first_index to last_index.
static bool reaches_indices(int bag, int tiles_played, int first_index,
                            int last_index) {
  const int index = bag - tiles_played + RACK_SIZE;
  return index >= first_index && index <= last_index;
}

// A large bonus for the placements that reach some indices makes the best
// move one of them whenever one exists, which checks the sign and the index
// of the table end to end.
static void assert_bonus_decides(const Game *game, PlayersData *players_data,
                                 int first_index, int last_index) {
  const int player_index = game_get_player_on_turn_index(game);
  const int bag = bag_get_letters(game_get_bag(game));
  Equity values[PEG_ADJUST_TEST_LONG_LENGTH] = {0};
  for (int index = first_index; index <= last_index; index++) {
    values[index] = int_to_equity(PEG_ADJUST_TEST_BONUS);
  }
  PegAdjust *bonus =
      peg_adjust_create_from_values("bonus", values, last_index + 1);
  set_peg_adjust(players_data, game, player_index, bonus);
  MoveList *all_moves = generate_all_moves(game);
  bool exists = false;
  for (int move_idx = 0; move_idx < move_list_get_count(all_moves);
       move_idx++) {
    const Move *move = move_list_get_move(all_moves, move_idx);
    exists = exists || (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                        reaches_indices(bag, move_get_tiles_played(move),
                                        first_index, last_index));
  }
  MoveList *best_list = generate_moves_recorded(game, 1, MOVE_RECORD_BEST, 0);
  const Move *best = move_list_get_move(best_list, 0);
  assert(!exists || (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                     reaches_indices(bag, move_get_tiles_played(best),
                                     first_index, last_index)));
  move_list_destroy(best_list);
  move_list_destroy(all_moves);
  set_peg_adjust(players_data, game, player_index, NULL);
  peg_adjust_destroy(bonus);
}

static void test_peg_adjust_bonus(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  Game *game = config_game_create(config);
  for (int game_idx = 0; game_idx < PEG_ADJUST_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 1000);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (bag > 0 && bag < PEG_ADJUST_STANDARD_VALUES_LENGTH) {
        // Leaving 1 to RACK_SIZE - 2 tiles in the bag, then emptying it.
        assert_bonus_decides(game, players_data, RACK_SIZE + 1,
                             PEG_ADJUST_STANDARD_VALUES_LENGTH - 1);
        assert_bonus_decides(game, players_data, 1, RACK_SIZE);
      }
      if (bag > 0 && bag < PEG_ADJUST_TEST_LONG_LENGTH) {
        // Leaving more tiles in the bag, which only a long table reaches.
        assert_bonus_decides(game, players_data,
                             PEG_ADJUST_STANDARD_VALUES_LENGTH,
                             PEG_ADJUST_TEST_LONG_LENGTH - 1);
      }
      play_top_n_equity_move(game, 0);
    }
  }
  game_destroy(game);
  config_destroy(config);
}

// The best move with the carry, which starts from the moves and lane bounds
// earlier generations remembered, is the best move without it.
static void assert_carry_matches(Game *game, MoveList *plain_list,
                                 MoveList *carried_list, BestMoveCarry *carry) {
  const Move *plain = get_top_equity_move(game, plain_list);
  const Move *carried =
      get_top_equity_move_with_carry(game, carried_list, carry);
  assert(compare_moves_without_equity(plain, carried, true) == -1);
  assert(move_get_equity(plain) == move_get_equity(carried));
}

// With a table a move's equity depends on the bag, so what the carry
// remembers at one bag must not decide a generation at another. Simulation
// uses the carry for its rollouts: each candidate is played and the next two
// plies are compared. The same board and rack are also generated at the bag
// as it is and with the opponent's rack returned to it. The table is both
// players', or only the first player's: the bags that share what is
// remembered are the mover's table's to say. Or it is neither's, and only
// says which bags are checked: without a table every bag that holds tiles
// shares what is remembered.
static void test_peg_adjust_carry_pattern(const char *set_cmd,
                                          peg_adjust_pattern_t pattern,
                                          int players_with_table) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  PegAdjust *peg_adjust = create_pattern_table(pattern, 1);
  Game *game = config_game_create(config);
  for (int player_index = 0; player_index < players_with_table;
       player_index++) {
    set_peg_adjust(players_data, game, player_index, peg_adjust);
  }
  MoveList *candidates = move_list_create(PEG_ADJUST_TEST_TOP_N);
  MoveList *plain_list = move_list_create(1);
  MoveList *carried_list = move_list_create(1);
  BestMoveCarry *carry = best_move_carry_create(1024, true);
  int generations = 0;
  for (int game_idx = 0; game_idx < PEG_ADJUST_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 2000 + 100 * (uint64_t)pattern);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (bag > 0 && bag < peg_adjust_get_number_of_values(peg_adjust)) {
        best_move_carry_reset(carry);
        const int opponent_index = 1 - game_get_player_on_turn_index(game);
        for (int repeat = 0; repeat < 2; repeat++) {
          assert_carry_matches(game, plain_list, carried_list, carry);
          return_rack_to_bag(game, opponent_index);
          assert_carry_matches(game, plain_list, carried_list, carry);
          draw_to_full_rack(game, opponent_index);
          generations += 2;
        }
        const MoveGenArgs candidate_args = {
            .game = game,
            .move_list = candidates,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
        };
        generate_moves(&candidate_args);
        for (int candidate_idx = 0;
             candidate_idx < move_list_get_count(candidates); candidate_idx++) {
          Game *playout = game_duplicate(game);
          play_move(move_list_get_move(candidates, candidate_idx), playout,
                    NULL);
          for (int ply = 0; ply < 2 && !game_over(playout) &&
                            bag_get_letters(game_get_bag(playout)) > 0;
               ply++) {
            assert_carry_matches(playout, plain_list, carried_list, carry);
            play_move(get_top_equity_move(playout, plain_list), playout, NULL);
            generations++;
          }
          game_destroy(playout);
        }
      }
      play_move(get_top_equity_move(game, plain_list), game, NULL);
    }
  }
  assert(generations > 100);
  best_move_carry_destroy(carry);
  move_list_destroy(carried_list);
  move_list_destroy(plain_list);
  move_list_destroy(candidates);
  for (int player_index = 0; player_index < 2; player_index++) {
    set_peg_adjust(players_data, game, player_index, NULL);
  }
  game_destroy(game);
  peg_adjust_destroy(peg_adjust);
  config_destroy(config);
}

// The tiles a tile placement takes from the rack, a blank as the blank.
static void set_played_tiles(const Move *move, Rack *played_tiles) {
  rack_reset(played_tiles);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rack_add_letter(played_tiles,
                    get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
  }
}

// The number and mean value of the leaves inference finds consistent with
// the player on turn making move.
static void get_inferred_leaves(const Config *config, const Game *game,
                                const Move *move, uint64_t *samples,
                                double *mean) {
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack played_tiles;
  rack_set_dist_size_and_reset(&played_tiles, ld_size);
  set_played_tiles(move, &played_tiles);
  Rack known_rack;
  rack_set_dist_size_and_reset(&known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  InferenceArgs args;
  infer_args_fill(&args, PEG_ADJUST_TEST_INFERENCE_LEAVES, 0, NULL, game, 1, 0,
                  0, thread_control, false, true,
                  game_get_player_on_turn_index(game), move_get_score(move), 0,
                  &played_tiles, &known_rack, &nontarget_known_rack);
  InferenceResults *results = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();
  infer_without_ctx(&args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const Stat *leaves =
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE);
  *samples = stat_get_num_samples(leaves);
  *mean = stat_get_mean(leaves);
  error_stack_destroy(error_stack);
  inference_results_destroy(results);
}

// Inference's game holds every tile the target cannot see in the bag, so its
// bag is not the one the target played from, and inference leaves the table
// out: the leaves it infers are the same with one loaded.
static void test_peg_adjust_inference(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  PegAdjust *peg_adjust = create_pattern_table(PEG_ADJUST_PATTERN_EXTREME, 1);
  Game *game = config_game_create(config);
  MoveList *best_list = move_list_create(1);
  int inferences = 0;
  for (int game_idx = 0; game_idx < 4 * PEG_ADJUST_TEST_GAMES &&
                         inferences < PEG_ADJUST_TEST_INFERENCES;
       game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 3000);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int bag = bag_get_letters(game_get_bag(game));
      const Move *best = get_top_equity_move(game, best_list);
      if (bag > 0 && bag < RACK_SIZE &&
          inferences < PEG_ADJUST_TEST_INFERENCES &&
          move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          move_get_tiles_played(best) >= PEG_ADJUST_TEST_INFERENCE_MIN_TILES) {
        uint64_t samples_without = 0;
        double mean_without = 0;
        get_inferred_leaves(config, game, best, &samples_without,
                            &mean_without);
        for (int player_index = 0; player_index < 2; player_index++) {
          set_peg_adjust(players_data, game, player_index, peg_adjust);
        }
        uint64_t samples_with = 0;
        double mean_with = 0;
        get_inferred_leaves(config, game, best, &samples_with, &mean_with);
        for (int player_index = 0; player_index < 2; player_index++) {
          set_peg_adjust(players_data, game, player_index, NULL);
        }
        assert(samples_with == samples_without);
        assert(mean_with == mean_without);
        inferences++;
      }
      play_move(best, game, NULL);
    }
  }
  assert(inferences == PEG_ADJUST_TEST_INFERENCES);
  move_list_destroy(best_list);
  game_destroy(game);
  peg_adjust_destroy(peg_adjust);
  config_destroy(config);
}

// Sims the config's position and candidates with mover_table for the player
// on turn and opponent_table for the other, then unloads both. The sim's
// workers are new ones if sim_ctx is NULL or has run no sim yet, and
// otherwise those of its earlier sims, reset. A context writes every sim's
// results where its first sim's went, so it goes with one sim_results.
static void simulate_with_tables(Config *config, SimCtx **sim_ctx,
                                 PegAdjust *mover_table,
                                 PegAdjust *opponent_table,
                                 SimResults *sim_results) {
  PlayersData *players_data = config_get_players_data(config);
  const Game *game = config_get_game(config);
  const int mover_index = game_get_player_on_turn_index(game);
  set_peg_adjust(players_data, game, mover_index, mover_table);
  set_peg_adjust(players_data, game, 1 - mover_index, opponent_table);
  const error_code_t status =
      config_simulate_and_return_status(config, sim_ctx, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);
  for (int player_index = 0; player_index < 2; player_index++) {
    set_peg_adjust(players_data, game, player_index, NULL);
  }
}

// Whether two sims of the same candidates differ in the opponent's replies
// to one of them, the first ply of its rollouts: in how many there are or in
// their mean score.
static bool sim_replies_differ(const SimResults *first,
                               const SimResults *second) {
  const int number_of_plays = sim_results_get_number_of_plays(first);
  assert(number_of_plays == sim_results_get_number_of_plays(second));
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    const Stat *first_scores = simmed_play_get_score_stat(
        sim_results_get_simmed_play(first, play_idx), 0);
    const Stat *second_scores = simmed_play_get_score_stat(
        sim_results_get_simmed_play(second, play_idx), 0);
    if (stat_get_num_samples(first_scores) !=
            stat_get_num_samples(second_scores) ||
        stat_get_mean(first_scores) != stat_get_mean(second_scores)) {
      return true;
    }
  }
  return false;
}

// A sim models the opponent as playing by the simming player's table,
// whichever table the opponent has. With the same candidates, a sim gives the
// same results with a table for both players, for the simming player only,
// and for the simming player against another table; and the same results
// with a table for the opponent only as with none. This holds for new
// workers and for the reset workers of an earlier sim. The replies do depend
// on the table that plays them: with the table for both players they differ
// from the replies with none. The sims' rollouts are steered by the census
// if use_census.
static void test_peg_adjust_sim(bool use_census) {
  char *set_cmd = get_formatted_string(
      "set -lex CSW21 -wmp true -threads 1 -plies 2 -numplays %d "
      "-iterations %d -minplayiterations %d -scond none -patrollout false "
      "-sinfer false -seed 7 -scensus %s",
      PEG_ADJUST_TEST_SIM_CANDIDATES,
      PEG_ADJUST_TEST_SIM_CANDIDATES * PEG_ADJUST_TEST_SIM_PLAY_ROLLOUTS,
      PEG_ADJUST_TEST_SIM_PLAY_ROLLOUTS, use_census ? "true" : "false");
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  load_and_exec_config_or_die(config, "new");
  PegAdjust *table = create_pattern_table(PEG_ADJUST_PATTERN_ALTERNATING, 1);
  PegAdjust *other_table =
      create_pattern_table(PEG_ADJUST_PATTERN_ALTERNATING, -1);
  // The sims of each of the first three results share a context, so all but
  // the first of them reset its workers. Those of the last create theirs.
  SimCtx *neither_ctx = NULL;
  SimCtx *both_ctx = NULL;
  SimCtx *one_sided_ctx = NULL;
  SimResults *neither = sim_results_create(0);
  SimResults *both = sim_results_create(0);
  SimResults *one_sided = sim_results_create(0);
  SimResults *one_sided_new = sim_results_create(0);
  Game *game = config_get_game(config);
  MoveList *best_list = move_list_create(1);
  bool mover_seen[2] = {false, false};
  int positions = 0;
  int positions_with_other_replies = 0;
  for (int game_idx = 0; game_idx < PEG_ADJUST_TEST_SIM_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 4000);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (bag > 0 && bag <= PEG_ADJUST_TEST_SIM_MAX_BAG) {
        // The candidates of every sim of the position: its best plays
        // without a table.
        load_and_exec_config_or_die(config, "gen");
        simulate_with_tables(config, &neither_ctx, NULL, NULL, neither);
        assert(sim_results_get_census_rollout(neither) == use_census);
        simulate_with_tables(config, &both_ctx, table, table, both);
        assert(sim_results_get_census_rollout(both) == use_census);
        simulate_with_tables(config, &one_sided_ctx, table, NULL, one_sided);
        assert_sim_results_equal(both, one_sided);
        simulate_with_tables(config, &one_sided_ctx, table, other_table,
                             one_sided);
        assert_sim_results_equal(both, one_sided);
        simulate_with_tables(config, &one_sided_ctx, NULL, table, one_sided);
        assert_sim_results_equal(neither, one_sided);
        simulate_with_tables(config, NULL, table, other_table, one_sided_new);
        assert_sim_results_equal(both, one_sided_new);
        simulate_with_tables(config, NULL, NULL, table, one_sided_new);
        assert_sim_results_equal(neither, one_sided_new);
        if (sim_replies_differ(neither, both)) {
          positions_with_other_replies++;
        }
        mover_seen[game_get_player_on_turn_index(game)] = true;
        positions++;
      }
      play_move(get_top_equity_move(game, best_list), game, NULL);
    }
  }
  assert(positions >= PEG_ADJUST_TEST_SIM_GAMES);
  assert(mover_seen[0] && mover_seen[1]);
  assert(positions_with_other_replies > 0);
  move_list_destroy(best_list);
  sim_results_destroy(one_sided_new);
  sim_results_destroy(one_sided);
  sim_results_destroy(both);
  sim_results_destroy(neither);
  sim_ctx_destroy(one_sided_ctx);
  sim_ctx_destroy(both_ctx);
  sim_ctx_destroy(neither_ctx);
  peg_adjust_destroy(other_table);
  peg_adjust_destroy(table);
  config_destroy(config);
}

// A remembered move scored without its adjustment starts the generation
// from too high an equity when its adjustment is negative and the true best
// move's is no better, as under the table of the most negative values. Under
// a long table the bags above the standard length differ too, and a lane's
// bound remembered at one of them is too low at another. Without a table a
// lane's bound holds at every bag.
static void test_peg_adjust_carry(const char *set_cmd) {
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_ALTERNATING, 2);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_FLOOR, 2);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_LONG_ALTERNATING,
                                2);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_LONG_FLOOR, 2);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_LONG_FLOOR, 1);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_LONG_ALTERNATING,
                                0);
}

// Runs every movegen check under set_cmd.
static void test_peg_adjust_config_movegen(const char *set_cmd) {
  test_peg_adjust_movegen(set_cmd);
  test_peg_adjust_bonus(set_cmd);
  test_peg_adjust_carry(set_cmd);
  test_peg_adjust_inference(set_cmd);
}

void test_peg_adjust(void) {
  test_peg_adjust_file();
  test_peg_adjust_bag_class();
  test_peg_adjust_config();
  test_peg_adjust_auto();
  test_peg_adjust_auto_default();
  // The PAT term's bound joins the adjustment in the WMP prune bound.
  const char *set_cmds[] = {
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1",
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1",
      "set -lex CSW21 -wmp true -pat CSW21 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1",
  };
  for (size_t cmd_idx = 0; cmd_idx < sizeof(set_cmds) / sizeof(set_cmds[0]);
       cmd_idx++) {
    test_peg_adjust_config_movegen(set_cmds[cmd_idx]);
  }
  // The rack info table has its own exchange path, and the word info table
  // its own bounds. They are in the data directory where make prepare_data
  // has built them, for the standard board. (DEFAULT_BOARD_DIM is an enum
  // constant, which a preprocessor #if reads as 0.)
  // NOLINTNEXTLINE(misc-redundant-expression)
  const bool is_standard_board = BOARD_DIM == DEFAULT_BOARD_DIM;
  const bool have_tables =
      is_standard_board &&
      cfile_access("./data/lexica/CSW24.rit", CFILE_ACCESS_READ) &&
      cfile_access("./data/lexica/CSW24.wit", CFILE_ACCESS_READ);
  if (have_tables) {
    test_peg_adjust_config_movegen(
        "set -lex CSW24 -wmp true -rit true -wit true -s1 equity -s2 equity "
        "-r1 all -r2 all -numplays 1");
  }
  // The census, which reads the table for its rollouts itself, serves the
  // standard board.
  test_peg_adjust_sim(false);
  if (is_standard_board) {
    test_peg_adjust_sim(true);
  }
}

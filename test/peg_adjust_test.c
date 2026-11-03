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
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Quackle's table, in test/fixtures/strategy.
#define PEG_ADJUST_TEST_FIXTURES_PATH "./test/fixtures"

enum {
  PEG_ADJUST_TEST_GAMES = 4,
  // Only positions from this bag size down can reach the table.
  PEG_ADJUST_TEST_MAX_BAG = PEG_ADJUST_VALUES_LENGTH - 1,
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
};

// Tables that stress the bounds differently: values of both signs, values
// that only raise equities (which a bound must include), values that only
// lower them (which must not overflow a sentinel), the largest values a file
// may hold, of both signs and all negative.
typedef enum {
  PEG_ADJUST_PATTERN_ALTERNATING,
  PEG_ADJUST_PATTERN_POSITIVE,
  PEG_ADJUST_PATTERN_NEGATIVE,
  PEG_ADJUST_PATTERN_EXTREME,
  PEG_ADJUST_PATTERN_FLOOR,
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

static void
assert_peg_adjust_values(const PegAdjust *peg_adjust,
                         const double expected[PEG_ADJUST_VALUES_LENGTH]) {
  for (int index = 0; index < PEG_ADJUST_VALUES_LENGTH; index++) {
    assert(peg_adjust_get_value(peg_adjust, index) ==
           double_to_equity(expected[index]));
  }
}

// Loads <dir>/strategy/<name>.csv, written with contents, and checks its
// values.
static void
assert_peg_adjust_file_values(const char *dir, const char *name,
                              const char *contents,
                              const double expected[PEG_ADJUST_VALUES_LENGTH]) {
  write_peg_adjust_file(dir, name, contents);
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(dir, name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_peg_adjust_values(peg_adjust, expected);
  peg_adjust_destroy(peg_adjust);
  error_stack_destroy(error_stack);
}

static void test_peg_adjust_file(void) {
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(PEG_ADJUST_TEST_FIXTURES_PATH,
                                            "quackle_pegadjust", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(peg_adjust_get_name(peg_adjust), "quackle_pegadjust");
  const double quackle[PEG_ADJUST_VALUES_LENGTH] = {
      0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
  assert_peg_adjust_values(peg_adjust, quackle);

  // A placement's index is the bag less the tiles played, plus RACK_SIZE.
  assert(peg_adjust_get_placement_value(peg_adjust, 1, 1) ==
         double_to_equity(2));
  assert(peg_adjust_get_placement_value(peg_adjust, 1, RACK_SIZE) ==
         double_to_equity(-8));
  assert(peg_adjust_get_placement_value(peg_adjust, 5, 1) ==
         double_to_equity(-1));
  // Plays that leave PEG_ADJUST_VALUES_LENGTH - RACK_SIZE or more tiles in
  // the bag, plays from an empty bag and players without a table get none.
  assert(peg_adjust_get_placement_value(peg_adjust, 7, 1) == 0);
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
  // Blank lines and spaces are allowed, and an index left out is 0.
  const double partial[PEG_ADJUST_VALUES_LENGTH] = {
      0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, 0};
  assert_peg_adjust_file_values(dir, "partial",
                                "# index,value\n"
                                "1,-8\n2,0\n3,-0.5\n4,-2\n5,-3.5\n6,-2\n"
                                "\n   \n"
                                "7,2\n8,10\n  9 , 7 \n10,4\n11,-1\n",
                                partial);
  // Windows line endings, signs, bare decimal points and the largest values.
  const double edges[PEG_ADJUST_VALUES_LENGTH] = {0, 0,    3,     0.5, 0, 0, 0,
                                                  0, 1000, -1000, 0,   0, 0};
  assert_peg_adjust_file_values(
      dir, "edges", "# crlf\r\n+2,3\r\n3,.5\r\n8,1000\r\n9,-1000.0\r\n", edges);
  const double empty[PEG_ADJUST_VALUES_LENGTH] = {0};
  assert_peg_adjust_file_values(dir, "empty", "", empty);
  assert_peg_adjust_file_values(dir, "comments", "# nothing\n#1,5\n", empty);

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
  assert_peg_adjust_file_error(dir, "large_index", "13,2\n");
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
  Equity values[PEG_ADJUST_VALUES_LENGTH];
  for (int index = 0; index < PEG_ADJUST_VALUES_LENGTH; index++) {
    const int alternating = index % 2 == 0 ? 1 : -1;
    double points = 0;
    switch (pattern) {
    case PEG_ADJUST_PATTERN_ALTERNATING:
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
      points = -PEG_ADJUST_MAX_ABS_VALUE;
      break;
    case NUMBER_OF_PEG_ADJUST_PATTERNS:
      log_fatal("invalid pattern");
      break;
    }
    values[index] = double_to_equity(sign * points);
  }
  return peg_adjust_create_from_values("pattern", values);
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
        if (bag > 0 && bag <= PEG_ADJUST_TEST_MAX_BAG) {
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
  Equity values[PEG_ADJUST_VALUES_LENGTH] = {0};
  for (int index = first_index; index <= last_index; index++) {
    values[index] = int_to_equity(PEG_ADJUST_TEST_BONUS);
  }
  PegAdjust *bonus = peg_adjust_create_from_values("bonus", values);
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
      if (bag > 0 && bag <= PEG_ADJUST_TEST_MAX_BAG) {
        // Leaving 1 to 5 tiles in the bag, then emptying it.
        assert_bonus_decides(game, players_data, RACK_SIZE + 1,
                             PEG_ADJUST_VALUES_LENGTH - 1);
        assert_bonus_decides(game, players_data, 1, RACK_SIZE);
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
// as it is and with the opponent's rack returned to it.
static void test_peg_adjust_carry_pattern(const char *set_cmd,
                                          peg_adjust_pattern_t pattern) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  PegAdjust *peg_adjust = create_pattern_table(pattern, 1);
  Game *game = config_game_create(config);
  for (int player_index = 0; player_index < 2; player_index++) {
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
      if (bag > 0 && bag <= PEG_ADJUST_TEST_MAX_BAG) {
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

// A remembered move scored without its adjustment starts the generation
// from too high an equity when its adjustment is negative and the true best
// move's is no better, as under the table of the most negative values.
static void test_peg_adjust_carry(const char *set_cmd) {
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_ALTERNATING);
  test_peg_adjust_carry_pattern(set_cmd, PEG_ADJUST_PATTERN_FLOOR);
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
  test_peg_adjust_config();
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
  // has built them, for the standard board.
#if BOARD_DIM == DEFAULT_BOARD_DIM
  const bool have_tables =
      cfile_access("./data/lexica/CSW24.rit", CFILE_ACCESS_READ) &&
      cfile_access("./data/lexica/CSW24.wit", CFILE_ACCESS_READ);
#else
  const bool have_tables = false;
#endif
  if (have_tables) {
    test_peg_adjust_config_movegen(
        "set -lex CSW24 -wmp true -rit true -wit true -s1 equity -s2 equity "
        "-r1 all -r2 all -numplays 1");
  }
}

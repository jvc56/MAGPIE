#include "peg_adjust_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/peg_adjust.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

// Quackle's table, in test/fixtures/strategy.
#define PEG_ADJUST_TEST_FIXTURES_PATH "./test/fixtures"

enum {
  PEG_ADJUST_TEST_GAMES = 6,
  // Only positions from this bag size down can reach the table.
  PEG_ADJUST_TEST_MAX_BAG = PEG_ADJUST_VALUES_LENGTH - 1,
  PEG_ADJUST_TEST_MOVE_LIST_CAPACITY = 1024,
};

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

static void test_peg_adjust_file(void) {
  ErrorStack *error_stack = error_stack_create();
  PegAdjust *peg_adjust = peg_adjust_create(PEG_ADJUST_TEST_FIXTURES_PATH,
                                            "quackle_pegadjust", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(peg_adjust_get_name(peg_adjust), "quackle_pegadjust");
  const double quackle[PEG_ADJUST_VALUES_LENGTH] = {
      0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
  assert_peg_adjust_values(peg_adjust, quackle);
  peg_adjust_destroy(peg_adjust);

  // Blank lines and spaces are allowed, and an index left out is 0.
  char *dir = test_platform_make_temporary_directory("peg_adjust_test");
  write_peg_adjust_file(dir, "partial",
                        "# index,value\n"
                        "1,-8\n2,0\n3,-0.5\n4,-2\n5,-3.5\n6,-2\n"
                        "\n"
                        "7,2\n8,10\n  9 , 7 \n10,4\n11,-1\n");
  peg_adjust = peg_adjust_create(dir, "partial", error_stack);
  assert(error_stack_is_empty(error_stack));
  const double partial[PEG_ADJUST_VALUES_LENGTH] = {
      0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, 0};
  assert_peg_adjust_values(peg_adjust, partial);

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

  assert_peg_adjust_file_error(dir, "three_fields", "1,2,3\n");
  assert_peg_adjust_file_error(dir, "no_value", "1\n");
  assert_peg_adjust_file_error(dir, "bad_index", "x,2\n");
  assert_peg_adjust_file_error(dir, "bad_value", "1,x\n");
  assert_peg_adjust_file_error(dir, "negative_index", "-1,2\n");
  assert_peg_adjust_file_error(dir, "large_index", "13,2\n");
  assert_peg_adjust_file_error(dir, "twice", "1,2\n1,3\n");

  // Loaded per player through the config, and unloaded with "none".
  char *set_cmd = get_formatted_string(
      "set -lex CSW21 -path %s:%s -pegadjust1 quackle_pegadjust",
      PEG_ADJUST_TEST_FIXTURES_PATH, DEFAULT_TEST_DATA_PATH);
  Config *config = config_create_or_die(set_cmd);
  const PlayersData *players_data = config_get_players_data(config);
  assert(players_data_get_peg_adjust(players_data, 0) != NULL);
  assert(players_data_get_peg_adjust(players_data, 1) == NULL);
  load_and_exec_config_or_die(config, "set -pegadjust quackle_pegadjust");
  assert(players_data_get_peg_adjust(players_data, 0) ==
         players_data_get_peg_adjust(players_data, 1));
  assert(players_data_get_peg_adjust(players_data, 1) != NULL);
  load_and_exec_config_or_die(config, "set -pegadjust2 none");
  assert(players_data_get_peg_adjust(players_data, 0) != NULL);
  assert(players_data_get_peg_adjust(players_data, 1) == NULL);
  config_destroy(config);
  free(set_cmd);

  error_stack_destroy(error_stack);
  test_platform_remove_directory_tree(dir);
  free(dir);
}

// Every move, in a list grown until it holds them all.
static MoveList *generate_all_moves(const Game *game) {
  int capacity = PEG_ADJUST_TEST_MOVE_LIST_CAPACITY;
  MoveList *move_list = move_list_create(capacity);
  while (true) {
    const MoveGenArgs args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&args);
    if (move_list_get_count(move_list) < capacity) {
      return move_list;
    }
    capacity *= 2;
    move_list_resize(move_list, capacity);
  }
}

static Equity generate_best_move_equity(const Game *game) {
  MoveList *move_list = move_list_create(1);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  const Equity equity = move_get_equity(move_list_get_move(move_list, 0));
  move_list_destroy(move_list);
  return equity;
}

// With the table, every tile placement's equity is its equity without it plus
// the table's value for its bag and tiles, an exchange's is unchanged, and
// the best move movegen records is the best of all moves, which checks that
// shadow's equity bounds still bound every move.
static void assert_peg_adjust_equities(const Game *game,
                                       PlayersData *players_data,
                                       PegAdjust *peg_adjust) {
  const int player_index = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, player_index);
  const int bag = bag_get_letters(game_get_bag(game));

  players_data_set_data(players_data, PLAYERS_DATA_TYPE_PEG_ADJUST,
                        player_index, NULL);
  player_update(players_data, player);
  MoveList *without = generate_all_moves(game);

  players_data_set_data(players_data, PLAYERS_DATA_TYPE_PEG_ADJUST,
                        player_index, peg_adjust);
  player_update(players_data, player);
  MoveList *with = generate_all_moves(game);
  const Equity best_equity = generate_best_move_equity(game);

  const int count = move_list_get_count(without);
  assert(count == move_list_get_count(with));
  Equity highest_equity = EQUITY_MIN_VALUE;
  for (int with_idx = 0; with_idx < count; with_idx++) {
    const Move *with_move = move_list_get_move(with, with_idx);
    if (move_get_equity(with_move) > highest_equity) {
      highest_equity = move_get_equity(with_move);
    }
    const Move *without_move = NULL;
    for (int without_idx = 0; without_idx < count; without_idx++) {
      const Move *candidate = move_list_get_move(without, without_idx);
      if (compare_moves_without_equity(with_move, candidate, true) == -1) {
        without_move = candidate;
        break;
      }
    }
    assert(without_move != NULL);
    Equity expected = move_get_equity(without_move);
    if (move_get_type(with_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      expected += peg_adjust_get_placement_value(
          peg_adjust, bag, move_get_tiles_played(with_move));
    }
    assert(move_get_equity(with_move) == expected);
  }
  assert(best_equity == highest_equity);

  players_data_set_data(players_data, PLAYERS_DATA_TYPE_PEG_ADJUST,
                        player_index, NULL);
  player_update(players_data, player);
  move_list_destroy(without);
  move_list_destroy(with);
}

static void test_peg_adjust_movegen(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  PlayersData *players_data = config_get_players_data(config);
  // Large and of both signs, so that the adjustment reorders moves and a
  // bound that left it out would prune the best one.
  Equity values[PEG_ADJUST_VALUES_LENGTH];
  for (int index = 0; index < PEG_ADJUST_VALUES_LENGTH; index++) {
    values[index] = int_to_equity((index % 2 == 0 ? 1 : -1) * 15 * index);
  }
  PegAdjust *peg_adjust = peg_adjust_create_from_values("test", values);
  Game *game = config_game_create(config);
  int positions = 0;
  for (int game_idx = 0; game_idx < PEG_ADJUST_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 1);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int bag = bag_get_letters(game_get_bag(game));
      if (bag > 0 && bag <= PEG_ADJUST_TEST_MAX_BAG) {
        assert_peg_adjust_equities(game, players_data, peg_adjust);
        positions++;
      }
      play_top_n_equity_move(game, 0);
    }
  }
  assert(positions > 0);
  game_destroy(game);
  peg_adjust_destroy(peg_adjust);
  config_destroy(config);
}

void test_peg_adjust(void) {
  test_peg_adjust_file();
  test_peg_adjust_movegen("set -lex CSW21 -wmp false -s1 equity -s2 equity "
                          "-r1 all -r2 all -numplays 1");
  test_peg_adjust_movegen("set -lex CSW21 -wmp true -s1 equity -s2 equity "
                          "-r1 all -r2 all -numplays 1");
}

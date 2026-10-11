#include "history_replay_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gcg.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

typedef struct {
  GameHistory *game_history;
  Game *expected;
  ErrorStack *error_stack;
  int visits;
  int stop_after;
} ReplayCheck;

static bool check_snapshot(const GameHistory *game_history, const Game *game,
                           int event_index, void *user_data) {
  ReplayCheck *check = user_data;
  assert(game_history == check->game_history);
  assert(event_index == check->visits);
  check->visits++;
  game_reset(check->expected);
  game_seed(check->expected, 42);
  game_play_n_events(check->game_history, check->expected, event_index, true,
                     check->error_stack);
  assert(error_stack_is_empty(check->error_stack));
  assert_games_are_equal(game, check->expected, true);
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_get_letter(board, row, col)) {
        const int owner = board_get_square_owner(board, row, col);
        assert(owner == 0 || owner == 1);
      }
    }
  }
  MachineLetter actual_tiles[MAX_BAG_SIZE];
  MachineLetter expected_tiles[MAX_BAG_SIZE];
  const int actual_count = bag_peek_tiles(game_get_bag(game), actual_tiles);
  const int expected_count =
      bag_peek_tiles(game_get_bag(check->expected), expected_tiles);
  assert(actual_count == expected_count);
  for (int tile_idx = 0; tile_idx < actual_count; tile_idx++) {
    assert(actual_tiles[tile_idx] == expected_tiles[tile_idx]);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const Rack *actual_known =
        player_get_known_rack_from_phonies(game_get_player(game, player_idx));
    const Rack *expected_known = player_get_known_rack_from_phonies(
        game_get_player(check->expected, player_idx));
    assert_racks_equal(game_get_ld(game), actual_known, expected_known);
  }
  return check->stop_after != event_index;
}

static void test_initial_position(void) {
  Config *config = config_create_or_die("set -lex CSW24");
  ErrorStack *error_stack = error_stack_create();
  GameHistory *history = config_get_game_history(config);
  StringBuilder *cgp = string_builder_create();
  for (int row = 0; row < BOARD_DIM; row++) {
    if (row) {
      string_builder_add_char(cgp, '/');
    }
    if (row == BOARD_DIM / 2) {
      string_builder_add_formatted_string(cgp, "%dA%d", BOARD_DIM / 2,
                                          BOARD_DIM / 2);
    } else {
      string_builder_add_int(cgp, BOARD_DIM);
    }
  }
  string_builder_add_string(cgp, " AEINRST/BCDFGHJ 100/80 1");
  char *record = get_formatted_string(
      "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#cgp %s\n",
      string_builder_peek(cgp));
  config_parse_gcg_string(config, record, history, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(game_history_get_num_events(history) == 0);
  GameHistory *copy = game_history_duplicate(history);
  game_history_reset(history);
  assert(game_history_get_initial_cgp(history) == NULL);
  assert(game_history_get_unplayed_initial_rack(history, 0, 0) == NULL);
  assert_strings_equal(game_history_get_initial_cgp(copy),
                       string_builder_peek(cgp));
  Game *game = config_game_create(config);
  game_play_n_events(copy, game, 0, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  char *actual = game_get_cgp(game, true);
  assert_strings_equal(actual, string_builder_peek(cgp));
  free(actual);
  StringBuilder *saved = string_builder_create();
  string_builder_add_gcg(saved, game_get_ld(game), copy, false);
  config_parse_gcg_string(config, string_builder_peek(saved), history,
                          error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(game_history_get_initial_cgp(history),
                       string_builder_peek(cgp));
  char *with_move =
      get_formatted_string("%s>A: AEINRST %d%c .T +2 102\n", record,
                           (BOARD_DIM / 2) + 1, 'A' + (BOARD_DIM / 2));
  config_parse_gcg_string(config, with_move, history, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(game_history_get_num_events(history) == 1);
  game_play_n_events(history, game, 0, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  actual = game_get_cgp(game, true);
  assert_strings_equal(actual, string_builder_peek(cgp));
  free(actual);
  game_play_n_events(history, game, 1, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(102));
  assert_rack_equals_string(
      game_get_ld(game), player_get_rack(game_get_player(game, 1)), "BCDFGHJ");
  load_and_exec_config_or_die(config, "set -lex NWL23");
  game_destroy(game);
  game = config_game_create(config);
  game_play_n_events(history, game, 1, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(102));
  assert_rack_equals_string(
      game_get_ld(game), player_get_rack(game_get_player(game, 1)), "BCDFGHJ");
  // Both direct CGPs and file-import CGPs must reject oversized gaps.
  char *bad_cgp = string_duplicate(string_builder_peek(cgp));
  bad_cgp[0] = '9';
  game_load_cgp(game, bad_cgp, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  char *bad_record = get_formatted_string(
      "#lexicon NWL23\n#player1 A Alice\n#player2 B Bob\n#cgp %s\n", bad_cgp);
  config_parse_gcg_string(config, bad_record, history, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  free(bad_record);
  free(bad_cgp);
  free(with_move);
  string_builder_destroy(saved);
  string_builder_destroy(cgp);
  free(record);
  game_history_destroy(copy);
  game_destroy(game);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

static void test_initial_rack_after_exchanges(void) {
  Config *config = config_create_or_die("set -lex CSW24");
  ErrorStack *errors = error_stack_create();
  GameHistory *history = config_get_game_history(config);
  StringBuilder *record = string_builder_create();
  string_builder_add_string(
      record, "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    if (row) {
      string_builder_add_char(record, '/');
    }
    string_builder_add_int(record, BOARD_DIM);
  }
  char *header = string_builder_dump(record, NULL);
  string_builder_add_string(
      record, " AEINRST/BCDFGHJ 100/80 3\n"
              ">A: AEINRST -AEI +0 100\n>B: BCDFGHJ -BCD +0 80\n");
  config_parse_gcg_string(config, string_builder_peek(record), history, errors);
  assert(error_stack_is_empty(errors));
  assert(game_history_get_unplayed_initial_rack(history, 1, 1));
  assert(!game_history_get_unplayed_initial_rack(history, 0, 1));
  assert(!game_history_get_unplayed_initial_rack(history, 1, 2));
  load_and_exec_config_or_die(config, "goto 2");
  load_and_exec_config_or_die(config, "rack NRSTOOO");
  assert_config_exec_status(config, "commit pass",
                            ERROR_STATUS_COMMIT_PREVIOUS_PASS_NOT_FOUND);
  history = config_get_game_history(config);
  // The untouched opponent rack remains valid when the starting CGP already
  // has five scoreless turns and the first recorded turn is the final pass.
  string_builder_clear(record);
  string_builder_add_string(record, header);
  string_builder_add_string(record, " AEINRST/BCDFGHJ 100/80 5\n");
  config_parse_gcg_string(config, string_builder_peek(record), history, errors);
  assert(error_stack_is_empty(errors));
  load_and_exec_config_or_die(config, "goto 0");
  load_and_exec_config_or_die(config, "commit pass");
  assert(player_get_score(game_get_player(config_get_game(config), 0)) ==
         int_to_equity(93));
  assert(player_get_score(game_get_player(config_get_game(config), 1)) ==
         int_to_equity(54));
  free(header);
  string_builder_destroy(record);
  error_stack_destroy(errors);
  config_destroy(config);
}

static void test_initial_endgame_racks(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  Config *config = config_create_or_die("set -lex CSW24");
  ErrorStack *errors = error_stack_create();
  GameHistory *history = config_get_game_history(config);
  const char *record =
      "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0\n>A: FV 6B .F +10 452\n";
  config_parse_gcg_string(config, record, history, errors);
  assert(error_stack_is_empty(errors));
  Game *game = config_game_create(config);
  for (int validate = 0; validate < 2; validate++) {
    game_play_n_events(history, game, 1, validate != 0, errors);
    assert(error_stack_is_empty(errors));
    assert(game_get_player_on_turn_index(game) == 1);
    assert(bag_is_empty(game_get_bag(game)));
    assert_rack_equals_string(game_get_ld(game),
                              player_get_rack(game_get_player(game, 0)), "V");
    assert_rack_equals_string(
        game_get_ld(game), player_get_rack(game_get_player(game, 1)), "AADIZ");
  }
  game_destroy(game);
  error_stack_destroy(errors);
  config_destroy(config);
}

void test_history_replay(void) {
  test_initial_position();
  test_initial_rack_after_exchanges();
  test_initial_endgame_racks();
  Config *config = config_create_or_die("set -lex CSW24");
  ErrorStack *error_stack = error_stack_create();
  GameHistory *game_history = config_get_game_history(config);
  char *gcg = fileproxy_get_string_from_filename("test/fixtures/replay.gcg",
                                                 error_stack);
  assert(error_stack_is_empty(error_stack));
  config_parse_gcg_string(config, gcg, game_history, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(gcg);
  Game *game = config_game_create(config);
  Game *expected = config_game_create(config);
  ReplayCheck check = {game_history, expected, error_stack, 0, -1};
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(game_history, game, check_snapshot, &check, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(check.visits == game_history_get_num_events(game_history) + 1);
  check.visits = 0;
  check.stop_after = 2;
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(game_history, game, check_snapshot, &check, error_stack);
  assert(check.visits == 3);
  assert(error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  check.stop_after = -1;
  // Empty records still visit the starting position once.
  config_parse_gcg_string(config,
                          "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n",
                          game_history, error_stack);
  assert(error_stack_is_empty(error_stack));
  check.visits = 0;
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(game_history, game, check_snapshot, &check, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(check.visits == 1);
  game_destroy(expected);
  game_destroy(game);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

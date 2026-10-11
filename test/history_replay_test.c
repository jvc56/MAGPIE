#include "history_replay_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
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

void test_history_replay(void) {
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

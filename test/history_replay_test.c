#include "history_replay_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

typedef struct {
  GameHistory *history;
  Game *expected;
  ErrorStack *errors;
  int visits;
  int stop_after;
} ReplayCheck;

static void check_snapshot(const GameHistory *history, const Game *game,
                           int event_index, void *context) {
  ReplayCheck *check = context;
  assert(history == check->history);
  assert(event_index == check->visits);
  check->visits++;
  game_reset(check->expected);
  game_seed(check->expected, 42);
  game_play_n_events(check->history, check->expected, event_index, false,
                     check->errors);
  assert(error_stack_is_empty(check->errors));
  char *actual = game_get_cgp(game, false);
  char *expected = game_get_cgp(check->expected, false);
  assert(strings_equal(actual, expected));
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
    for (int ml = 0; ml < ld_get_size(game_get_ld(game)); ml++) {
      assert(rack_get_letter(actual_known, ml) ==
             rack_get_letter(expected_known, ml));
    }
  }
  free(actual);
  free(expected);
  if (check->stop_after == event_index) {
    error_stack_push(check->errors, ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG,
                     string_duplicate("visitor stopped replay"));
  }
}

void test_history_replay(void) {
  Config *config = config_create_or_die("set -lex CSW24");
  ErrorStack *errors = error_stack_create();
  GameHistory *history = config_get_game_history(config);
  char *gcg =
      fileproxy_get_string_from_filename("test/fixtures/replay.gcg", errors);
  assert(error_stack_is_empty(errors));
  config_parse_gcg_string(config, gcg, history, errors);
  assert(error_stack_is_empty(errors));
  free(gcg);
  Game *game = config_game_create(config);
  Game *expected = config_game_create(config);
  ReplayCheck check = {history, expected, errors, 0, -1};
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(history, game, check_snapshot, &check, errors);
  assert(error_stack_is_empty(errors));
  assert(check.visits == game_history_get_num_events(history) + 1);
  check.visits = 0;
  check.stop_after = 2;
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(history, game, check_snapshot, &check, errors);
  assert(check.visits == 3);
  assert(!error_stack_is_empty(errors));
  error_stack_reset(errors);
  check.stop_after = -1;
  // Empty records still visit the starting position once.
  config_parse_gcg_string(config,
                          "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n",
                          history, errors);
  assert(error_stack_is_empty(errors));
  check.visits = 0;
  game_reset(game);
  game_seed(game, 42);
  game_replay_history(history, game, check_snapshot, &check, errors);
  assert(error_stack_is_empty(errors));
  assert(check.visits == 1);
  game_destroy(expected);
  game_destroy(game);
  error_stack_destroy(errors);
  config_destroy(config);
}

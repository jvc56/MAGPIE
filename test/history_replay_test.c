#include "history_replay_test.h"

#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
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
} ReplayCheck;

static void check_snapshot(const GameHistory *history, const Game *game,
                           int event_index, void *context) {
  ReplayCheck *check = context;
  assert(history == check->history);
  assert(event_index == check->visits++);
  game_play_n_events(check->history, check->expected, event_index, false,
                     check->errors);
  assert(error_stack_is_empty(check->errors));
  char *actual = game_get_cgp(game, false);
  char *expected = game_get_cgp(check->expected, false);
  assert(strings_equal(actual, expected));
  free(actual);
  free(expected);
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
  ReplayCheck check = {history, expected, errors, 0};
  game_replay_history(history, game, check_snapshot, &check, errors);
  assert(error_stack_is_empty(errors));
  assert(check.visits == game_history_get_num_events(history) + 1);
  // Empty records still visit the starting position once.
  config_parse_gcg_string(config,
                          "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n",
                          history, errors);
  assert(error_stack_is_empty(errors));
  check.visits = 0;
  game_replay_history(history, game, check_snapshot, &check, errors);
  assert(error_stack_is_empty(errors));
  assert(check.visits == 1);
  game_destroy(expected);
  game_destroy(game);
  error_stack_destroy(errors);
  config_destroy(config);
}

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "../src/ent/config.h"
#include "../src/ent/game.h"

#include "board_test.h"
#include "test_constants.h"
#include "test_util.h"
#include "testconfig.h"

void test_board_cross_set_for_cross_set_index(Game *game, int cross_set_index) {
  // Test cross set
  Board *board = game_get_board(game);

  clear_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION, cross_set_index);
  set_cross_set_letter(get_cross_set_pointer(board, 0, 0,
                                             BOARD_HORIZONTAL_DIRECTION,
                                             cross_set_index),
                       13);
  assert(get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                       cross_set_index) == 8192);
  set_cross_set_letter(get_cross_set_pointer(board, 0, 0,
                                             BOARD_HORIZONTAL_DIRECTION,
                                             cross_set_index),
                       0);
  assert(get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                       cross_set_index) == 8193);

  uint64_t cs =
      get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION, cross_set_index);
  assert(!allowed(cs, 1));
  assert(allowed(cs, 0));
  assert(!allowed(cs, 14));
  assert(allowed(cs, 13));
  assert(!allowed(cs, 12));
}

void test_board(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Board *board = game_get_board(game);

  load_cgp(game, VS_ED);

  assert(!get_anchor(board, 3, 3, 0) && !get_anchor(board, 3, 3, 1));
  assert(get_anchor(board, 12, 12, 0) && get_anchor(board, 12, 12, 1));
  assert(get_anchor(board, 4, 3, 1) && !get_anchor(board, 4, 3, 0));

  test_board_cross_set_for_cross_set_index(game, 0);
  test_board_cross_set_for_cross_set_index(game, 1);
  destroy_game(game);
}
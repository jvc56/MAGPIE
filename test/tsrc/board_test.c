#include <assert.h>
#include <stdint.h>

#include "../../src/def/board_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"

#include "board_test.h"
#include "test_constants.h"
#include "test_util.h"

void test_board_cross_set_for_cross_set_index(Game *game, int cross_set_index) {
  // Test cross set
  Board *board = game_get_board(game, 0);

  board_clear_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                        cross_set_index);
  board_set_cross_set_letter(
      board_get_cross_set_pointer(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                  cross_set_index),
      13);
  assert(board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index) == 8192);
  board_set_cross_set_letter(
      board_get_cross_set_pointer(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                  cross_set_index),
      0);
  assert(board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index) == 8193);

  uint64_t cs = board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                    cross_set_index);
  assert(!board_is_letter_allowed_in_cross_set(cs, 1));
  assert(board_is_letter_allowed_in_cross_set(cs, 0));
  assert(!board_is_letter_allowed_in_cross_set(cs, 14));
  assert(board_is_letter_allowed_in_cross_set(cs, 13));
  assert(!board_is_letter_allowed_in_cross_set(cs, 12));
}

void test_board() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game, 0);

  game_load_cgp(game, VS_ED);

  assert(!board_get_anchor(board, 3, 3, 0) &&
         !board_get_anchor(board, 3, 3, 1));
  assert(board_get_anchor(board, 12, 12, 0) &&
         board_get_anchor(board, 12, 12, 1));
  assert(board_get_anchor(board, 4, 3, 1) && !board_get_anchor(board, 4, 3, 0));

  test_board_cross_set_for_cross_set_index(game, 0);
  test_board_cross_set_for_cross_set_index(game, 1);
  game_destroy(game);
  config_destroy(config);
}
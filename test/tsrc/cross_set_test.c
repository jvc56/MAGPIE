#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/cross_set_defs.h"
#include "../../src/def/letter_distribution_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

void test_gen_cross_set(Game *game, int row, int col, const char *letters,
                        int expected_cross_score) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  uint64_t expected_cross_set = cross_set_from_string(ld, letters);
  uint64_t actual_cross_set =
      board_get_cross_set(board, row, col, BOARD_VERTICAL_DIRECTION, 0);
  assert(expected_cross_set == actual_cross_set);
  int actual_cross_score =
      board_get_cross_score(board, row, col, BOARD_VERTICAL_DIRECTION, 0);
  assert(expected_cross_score == actual_cross_score);
}

void test_gen_cross_set_row(Game *game, int row, int col,
                            const char *row_content, const char *letters,
                            int expected_cross_score) {
  set_row(game, row, row_content);
  game_gen_all_cross_sets(game);
  test_gen_cross_set(game, row, col, letters, expected_cross_score);
}

void test_cross_set() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);

  // TestGencross_setLoadedGame
  game_load_cgp(game, VS_MATT);
  test_gen_cross_set(game, 10, 10, "?E", 11);
  test_gen_cross_set(game, 2, 4, "?DHKLRSV", 9);
  test_gen_cross_set(game, 8, 7, "?S", 11);
  test_gen_cross_set(game, 12, 8, "", 11);
  test_gen_cross_set(game, 3, 1, "", 10);
  test_gen_cross_set(game, 6, 8, "", 5);
  test_gen_cross_set(game, 2, 10, "?M", 2);
  board_transpose(board);
  test_gen_cross_set(game, 10, 10, TRIVIAL_CROSS_SET_STRING, 0);
  test_gen_cross_set(game, 2, 4, "?HIO", 1);
  test_gen_cross_set(game, 12, 8, TRIVIAL_CROSS_SET_STRING, 0);
  test_gen_cross_set(game, 2, 10, "?CDEFHIMSTWY", 2);
  board_transpose(board);

  // TestGencross_setEdges
  game_reset(game);
  test_gen_cross_set_row(game, 4, 0, " A", "?ABDFHKLMNPTYZ", 1);
  test_gen_cross_set_row(game, 4, 1, "A", "?ABDEGHILMNRSTWXY", 1);
  test_gen_cross_set_row(game, 4, 13, "              F", "?EIO", 4);
  test_gen_cross_set_row(game, 4, 14, "             F ", "?AE", 4);
  test_gen_cross_set_row(game, 4, 14, "          WECH ", "?T", 12);
  test_gen_cross_set_row(game, 4, 14, "           ZZZ ", "", 30);
  test_gen_cross_set_row(game, 4, 14, "       ZYZZYVA ", "?S", 43);
  test_gen_cross_set_row(game, 4, 14, "        ZYZZYV ", "?A", 42);
  test_gen_cross_set_row(game, 4, 14, "       Z Z Y A ", "?ABDEGHILMNRSTWXY", 1);
  test_gen_cross_set_row(game, 4, 12, "       z z Y A ", "?E", 5);
  test_gen_cross_set_row(game, 4, 14, "OxYpHeNbUTAzON ", "?E", 15);
  test_gen_cross_set_row(game, 4, 6, "OXYPHE BUTAZONE", "?N", 40);
  test_gen_cross_set_row(game, 4, 0, " YHJKTKHKTLV", "", 42);
  test_gen_cross_set_row(game, 4, 14, "   YHJKTKHKTLV ", "", 42);
  test_gen_cross_set_row(game, 4, 6, "YHJKTK HKTLV", "", 42);

  // Test setting cross sets with tiles on either side
  test_gen_cross_set_row(game, 4, 1, "D NATURES", "?E", 9);
  test_gen_cross_set_row(game, 4, 1, "D N", "?AEIOU", 3);
  test_gen_cross_set_row(game, 4, 1, "D NT", "?EIU", 4);
  test_gen_cross_set_row(game, 4, 1, "D NTS", "?EIU", 5);
  test_gen_cross_set_row(game, 4, 1, "R VOTED", "?E", 10);
  test_gen_cross_set_row(game, 4, 5, "PHENY BUTAZONE", "?L", 32);
  test_gen_cross_set_row(game, 4, 6, "OXYPHE BUTAZONE", "?N", 40);
  test_gen_cross_set_row(game, 4, 1, "R XED", "?A", 12);
  test_gen_cross_set_row(game, 4, 2, "BA ED", "?AKLNRSTY", 7);
  test_gen_cross_set_row(game, 4, 1, "X Z", "", 18);
  test_gen_cross_set_row(game, 4, 6, "STRONG L", "?Y", 8);
  test_gen_cross_set_row(game, 4, 1, "W SIWYG", "?Y", 16);
  test_gen_cross_set_row(game, 4, 0, " EMSTVO", "?Z", 11);
  test_gen_cross_set_row(game, 4, 1, "T UNFOLD", "", 11);
  test_gen_cross_set_row(game, 4, 1, "S OBCONIc", "", 11);

  // TestGenAllcross_sets
  game_load_cgp(game, VS_ED);
  test_gen_cross_set(game, 8, 8, "?OS", 8);
  board_transpose(board);
  test_gen_cross_set(game, 8, 8, "?S", 9);
  board_transpose(board);
  test_gen_cross_set(game, 5, 11, "?S", 5);
  board_transpose(board);
  test_gen_cross_set(game, 11, 5, "?AO", 2);
  board_transpose(board);
  test_gen_cross_set(game, 8, 13, "?AEOU", 1);
  board_transpose(board);
  test_gen_cross_set(game, 13, 8, "?AEIMOUY", 3);
  board_transpose(board);
  test_gen_cross_set(game, 9, 13, "?HMNPST", 1);
  board_transpose(board);
  test_gen_cross_set(game, 13, 9, TRIVIAL_CROSS_SET_STRING, 0);
  board_transpose(board);
  test_gen_cross_set(game, 14, 14, TRIVIAL_CROSS_SET_STRING, 0);
  board_transpose(board);
  test_gen_cross_set(game, 14, 14, TRIVIAL_CROSS_SET_STRING, 0);
  board_transpose(board);
  test_gen_cross_set(game, 12, 12, "", 0);
  board_transpose(board);
  test_gen_cross_set(game, 12, 12, "", 0);
  board_transpose(board);

  // TestUpdateSinglecross_set
  game_load_cgp(game, VS_MATT);
  board_set_letter(board, 8, 10, 19);
  board_set_letter(board, 9, 10, 0);
  board_set_letter(board, 10, 10, 4);
  board_set_letter(board, 11, 10, 11);
  game_gen_cross_set(game, 7, 10, BOARD_VERTICAL_DIRECTION, 0);
  board_transpose(board);
  game_gen_cross_set(game, 10, 7, BOARD_VERTICAL_DIRECTION, 0);
  board_transpose(board);
  assert(board_get_cross_set(board, 7, 10, BOARD_VERTICAL_DIRECTION, 0) == 0);
  assert(board_get_cross_set(board, 7, 10, BOARD_HORIZONTAL_DIRECTION, 0) == 0);

  game_destroy(game);
  config_destroy(config);
}
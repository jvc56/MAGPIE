#include "../src/word_prune.h"

#include <assert.h>

#include "../src/game.h"
#include "testconfig.h"

void assert_row_equals(const LetterDistribution *ld, BoardRows *board_rows,
                       int row, const char *human_readable_row) {                        
  char row_copy[BOARD_DIM+1];
  memory_copy(row_copy, human_readable_row, BOARD_DIM+1);
  for (int i = 0; i < BOARD_DIM; i++) {
    if (row_copy[i] == ' ') {
      row_copy[i] = '?';
    }
  }                        
  uint8_t expected[BOARD_DIM];
  str_to_machine_letters(ld, row_copy, false, expected, BOARD_DIM);
  for (int i = 0; i < BOARD_DIM; i++) {
    assert(board_rows->rows[row].letters[i] == expected[i]);
  }
}

void test_unique_rows(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_cgp(game, qi_qis);

  BoardRows *board_rows = create_board_rows(game);
  assert(board_rows->num_rows == 6);
  assert_row_equals(ld, board_rows, 0, "               ");
  assert_row_equals(ld, board_rows, 1, "       I       ");
  assert_row_equals(ld, board_rows, 2, "       QIS     ");
  assert_row_equals(ld, board_rows, 3, "      I        ");
  assert_row_equals(ld, board_rows, 4, "      QI       ");
  assert_row_equals(ld, board_rows, 5, "      S        ");

  destroy_board_rows(board_rows);
}

void test_possible_words(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  PossibleWordList *possible_word_list = create_possible_word_list(game, NULL);
  assert(possible_word_list != NULL);
  destroy_possible_word_list(possible_word_list);
}

void test_word_prune(TestConfig *testconfig) {
  test_unique_rows(testconfig);
  test_possible_words(testconfig);
}
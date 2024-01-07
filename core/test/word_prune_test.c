#include "../src/word_prune.h"

#include <assert.h>

#include "../src/game.h"
#include "testconfig.h"

void assert_row_equals(const LetterDistribution *ld, BoardRows *board_rows,
                       int row, const char *human_readable_row) {
  char row_copy[BOARD_DIM + 1];
  memory_copy(row_copy, human_readable_row, BOARD_DIM + 1);
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

  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[0]) == 15);
  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[1]) == 6);
  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[2]) == 6);
  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[3]) == 7);
  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[4]) == 6);
  assert(max_nonplaythrough_spaces_in_row(&board_rows->rows[5]) == 7);

  destroy_board_rows(board_rows);
}

void test_add_words_without_playthrough(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  const Game *game = create_game(config);
  const KWG *kwg = game->players[game->player_on_turn_index]->kwg;

  PossibleWordList *possible_word_list = create_empty_possible_word_list();
  assert(possible_word_list != NULL);
  assert(possible_word_list->num_words == 0);
  Bag *full_bag = create_bag(game->gen->letter_distribution);
  Rack *bag_as_rack = create_rack(game->gen->letter_distribution->size);
  add_bag_to_rack(full_bag, bag_as_rack);
  uint8_t word[BOARD_DIM];
  add_words_without_playthrough(kwg, kwg_get_dawg_root_node_index(kwg),
                                bag_as_rack, 2, word, 0, false,
                                possible_word_list);
  assert(possible_word_list->num_words == 127);  // all two letter words
  destroy_possible_word_list(possible_word_list);

  possible_word_list = create_empty_possible_word_list();
  assert(possible_word_list != NULL);
  assert(possible_word_list->num_words == 0);
  add_words_without_playthrough(kwg, kwg_get_dawg_root_node_index(kwg),
                                bag_as_rack, 15, word, 0, false,
                                possible_word_list);
  // all words except unplayable (PIZZAZZ, etc.)
  // 24 of the 279077 words in CSW21 are not playable using a standard English
  // tile set. 17 have >3 Z's, 2 have >3 K's, 5 have >6 S's.
  assert(possible_word_list->num_words == 279053);

  /*
      shuffle_words(possible_word_list);
      assert(possible_word_list->num_words == 279053);

      uint64_t start_time = __rdtsc();  // in nanoseconds
      sort_words(possible_word_list);
      uint64_t end_time = __rdtsc();  // in milliseconds
      printf("sort_words took %f seconds\n", (end_time - start_time) * 1e-9);
  */
  destroy_possible_word_list(possible_word_list);
}

void test_add_playthrough_words_from_row(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_cgp(game, qi_qis);

  BoardRows *board_rows = create_board_rows(game);
  assert_row_equals(ld, board_rows, 4, "      QI       ");

  PossibleWordList *possible_word_list = create_empty_possible_word_list();

  Bag *full_bag = create_bag(game->gen->letter_distribution);
  Rack *bag_as_rack = create_rack(game->gen->letter_distribution->size);
  add_bag_to_rack(full_bag, bag_as_rack);

  add_playthrough_words_from_row(&board_rows->rows[4], game->players[0]->kwg,
                                 bag_as_rack, possible_word_list);
  // cat ~/scrabble/csw21.txt| grep QI | wc -l = 26
  // QINGHAOSUS doesn't fit on the board and QI itself doesn't play a tile                                  
  assert(possible_word_list->num_words == 24);
/*  
  for (int i = 0; i < possible_word_list->num_words; i++) {
    for (int j = 0; j < possible_word_list->possible_words[i].word_length;
         j++) {
      uint8_t ml = possible_word_list->possible_words[i].word[j];
      char c = 'A' + ml - 1;
      printf("%c", c);
    }
    printf("\n");
  }
*/
  destroy_possible_word_list(possible_word_list);
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
  test_add_words_without_playthrough(testconfig);
  test_add_playthrough_words_from_row(testconfig);
  test_possible_words(testconfig);
}
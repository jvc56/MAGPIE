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

void assert_word_count(const LetterDistribution *ld, PossibleWordList *pwl,
                       const char *human_readable_word, int expected_count) {
  int expected_length = string_length(human_readable_word);
  uint8_t expected[BOARD_DIM];
  str_to_machine_letters(ld, human_readable_word, false, expected, BOARD_DIM);
  int count = 0;
  for (int i = 0; i < pwl->num_words; i++) {
    if ((pwl->possible_words[i].word_length == expected_length) &&
        (memory_compare(pwl->possible_words[i].word, expected,
                        expected_length) == 0)) {
      count++;
    }
  }
  assert(count == expected_count);
}

void test_unique_rows(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char qi_qis[300] =
      "15/15/15/15/15/15/15/6Qi7/6I8/6S8/15/15/15/15/15 / 22/12 "
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
  destroy_game(game);
}

void test_add_words_without_playthrough(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
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
  destroy_rack(bag_as_rack);
  destroy_bag(full_bag);
  destroy_game(game);
}

void test_add_playthrough_words_from_row(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 / 22/12 "
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
  destroy_rack(bag_as_rack);
  destroy_bag(full_bag);
  destroy_board_rows(board_rows);
  destroy_game(game);
}

void test_multiple_playthroughs_in_row(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 / 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_game(game, sb);
    printf("%s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */

  BoardRows *board_rows = create_board_rows(game);
  assert(board_rows->num_rows == 30);

  PossibleWordList *possible_word_list = create_empty_possible_word_list();
  assert(possible_word_list != NULL);

  Rack *bag_as_rack = create_rack(game->gen->letter_distribution->size);
  add_bag_to_rack(game->gen->bag, bag_as_rack);
  /*
    sb = create_string_builder();
    string_builder_add_rack(bag_as_rack, game->gen->letter_distribution, sb);
    printf("rack: %s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */
  assert_row_equals(ld, board_rows, 27, "U      GI   O  ");
  add_playthrough_words_from_row(&board_rows->rows[27], game->players[0]->kwg,
                                 bag_as_rack, possible_word_list);

  // found both as (U)NGIRT and UN(GI)RT
  assert_word_count(ld, possible_word_list, "UNGIRT", 2);

  // check that we get some GI...O words only once
  assert_word_count(ld, possible_word_list, "GITANO", 1);
  assert_word_count(ld, possible_word_list, "GITANOS", 1);
  assert_word_count(ld, possible_word_list, "GIUSTO", 1);
  assert_word_count(ld, possible_word_list, "SIGISBEO", 1);

  sort_possible_word_list(possible_word_list);
  PossibleWordList *unique =
      create_unique_possible_word_list(possible_word_list);
  assert_word_count(ld, unique, "UNGIRT", 1);

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
  assert(unique->num_words == 2370);

  destroy_possible_word_list(unique);
  destroy_possible_word_list(possible_word_list);
  destroy_rack(bag_as_rack);
  destroy_game(game);
}

// tests blank playthrough and edge of board
void test_enguard_d_row(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  const LetterDistribution *ld = game->gen->letter_distribution;

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 / 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_game(game, sb);
    printf("%s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */

  BoardRows *board_rows = create_board_rows(game);
  assert(board_rows->num_rows == 30);

  PossibleWordList *possible_word_list = create_empty_possible_word_list();
  assert(possible_word_list != NULL);

  Rack *bag_as_rack = create_rack(game->gen->letter_distribution->size);
  add_bag_to_rack(game->gen->bag, bag_as_rack);

  /*
    sb = create_string_builder();
    string_builder_add_rack(bag_as_rack, game->gen->letter_distribution, sb);
    printf("rack: %s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */

  assert_row_equals(ld, board_rows, 10, " ENGUARD      D");

  // uint64_t start_time = __rdtsc();  // in nanoseconds
  add_playthrough_words_from_row(&board_rows->rows[10], game->players[0]->kwg,
                                 bag_as_rack, possible_word_list);
  // uint64_t end_time = __rdtsc();  // in milliseconds

  /*
    printf("add_playthrough_words_from_row took %f seconds\n",
           (end_time - start_time) * 1e-9);
    printf("possible_word_list->num_words: %d\n",
    possible_word_list->num_words);
  */

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
  PossibleWordList *unique =
      create_unique_possible_word_list(possible_word_list);
  assert_word_count(ld, unique, "ZOOMED", 1);
  assert_word_count(ld, unique, "ENGUARDED", 1);
  assert_word_count(ld, unique, "ENGUARDS", 1);
  assert_word_count(ld, unique, "ENGUARDING", 1);
  assert_word_count(ld, unique, "ENGUARD", 0);

  assert(unique->num_words == 1827);

  destroy_possible_word_list(unique);
  destroy_possible_word_list(possible_word_list);
    destroy_rack(bag_as_rack);
  destroy_game(game);
}

void test_possible_words(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 / 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  /*
      StringBuilder *sb = create_string_builder();
      string_builder_add_game(game, sb);
      printf("%s\n", string_builder_peek(sb));
      destroy_string_builder(sb);
  */

  //  uint64_t start_time = __rdtsc();  // in nanoseconds
  PossibleWordList *possible_word_list = create_possible_word_list(game, NULL);
  //  uint64_t end_time = __rdtsc();  // in milliseconds
  /*
    printf("create_possible_word_list took %f seconds\n",
           (end_time - start_time) * 1e-9);
    printf("possible_word_list->num_words: %d\n",
    possible_word_list->num_words);
  */

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

  assert(possible_word_list->num_words == 62702);
  destroy_possible_word_list(possible_word_list);
  destroy_game(game);
}

void test_word_prune(TestConfig *testconfig) {
  test_unique_rows(testconfig);
  test_add_words_without_playthrough(testconfig);
  test_add_playthrough_words_from_row(testconfig);
  test_enguard_d_row(testconfig);
  test_multiple_playthroughs_in_row(testconfig);
  test_possible_words(testconfig);
}
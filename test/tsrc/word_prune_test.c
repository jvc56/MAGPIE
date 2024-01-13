#include "../../src/impl/word_prune.h"

#include <assert.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/letter_distribution_defs.h"
#include "../../src/ent/config.h"
#include "../../src/ent/dictionary_word.h"
#include "../../src/ent/game.h"
#include "../../src/ent/player.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"
#include "test_util.h"

void assert_word_count(const LetterDistribution *ld, DictionaryWordList *words,
                       const char *human_readable_word, int expected_count) {
  int expected_length = string_length(human_readable_word);
  uint8_t expected[BOARD_DIM];
  ld_str_to_mls(ld, human_readable_word, false, expected,
                string_length(human_readable_word));
  int count = 0;
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if ((dictionary_word_get_length(word) == expected_length) &&
        (memory_compare(dictionary_word_get_word(word), expected,
                        expected_length) == 0)) {
      count++;
    }
  }
  assert(count == expected_count);
}

void test_possible_words() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);

  char zonule[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 / 340/419 0 lex CSW21;";
  game_load_cgp(game, zonule);

  /*
      StringBuilder *sb = create_string_builder();
      string_builder_add_game(game, sb);
      printf("%s\n", string_builder_peek(sb));
      destroy_string_builder(sb);
  */

  //  uint64_t start_time = __rdtsc();  // in nanoseconds
  DictionaryWordList *possible_word_list = dictionary_word_list_create();
  generate_possible_words(game, NULL, possible_word_list);

  //  uint64_t end_time = __rdtsc();  // in milliseconds
  /*
    printf("create_possible_word_list took %f seconds\n",
           (end_time - start_time) * 1e-9);
    printf("possible_word_list->num_words: %d\n",
    possible_word_list->num_words);
  */

/*
  for (int i = 0; i < dictionary_word_list_get_count(possible_word_list); i++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(possible_word_list, i);
    for (int j = 0; j < dictionary_word_get_length(word); j++) {
      uint8_t ml = dictionary_word_get_word(word)[j];
      char c = 'A' + ml - 1;
      printf("%c", c);
    }
    printf("\n");
  }
*/
  assert(dictionary_word_list_get_count(possible_word_list) == 62702);
  dictionary_word_list_destroy(possible_word_list);
  game_destroy(game);
}

void test_word_prune() { test_possible_words(); }
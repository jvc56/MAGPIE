#include "brute_dictionary_test.h"

#include "../src/ent/dictionary_word.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/impl/config.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

static DictionaryWord create_word_from_string(const LetterDistribution *ld,
                                               const char *human_readable_word) {
  DictionaryWord word;
  int length = (int)string_length(human_readable_word);
  ld_str_to_mls(ld, human_readable_word, false, word.word, length);
  word.length = (uint8_t)length;
  return word;
}

void word_lookup_linear_search(void) {
  // Create Config with CSW24 dictionary
  // This automatically creates the unsorted words list
  Config *config = config_create_or_die("set -lex CSW24");
  const LetterDistribution *ld = config_get_ld(config);
  PlayersData *players_data = config_get_players_data(config);

  // Get the shuffled word list from players data (automatically created)
  const DictionaryWordList *word_list = players_data_get_unsorted_words(players_data, 0);

  // Create test words
  DictionaryWord brute = create_word_from_string(ld, "BRUTE");
  DictionaryWord aa = create_word_from_string(ld, "AA");
  DictionaryWord zzzs = create_word_from_string(ld, "ZZZS");
  DictionaryWord olaugh = create_word_from_string(ld, "OLAUGH");

  // Perform checks
  assert(dictionary_word_list_contains_word_linear_search(word_list, &brute) == true);
  assert(dictionary_word_list_contains_word_linear_search(word_list, &aa) == true);
  assert(dictionary_word_list_contains_word_linear_search(word_list, &zzzs) == true);
  assert(dictionary_word_list_contains_word_linear_search(word_list, &olaugh) == false);

  // Cleanup
  config_destroy(config);
}

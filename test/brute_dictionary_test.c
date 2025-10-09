#include "brute_dictionary_test.h"

#include "../src/def/kwg_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static void shuffle_word_list(DictionaryWordList *word_list, uint64_t seed) {
  int count = dictionary_word_list_get_count(word_list);
  if (count <= 1) {
    return;
  }

  XoshiroPRNG *prng = prng_create(seed);

  // Fisher-Yates shuffle
  for (int i = count - 1; i > 0; i--) {
    int j = (int)prng_get_random_number(prng, i + 1);

    // Swap words at positions i and j
    DictionaryWord temp = *dictionary_word_list_get_word(word_list, i);
    *dictionary_word_list_get_word(word_list, i) =
        *dictionary_word_list_get_word(word_list, j);
    *dictionary_word_list_get_word(word_list, j) = temp;
  }

  prng_destroy(prng);
}

static DictionaryWord create_word_from_string(const LetterDistribution *ld,
                                               const char *human_readable_word) {
  DictionaryWord word;
  int length = (int)string_length(human_readable_word);
  ld_str_to_mls(ld, human_readable_word, false, word.word, length);
  word.length = (uint8_t)length;
  return word;
}

void word_lookup_linear_search(void) {
  // Create Config with CSW21 dictionary (CSW24 doesn't exist in the codebase)
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);

  // Get the KWG from the game
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player);

  // Dump KWG to DictionaryWordList
  DictionaryWordList *word_list = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), word_list, NULL);

  // Shuffle the word list
  shuffle_word_list(word_list, 42);  // Using seed 42 for reproducibility

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
  dictionary_word_list_destroy(word_list);
  game_destroy(game);
  config_destroy(config);
}

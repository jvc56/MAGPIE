#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void add_test_word(const LetterDistribution *ld, DictionaryWordList *words,
                   const char *human_readable_word) {
  int length = (int)string_length(human_readable_word);
  MachineLetter word[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_word, false, word, length);
  dictionary_word_list_add_word(words, word, length);
}

void assert_word_lists_are_equal(const DictionaryWordList *expected,
                                 const DictionaryWordList *actual) {
  assert(dictionary_word_list_get_count(expected) ==
         dictionary_word_list_get_count(actual));
  for (int i = 0; i < dictionary_word_list_get_count(expected); i++) {
    const DictionaryWord *expected_word =
        dictionary_word_list_get_word(expected, i);
    const DictionaryWord *actual_word =
        dictionary_word_list_get_word(actual, i);
    assert(dictionary_word_get_length(expected_word) ==
           dictionary_word_get_length(actual_word));
    assert(memcmp(dictionary_word_get_word(expected_word),
                  dictionary_word_get_word(actual_word),
                  dictionary_word_get_length(expected_word)) == 0);
  }
}

uint32_t kwg_prefix_arc_aux(const KWG *kwg, uint32_t node_index,
                            const DictionaryWord *prefix, int pos) {
  const MachineLetter ml = dictionary_word_get_word(prefix)[pos];
  const uint32_t next_node_index = kwg_get_next_node_index(kwg, node_index, ml);
  if (pos + 1 == dictionary_word_get_length(prefix)) {
    return next_node_index;
  }
  if (next_node_index == 0) {
    return 0;
  }
  return kwg_prefix_arc_aux(kwg, next_node_index, prefix, pos + 1);
}

uint32_t kwg_dawg_prefix_arc(const KWG *kwg, const LetterDistribution *ld,
                             const char *human_readable_prefix) {
  // Make a list with one word in it just because I don't have a handy function
  // to create a DictionaryWord by itself.
  DictionaryWordList *prefix_list = dictionary_word_list_create();
  uint8_t prefix_bytes[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_prefix, false, prefix_bytes,
                string_length(human_readable_prefix));
  dictionary_word_list_add_word(prefix_list, prefix_bytes,
                                (int)string_length(human_readable_prefix));
  const DictionaryWord *prefix = dictionary_word_list_get_word(prefix_list, 0);
  const uint32_t arc =
      kwg_prefix_arc_aux(kwg, kwg_get_dawg_root_node_index(kwg), prefix, 0);
  dictionary_word_list_destroy(prefix_list);
  return arc;
}

uint32_t kwg_gaddag_prefix_arc(const KWG *kwg, const LetterDistribution *ld,
                               const char *human_readable_prefix) {
  char string_for_conversion[MAX_KWG_STRING_LENGTH];
  strncpy(string_for_conversion, human_readable_prefix,
          sizeof(string_for_conversion));
  for (size_t i = 0; i < string_length(human_readable_prefix); i++) {
    if (string_for_conversion[i] == '@') {
      string_for_conversion[i] = '?';
    }
  }
  // Make a list with one word in it just because I don't have a handy function
  // to create a DictionaryWord by itself.
  DictionaryWordList *prefix_list = dictionary_word_list_create();
  uint8_t prefix_bytes[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, string_for_conversion, false, prefix_bytes,
                string_length(string_for_conversion));
  dictionary_word_list_add_word(prefix_list, prefix_bytes,
                                (int)string_length(string_for_conversion));
  const DictionaryWord *prefix = dictionary_word_list_get_word(prefix_list, 0);
  const uint32_t arc =
      kwg_prefix_arc_aux(kwg, kwg_get_root_node_index(kwg), prefix, 0);
  dictionary_word_list_destroy(prefix_list);
  return arc;
}

void test_qi_xi_xu_word_trie(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "QI");
  add_test_word(ld, words, "XI");
  add_test_word(ld, words, "XU");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_NONE);

  // Q and X prefixes are not empty
  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") != 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "X") != 0);
  // The full words are empty, none of them can be extended
  assert(kwg_dawg_prefix_arc(kwg, ld, "QI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XU") == 0);

  // Q and X prefixes are not merged, Z is not found and thus returning empty
  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") !=
         kwg_dawg_prefix_arc(kwg, ld, "X"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "Z") == 0);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);

  assert_word_lists_are_equal(words, encoded_words);

  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_egg_unmerged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "EGG");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_NONE);

  assert(kwg_gaddag_prefix_arc(kwg, ld, "G") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "GG") != 0);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_unmerged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_NONE);

  // Reversed words are gaddag entries. Check that the last letters of the words
  // are nonempty prefixes. However, they are not merged.
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "R") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") !=
         kwg_gaddag_prefix_arc(kwg, ld, "R"));

  // Some other checks
  assert(kwg_gaddag_prefix_arc(kwg, ld, "NEERA") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "C@AREE") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "AC@RE") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "RAC@E") != 0);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_exact_merged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "R") != 0);
  // N does not exactly equal R because R also has suffixes starting from the R
  // in the middle of the word.
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") !=
         kwg_gaddag_prefix_arc(kwg, ld, "R"));

  // But all of these merge to the same node.
  uint32_t c_aree = kwg_gaddag_prefix_arc(kwg, ld, "C@AREE");
  assert(c_aree != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "AC@REE") == c_aree);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "RAC@EE") == c_aree);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "ERAC@E") == c_aree);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "EERAC@") == c_aree);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_trie(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if (dictionary_word_get_length(word) == 2) {
      dictionary_word_list_add_word(two_letter_words,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(words);

  KWG *kwg = make_kwg_from_words(two_letter_words, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_NONE);

  // not merged: DA/DE/DI/DO, PA/PE/PI/PO, TA/TE/TI/TO
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") !=
         kwg_dawg_prefix_arc(kwg, ld, "P"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") !=
         kwg_dawg_prefix_arc(kwg, ld, "T"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "P") !=
         kwg_dawg_prefix_arc(kwg, ld, "T"));

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(two_letter_words);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_merged_dawg(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if (dictionary_word_get_length(word) == 2) {
      dictionary_word_list_add_word(two_letter_words,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(words);

  KWG *kwg = make_kwg_from_words(two_letter_words, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_EXACT);

  // merged: DA/DE/DI/DO, PA/PE/PI/PO, TA/TE/TI/TO
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") ==
         kwg_dawg_prefix_arc(kwg, ld, "P"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") ==
         kwg_dawg_prefix_arc(kwg, ld, "T"));

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(two_letter_words);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_polish_gaddag(void) {
  Config *config = config_create_or_die("set -lex OSPS49");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *ziet_words = dictionary_word_list_create();
  const MachineLetter ziet = ld_hl_to_ml(ld, "Ź");
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    const uint8_t length = dictionary_word_get_length(word);
    for (int j = 0; j < length; j++) {
      if (dictionary_word_get_word(word)[j] == ziet) {
        dictionary_word_list_add_word(ziet_words,
                                      dictionary_word_get_word(word), length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);
  DictionaryWordList *ziet_gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(ziet_words, ziet_gaddag_strings);

  KWG *kwg = make_kwg_from_words(ziet_words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(ziet_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(ziet_words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, NULL);
  assert_word_lists_are_equal(ziet_gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(ziet_gaddag_strings);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_large_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *q_words = dictionary_word_list_create();
  const MachineLetter q = ld_hl_to_ml(ld, "Q");
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    const uint8_t length = dictionary_word_get_length(word);
    for (int j = 0; j < length; j++) {
      if (dictionary_word_get_word(word)[j] == q) {
        dictionary_word_list_add_word(q_words, dictionary_word_get_word(word),
                                      length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);
  DictionaryWordList *q_gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(q_words, q_gaddag_strings);

  KWG *kwg = make_kwg_from_words(q_words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(q_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(q_words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, NULL);
  assert_word_lists_are_equal(q_gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(q_gaddag_strings);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_full_csw_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(words, gaddag_strings);

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  bool *nodes_reached =
      calloc_or_die(kwg_get_number_of_nodes(kwg), sizeof(bool));
  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words,
                  nodes_reached);
  assert_word_lists_are_equal(words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, nodes_reached);
  assert_word_lists_are_equal(gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(gaddag_strings);

  // first two nodes point to the dawg root and gaddag
  for (int i = 2; i < kwg_get_number_of_nodes(kwg); i++) {
    assert(nodes_reached[i]);
  }
  free(nodes_reached);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_kwg_maker(void) {
  test_qi_xi_xu_word_trie();
  test_egg_unmerged_gaddag();
  test_careen_career_unmerged_gaddag();
  test_careen_career_exact_merged_gaddag();
  test_two_letter_trie();
  test_two_letter_merged_dawg();
  test_polish_gaddag();
  test_large_gaddag();
  test_full_csw_gaddag();
}

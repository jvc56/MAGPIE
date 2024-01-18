#include "../../src/impl/kwg_maker.h"

#include <assert.h>

#include "../../src/def/kwg_defs.h"
#include "../../src/ent/config.h"
#include "../../src/ent/dictionary_word.h"
#include "../../src/impl/word_prune.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"
#include "test_util.h"

void add_test_word(const LetterDistribution *ld, DictionaryWordList *words,
                   const char *human_readable_word) {
  int length = string_length(human_readable_word);
  uint8_t word[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_word, false, word, length);
  dictionary_word_list_add_word(words, word, length);
}

void assert_word_lists_are_equal(const DictionaryWordList *expected,
                                 const DictionaryWordList *actual) {
  assert(dictionary_word_list_get_count(expected) ==
         dictionary_word_list_get_count(actual));
  for (int i = 0; i < dictionary_word_list_get_count(expected); i++) {
    DictionaryWord *expected_word = dictionary_word_list_get_word(expected, i);
    DictionaryWord *actual_word = dictionary_word_list_get_word(actual, i);
    assert(dictionary_word_get_length(expected_word) ==
           dictionary_word_get_length(actual_word));
    assert(memory_compare(dictionary_word_get_word(expected_word),
                          dictionary_word_get_word(actual_word),
                          dictionary_word_get_length(expected_word)) == 0);
  }
}

uint32_t kwg_prefix_arc_aux(const KWG *kwg, uint32_t node_index,
                            const DictionaryWord *prefix, int pos) {
  //printf("kwg_prefix_arc_aux(%d, %d)\n", node_index, pos);                               
  const uint8_t ml = dictionary_word_get_word(prefix)[pos];
  //printf("ml: %d\n", ml);
  const uint32_t next_node_index = kwg_get_next_node_index(kwg, node_index, ml);
  //printf("ml: %d, next_node_index: %d\n", ml, next_node_index);
  if (pos + 1 == dictionary_word_get_length(prefix)) {
    return next_node_index;
  }
  if (next_node_index == 0) {
    return 0;
  }
  return kwg_prefix_arc_aux(kwg, next_node_index, prefix, pos + 1);
}

uint32_t kwg_dawg_prefix_arc(const KWG *kwg, LetterDistribution *ld,
                             const char *human_readable_prefix) {
  // Make a list with one word in it just because I don't have a handy function
  // to create a DictionaryWord by itself.
  DictionaryWordList *prefix_list = dictionary_word_list_create();
  uint8_t prefix_bytes[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_prefix, false, prefix_bytes,
                string_length(human_readable_prefix));
  dictionary_word_list_add_word(prefix_list, prefix_bytes,
                                string_length(human_readable_prefix));
  DictionaryWord *prefix = dictionary_word_list_get_word(prefix_list, 0);
  return kwg_prefix_arc_aux(kwg, kwg_get_dawg_root_node_index(kwg), prefix, 0);
}

void test_qi_xi_xu_word_trie() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "QI");
  add_test_word(ld, words, "XI");
  add_test_word(ld, words, "XU");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_NONE);

  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") != 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "X") != 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "QI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XU") == 0);

  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") !=
         kwg_dawg_prefix_arc(kwg, ld, "X"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "Z") == 0);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words);

  assert_word_lists_are_equal(words, encoded_words);

  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_unmerged_gaddag() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_NONE);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_exact_merged_gaddag() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_trie() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  Game *game = game_create(config);
  DictionaryWordList *words = dictionary_word_list_create();
  generate_possible_words(game, NULL, words);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    DictionaryWord *word = dictionary_word_list_get_word(words, i);
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
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(two_letter_words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_merged_dawg() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  Game *game = game_create(config);
  DictionaryWordList *words = dictionary_word_list_create();
  generate_possible_words(game, NULL, words);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    DictionaryWord *word = dictionary_word_list_get_word(words, i);
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
  assert(kwg_dawg_prefix_arc(kwg, ld, "P") ==
         kwg_dawg_prefix_arc(kwg, ld, "T"));         

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(two_letter_words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_word_prune_dawg_and_gaddag() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  Game *game = game_create(config);
  char taurine[300] =
      "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/"
      "AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 lex NWL20;";

  game_load_cgp(game, taurine);

  DictionaryWordList *words = dictionary_word_list_create();
  generate_possible_words(game, NULL, words);

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(words);
  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_large_gaddag() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  Game *game = game_create(config);
  DictionaryWordList *words = dictionary_word_list_create();
  generate_possible_words(game, NULL, words);

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_kwg_maker() {
  test_qi_xi_xu_word_trie();
  // test_careen_career_unmerged_gaddag();
  // test_careen_career_exact_merged_gaddag();
  test_two_letter_trie();
  test_two_letter_merged_dawg();
  // for (int i = 0; i < 100; i++) {
  //  test_word_prune_dawg_and_gaddag();
  //}
  //test_large_gaddag();
}

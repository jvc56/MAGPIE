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

void test_two_letter_merged_dawg() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  Game *game = game_create(config);
  DictionaryWordList *words = dictionary_word_list_create();
  generate_possible_words(game, NULL, words);

  DictionaryWordList *short_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if (dictionary_word_get_length(word) <= 15) {
      dictionary_word_list_add_word(short_words, dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(words);

  KWG *kwg = make_kwg_from_words(short_words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  dictionary_word_list_destroy(short_words);
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
  // test_qi_xi_xu_word_trie();
  // test_careen_career_unmerged_gaddag();
  // test_careen_career_exact_merged_gaddag();
  test_two_letter_merged_dawg();
  // test_large_gaddag();
}

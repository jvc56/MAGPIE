#include "../../src/impl/kwg_maker.h"

#include <assert.h>

#include "../../src/def/kwg_defs.h"
#include "../../src/ent/config.h"
#include "../../src/ent/dictionary_word.h"
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

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_NONE);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_kwg_maker() { test_qi_xi_xu_word_trie(); }
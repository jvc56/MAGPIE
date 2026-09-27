#include "pat_lexicon_test.h"

#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/pat_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_lexicon.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "test_util.h"
#include <assert.h>
#include <stddef.h>

static MachineLetter pat_lexicon_test_ml(const LetterDistribution *ld,
                                         const char *letter) {
  MachineLetter ml;
  assert(ld_str_to_mls(ld, letter, false, &ml, 1) == 1);
  return ml;
}

// A six-word lexicon small enough to count by hand: AE AT CAT CATS QI TA,
// with English tile values (A E I S T 1, C 3, Q 10).
static void test_pat_lexicon_tables(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  static const char *const word_strings[] = {"AE",   "AT", "CAT",
                                             "CATS", "QI", "TA"};
  for (size_t word_idx = 0;
       word_idx < sizeof(word_strings) / sizeof(word_strings[0]); word_idx++) {
    MachineLetter word[8];
    const int length =
        ld_str_to_mls(ld, word_strings[word_idx], false, word, 8);
    assert(length > 0);
    dictionary_word_list_add_word(words, word, length);
  }
  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);
  const MachineLetter ml_a = pat_lexicon_test_ml(ld, "A");
  const MachineLetter ml_c = pat_lexicon_test_ml(ld, "C");
  const MachineLetter ml_e = pat_lexicon_test_ml(ld, "E");
  const MachineLetter ml_i = pat_lexicon_test_ml(ld, "I");
  const MachineLetter ml_q = pat_lexicon_test_ml(ld, "Q");
  const MachineLetter ml_s = pat_lexicon_test_ml(ld, "S");
  const MachineLetter ml_t = pat_lexicon_test_ml(ld, "T");
  const MachineLetter ml_z = pat_lexicon_test_ml(ld, "Z");

  PATWeights *pat = pat_create_zeroed("lexicon");
  pat_prepare_hook_flex(pat, kwg, ld);
  pat_require_prepared(pat);

  // Two-letter words containing each letter: A is in AE, AT and TA.
  assert(pat_get_hook_flex(pat, ml_a) == 3);
  assert(pat_get_hook_flex(pat, ml_t) == 2);
  assert(pat_get_hook_flex(pat, ml_e) == 1);
  assert(pat_get_hook_flex(pat, ml_q) == 1);
  assert(pat_get_hook_flex(pat, ml_i) == 1);
  assert(pat_get_hook_flex(pat, ml_c) == 0);
  assert(pat_get_hook_flex(pat, ml_s) == 0);

  // Counts are 8 log2(1 + words), rounded: one word 8, two 13, three 16.
  // Scores are the mean value of the rest of the word, rounded.
  // CAT starts with C (rest AT: 2) and ends with T (rest CA: 4).
  assert(pat_get_through_count_end(pat, 0, ml_c, 3) == 8);
  assert(pat_get_through_score_end(pat, 0, ml_c, 3) == 2);
  assert(pat_get_through_count_end(pat, 1, ml_t, 3) == 8);
  assert(pat_get_through_score_end(pat, 1, ml_t, 3) == 4);
  // CATS ends with S (rest CAT: 5).
  assert(pat_get_through_count_end(pat, 1, ml_s, 4) == 8);
  assert(pat_get_through_score_end(pat, 1, ml_s, 4) == 5);
  // Two-letter words starting with A: AE and AT, each with 1 left.
  assert(pat_get_through_count_end(pat, 0, ml_a, 2) == 13);
  assert(pat_get_through_score_end(pat, 0, ml_a, 2) == 1);
  // QI: Q first (rest I: 1), I last (rest Q: 10).
  assert(pat_get_through_score_end(pat, 0, ml_q, 2) == 1);
  assert(pat_get_through_score_end(pat, 1, ml_i, 2) == 10);
  // The unsigned table pools both ends: A starts AE and AT and ends TA.
  assert(pat_get_through_count(pat, ml_a, 2) == 16);
  assert(pat_get_through_count(pat, ml_z, 2) == 0);
  assert(pat_get_through_count(pat, ml_a, PAT_MAX_THROUGH_LEN) == 0);

  // Run-keyed tables: the key is the word's first (end 0) or last (end 1)
  // letters in word order.
  const MachineLetter key_at[] = {ml_a, ml_t};
  assert(pat_get_run_through_count(pat, 1, key_at, 2, 3) == 8);
  assert(pat_get_run_through_score(pat, 1, key_at, 2, 3) == 3);
  const MachineLetter key_ca[] = {ml_c, ml_a};
  assert(pat_get_run_through_count(pat, 0, key_ca, 2, 4) == 8);
  assert(pat_get_run_through_score(pat, 0, key_ca, 2, 4) == 2);
  const MachineLetter key_ats[] = {ml_a, ml_t, ml_s};
  assert(pat_get_run_through_count(pat, 1, key_ats, 3, 4) == 8);
  assert(pat_get_run_through_score(pat, 1, key_ats, 3, 4) == 3);
  // No word ends in AT at length 4, or starts with AT at length 3.
  assert(pat_get_run_through_count(pat, 1, key_at, 2, 4) == 0);
  assert(pat_get_run_through_count(pat, 0, key_at, 2, 3) == 0);
  // Keys outside 1..PAT_RUN_THROUGH_MAX_KEY and unprepared tables read 0.
  assert(pat_get_run_through_count(pat, 1, key_at, 0, 3) == 0);
  assert(pat_get_run_through_count(pat, 1, key_ats, PAT_RUN_THROUGH_MAX_KEY + 1,
                                   4) == 0);
  PATWeights *unprepared = pat_create_zeroed("unprepared");
  assert(pat_get_run_through_count(unprepared, 1, key_at, 2, 3) == 0);
  assert(pat_get_run_through_score(unprepared, 1, key_at, 2, 3) == 0);
  pat_destroy(unprepared);

  // Without a lexicon every table is empty, but the weights count as
  // prepared.
  pat_prepare_hook_flex(pat, NULL, ld);
  pat_require_prepared(pat);
  assert(pat_get_hook_flex(pat, ml_a) == 0);
  assert(pat_get_through_count(pat, ml_a, 2) == 0);
  assert(pat_get_through_count_end(pat, 0, ml_c, 3) == 0);

  pat_destroy(pat);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
  config_destroy(config);
}

void test_pat_lexicon(void) { test_pat_lexicon_tables(); }

#ifndef KWG_MAKER_TEST_H
#define KWG_MAKER_TEST_H

#include "../src/ent/dictionary_word.h"

// Defined in kwg_maker_test.c; shared so other suites can compare the word
// set a KWG encodes against an expected list.
void assert_word_lists_are_equal(const DictionaryWordList *expected,
                                 const DictionaryWordList *actual);

void test_kwg_maker(void);
void test_kwg_tail_merge(void);
void test_kwg_tail_reorder(void);
void test_kwg_merge_build_bench(void);

#endif
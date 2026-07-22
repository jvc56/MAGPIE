#include "word_info_table_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A tiny self-contained dictionary built from arbitrary machine letters
// (1..7), so the substring values are hand-verifiable. We deliberately do
// NOT go through a KWG/lexicon here: the maker only needs a word list.
//
// Letters: A=1, B=2, C=3, R=4, S=5, T=6.
enum { A = 1, B = 2, C = 3, R = 4, S = 5, T = 6 };

static void add_word(DictionaryWordList *words, const MachineLetter *letters,
                     int len) {
  dictionary_word_list_add_word(words, letters, len);
}

// Brute-force reference: the union of lettersets of every word in `words`
// of total length L that contains `key` (length key_len) as a contiguous
// substring.
static uint32_t reference_value(const DictionaryWordList *words,
                                const MachineLetter *key, int key_len,
                                int len) {
  uint32_t result = 0;
  const int num_words = dictionary_word_list_get_count(words);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *dw = dictionary_word_list_get_word(words, word_idx);
    const MachineLetter *w = dictionary_word_get_word(dw);
    const int wlen = dictionary_word_get_length(dw);
    if (wlen != len) {
      continue;
    }
    bool contains = false;
    for (int start = 0; start + key_len <= wlen; start++) {
      if (memcmp(w + start, key, (size_t)key_len * sizeof(MachineLetter)) ==
          0) {
        contains = true;
        break;
      }
    }
    if (!contains) {
      continue;
    }
    for (int k = 0; k < wlen; k++) {
      result |= 1U << w[k];
    }
  }
  return result;
}

// Assert that the trie's value row for `key` matches the brute-force
// reference at every length.
static void assert_key_matches_reference(const WordInfoTable *wit,
                                         const DictionaryWordList *words,
                                         const MachineLetter *key,
                                         int key_len) {
  const uint32_t *row = word_info_table_lookup(wit, key, key_len);
  assert(row != NULL);
  // Row entry i corresponds to total word length (key_len + i).
  for (int len = key_len; len <= BOARD_DIM; len++) {
    assert(row[len - key_len] == reference_value(words, key, key_len, len));
  }
}

static void assert_tries_equal(const WitTrie *a, const WitTrie *b, int stride) {
  assert(a->num_nodes == b->num_nodes);
  assert(a->root == b->root);
  assert(a->num_values == b->num_values);
  for (uint32_t node_idx = 0; node_idx < a->num_nodes; node_idx++) {
    assert(a->node_tile[node_idx] == b->node_tile[node_idx]);
    assert(a->node_last[node_idx] == b->node_last[node_idx]);
    assert(a->node_child[node_idx] == b->node_child[node_idx]);
    assert(a->node_value[node_idx] == b->node_value[node_idx]);
  }
  for (size_t i = 0; i < (size_t)a->num_values * (size_t)stride; i++) {
    assert(a->values[i] == b->values[i]);
  }
}

static void assert_wits_equal(const WordInfoTable *a, const WordInfoTable *b) {
  assert(a->version == b->version);
  for (int len = 1; len <= BOARD_DIM; len++) {
    assert_tries_equal(&a->tries[len], &b->tries[len], wit_stride_for_len(len));
  }
}

void test_word_info_table(void) {
  static const MachineLetter at[] = {A, T};
  static const MachineLetter cat[] = {C, A, T};
  static const MachineLetter cats[] = {C, A, T, S};
  static const MachineLetter rat[] = {R, A, T};
  static const MachineLetter ab[] = {A, B};

  DictionaryWordList *words = dictionary_word_list_create();
  // Insert in non-sorted order to exercise the maker's sorted insertion.
  add_word(words, rat, 3);
  add_word(words, at, 2);
  add_word(words, cats, 4);
  add_word(words, ab, 2);
  add_word(words, cat, 3);

  WordInfoTable *wit = make_word_info_table_from_words(words);
  assert(wit != NULL);
  assert(wit->version == WIT_VERSION);
  // One value row per distinct word, bucketed into its length's trie:
  // length 2 = {AT, AB}, length 3 = {CAT, RAT}, length 4 = {CATS}.
  assert(wit->tries[2].num_values == 2);
  assert(wit->tries[3].num_values == 2);
  assert(wit->tries[4].num_values == 1);
  // Each trie's value stride is right-sized to its key length.
  assert(wit->tries[2].values != NULL);
  assert(wit->tries[1].num_values == 0); // no length-1 words

  // Every inserted word must look up to a value row whose own-length entry
  // contains its own letters.
  const int num_words = dictionary_word_list_get_count(words);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *dw = dictionary_word_list_get_word(words, word_idx);
    const MachineLetter *w = dictionary_word_get_word(dw);
    const int wlen = dictionary_word_get_length(dw);
    uint32_t own = 0;
    for (int k = 0; k < wlen; k++) {
      own |= 1U << w[k];
    }
    const uint32_t *row = word_info_table_lookup(wit, w, wlen);
    assert(row != NULL);
    assert((row[0] & own) == own); // own length is row entry 0
    assert_key_matches_reference(wit, words, w, wlen);
  }

  // Spot-check a multi-word substring relationship by hand. "AT" appears in
  // AT, CAT, CATS, RAT.
  {
    const uint32_t *row = word_info_table_lookup(wit, at, 2);
    assert(row != NULL);
    // Entry i = total length (2 + i).
    assert(row[0] == ((1U << A) | (1U << T)));                         // AT
    assert(row[1] == ((1U << A) | (1U << T) | (1U << C) |              // CAT
                      (1U << R)));                                     // RAT
    assert(row[2] == ((1U << A) | (1U << T) | (1U << C) | (1U << S))); // CATS
  }

  // Phony 1: a path that exists in the trie but is not a word ("CA" is a
  // prefix of CAT/CATS but never inserted as a word) -> NULL (permit all).
  {
    static const MachineLetter ca[] = {C, A};
    assert(word_info_table_lookup(wit, ca, 2) == NULL);
  }
  // Phony 2: a path that does not exist at all ("TA") -> NULL.
  {
    static const MachineLetter ta[] = {T, A};
    assert(word_info_table_lookup(wit, ta, 2) == NULL);
  }
  // Phony 3: a real word extended past any word ("CATSS") -> NULL.
  {
    static const MachineLetter catss[] = {C, A, T, S, S};
    assert(word_info_table_lookup(wit, catss, 5) == NULL);
  }

  // ---- Roundtrip file I/O ----
  const char *data_paths = DEFAULT_TEST_DATA_PATH;
  const char *wit_name = "wit_test_tiny";
  ErrorStack *error_stack = error_stack_create();
  char *wit_filename = data_filepaths_get_writable_filename(
      data_paths, wit_name, DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));

  word_info_table_write_to_file(wit, wit_filename, error_stack);
  assert(error_stack_is_empty(error_stack));

  WordInfoTable *wit_loaded =
      word_info_table_create(data_paths, wit_name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wit_loaded != NULL);
  assert_wits_equal(wit, wit_loaded);

  // The loaded table answers lookups identically.
  {
    const uint32_t *row = word_info_table_lookup(wit_loaded, at, 2);
    assert(row != NULL);
    assert(row[1] == ((1U << A) | (1U << T) | (1U << C) | (1U << R)));
    static const MachineLetter ca[] = {C, A};
    assert(word_info_table_lookup(wit_loaded, ca, 2) == NULL);
  }

  word_info_table_destroy(wit_loaded);
  free(wit_filename);
  error_stack_destroy(error_stack);
  word_info_table_destroy(wit);
  dictionary_word_list_destroy(words);
}

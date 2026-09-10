#include "word_plus_floater_maker_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/kwg.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/impl/word_plus_floater_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  BAD_TRIE_ROOT,
  BAD_TRIE_DUPLICATE_VALUE,
  BAD_TRIE_MISSING_VALUE,
  BAD_TRIE_CYCLE,
  BAD_TRIE_LETTER,
  BAD_TRIE_LAST_FLAG,
  BAD_TRIE_COUNT,
};

static void add_literal(DictionaryWordList *words, const char *literal) {
  MachineLetter letters[BOARD_DIM] = {0};
  const size_t length = strlen(literal);
  assert(length <= BOARD_DIM);
  for (size_t position = 0; position < length; position++) {
    assert(literal[position] >= 'A' && literal[position] <= 'Z');
    letters[position] = (MachineLetter)(literal[position] - 'A' + 1);
  }
  dictionary_word_list_add_word(words, letters, (int)length);
}

static DictionaryWordList *make_tiny_words(void) {
  static const char *const literals[] = {
      "A",    "AA",   "AB",    "AT",    "BA",    "ZZ",     "AAA",   "ABA",
      "CAT",  "TAT",  "AAAA",  "ABAB",  "SCAT",  "TATA",   "ABABA", "AAAAA",
      "CATC", "CHAT", "TATAT", "ATCAT", "CATAT", "ABABAB", "BBAAB"};
  DictionaryWordList *words = dictionary_word_list_create();
  for (size_t word_idx = 0; word_idx < sizeof(literals) / sizeof(literals[0]);
       word_idx++) {
    add_literal(words, literals[word_idx]);
  }
  // The same base occurs at both board edges. These words exercise the
  // maximum final length and both extreme signed floater offsets.
  MachineLetter longest[BOARD_DIM];
  for (int position = 0; position < BOARD_DIM; position++) {
    longest[position] = 3;
  }
  longest[0] = 1;
  longest[1] = 20;
  longest[BOARD_DIM - 2] = 1;
  longest[BOARD_DIM - 1] = 20;
  dictionary_word_list_add_word(words, longest, BOARD_DIM);
  for (int position = 0; position < BOARD_DIM; position++) {
    longest[position] = 1;
  }
  dictionary_word_list_add_word(words, longest, BOARD_DIM);
  MachineLetter too_long[BOARD_DIM + 1];
  memset(too_long, 1, sizeof(too_long));
  dictionary_word_list_add_word(words, too_long, BOARD_DIM + 1);
  dictionary_word_list_sort(words);
  return words;
}

// Independent position oracle: remove the literal base occurrence and the
// one fixed floater position, then union every remaining position. It does
// not use letter counts, trie traversal, or the maker's cell-index helper.
static uint32_t reference_mask(const DictionaryWordList *words,
                               const DictionaryWord *base, int final_length,
                               int delta, MachineLetter floater) {
  uint32_t result = 0;
  const int base_length = dictionary_word_get_length(base);
  const MachineLetter *base_letters = dictionary_word_get_word(base);
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    if (dictionary_word_get_length(word) != final_length) {
      continue;
    }
    const MachineLetter *letters = dictionary_word_get_word(word);
    for (int start = 0; start + base_length <= final_length; start++) {
      const int floater_position = start + delta;
      if (floater_position < 0 || floater_position >= final_length ||
          (floater_position >= start &&
           floater_position < start + base_length) ||
          letters[floater_position] != floater ||
          memcmp(letters + start, base_letters, (size_t)base_length) != 0) {
        continue;
      }
      for (int position = 0; position < final_length; position++) {
        if (position != floater_position &&
            (position < start || position >= start + base_length)) {
          result |= UINT32_C(1) << letters[position];
        }
      }
    }
  }
  return result;
}

static void assert_table_matches_reference(const WordInfoTable *wit,
                                           const DictionaryWordList *words) {
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *base = dictionary_word_list_get_word(words, word_idx);
    const int base_length = dictionary_word_get_length(base);
    if (base_length < WPF_MIN_BLOCK_LENGTH ||
        base_length > WPF_MAX_BLOCK_LENGTH) {
      continue;
    }
    const uint32_t *wit_row = word_info_table_lookup(
        wit, dictionary_word_get_word(base), base_length);
    assert(wit_row != NULL);
    const size_t value_id = (size_t)(wit_row - wit->tries[base_length].values) /
                            (size_t)wit_stride_for_len(base_length);
    assert(wit->word_plus_floater[base_length] != NULL);
    const uint32_t *row =
        wit->word_plus_floater[base_length] +
        (value_id * word_plus_floater_cells_per_key(base_length));
    size_t cell = 0;
    for (int final_length = base_length + 1; final_length <= BOARD_DIM;
         final_length++) {
      const int extension = final_length - base_length;
      for (int delta = -extension; delta < final_length; delta++) {
        if (delta >= 0 && delta < base_length) {
          continue;
        }
        for (int floater = 1; floater <= WPF_ALPHABET_SIZE; floater++) {
          assert(row[cell] == reference_mask(words, base, final_length, delta,
                                             (MachineLetter)floater));
          cell++;
        }
      }
    }
    assert(cell == word_plus_floater_cells_per_key(base_length));
  }
}

static void assert_tables_equal(const WordInfoTable *first,
                                const WordInfoTable *second) {
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    assert(first->tries[length].num_values == second->tries[length].num_values);
    const size_t stored_rows = first->tries[length].num_values;
    if (stored_rows == 0) {
      assert(first->word_plus_floater[length] == NULL);
      assert(second->word_plus_floater[length] == NULL);
      continue;
    }
    assert((first->word_plus_floater[length] == NULL) ==
           (second->word_plus_floater[length] == NULL));
    if (first->word_plus_floater[length] == NULL) {
      continue;
    }
    assert(second->word_plus_floater[length] != NULL);
    assert(memcmp(first->word_plus_floater[length],
                  second->word_plus_floater[length],
                  stored_rows * word_plus_floater_cells_per_key(length) *
                      sizeof(uint32_t)) == 0);
  }
}

static void reverse_value_ids(WordInfoTable *wit) {
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    WitTrie *trie = &wit->tries[length];
    const size_t stride = (size_t)wit_stride_for_len(length);
    const size_t bytes = (size_t)trie->num_values * stride * sizeof(uint32_t);
    uint32_t *reordered = malloc_or_die(bytes);
    for (uint32_t value_id = 0; value_id < trie->num_values; value_id++) {
      memcpy(reordered + ((trie->num_values - value_id - 1) * stride),
             trie->values + (value_id * stride), stride * sizeof(uint32_t));
    }
    free(trie->values);
    trie->values = reordered;
    for (uint32_t node_idx = 0; node_idx < trie->num_nodes; node_idx++) {
      if (trie->node_value[node_idx] >= 0) {
        trie->node_value[node_idx] =
            (int32_t)trie->num_values - trie->node_value[node_idx] - 1;
      }
    }
  }
}

static void assert_roundtrip(const WordInfoTable *original,
                             ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, "wpf_maker_roundtrip",
      DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));
  word_info_table_write_to_file(original, filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  WordInfoTable *loaded = calloc_or_die(1, sizeof(WordInfoTable));
  word_info_table_load(loaded, "wpf_maker_roundtrip", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded->version == WIT_VERSION);
  assert_tables_equal(original, loaded);
  const int remove_status = remove(filename);
  assert(remove_status == 0);
  free(filename);
  word_info_table_destroy(loaded);
}

static void test_full_tables(void) {
  DictionaryWordList *words = make_tiny_words();
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  ErrorStack *error_stack = error_stack_create();
  make_word_plus_floater_from_kwg(kwg, wit, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_table_matches_reference(wit, words);
  assert_roundtrip(wit, error_stack);

  // Value IDs need not be in alphabetic order. Rebuilding also replaces an
  // existing positional table, and ZZ remains covered with an all-zero row.
  reverse_value_ids(wit);
  make_word_plus_floater_from_kwg(kwg, wit, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_table_matches_reference(wit, words);
  assert_roundtrip(wit, error_stack);

  word_info_table_destroy(wit);
  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  error_stack_destroy(error_stack);
}

static void test_no_covered_base_lengths(void) {
  DictionaryWordList *words = dictionary_word_list_create();
  add_literal(words, "A");
  add_literal(words, "AAAAA");
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  ErrorStack *error_stack = error_stack_create();
  make_word_plus_floater_from_kwg(kwg, wit, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    assert(wit->tries[length].num_values == 0);
    assert(wit->word_plus_floater[length] == NULL);
  }
  assert_roundtrip(wit, error_stack);
  word_info_table_destroy(wit);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
  error_stack_destroy(error_stack);
}

static void assert_uncovered(const WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(wit->word_plus_floater[length] == NULL);
  }
}

static void test_invalid_inputs(void) {
  DictionaryWordList *words = make_tiny_words();
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  ErrorStack *error_stack = error_stack_create();
  const uint64_t correct_hash = wit->kwg_hash;
  const uint64_t bad_hashes[] = {0, correct_hash ^ UINT64_C(1)};
  for (size_t hash_idx = 0;
       hash_idx < sizeof(bad_hashes) / sizeof(bad_hashes[0]); hash_idx++) {
    wit->kwg_hash = correct_hash;
    make_word_plus_floater_from_kwg(kwg, wit, error_stack);
    assert(error_stack_is_empty(error_stack));
    wit->kwg_hash = bad_hashes[hash_idx];
    make_word_plus_floater_from_kwg(kwg, wit, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_WIT_KWG_MISMATCH);
    assert_uncovered(wit);
    error_stack_reset(error_stack);
  }
  wit->kwg_hash = correct_hash;

  // A valid KWG can contain machine letters outside the supported WPF
  // alphabet. Leave the positional section absent before any bit operations.
  DictionaryWordList *unsupported_words = dictionary_word_list_create();
  static const MachineLetter unsupported[] = {1, 27};
  dictionary_word_list_add_word(unsupported_words, unsupported, 2);
  KWG *unsupported_kwg = make_kwg_from_words(
      unsupported_words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *unsupported_wit =
      make_word_info_table_from_kwg(unsupported_kwg);
  make_word_plus_floater_from_kwg(unsupported_kwg, unsupported_wit,
                                  error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_uncovered(unsupported_wit);
  assert_roundtrip(unsupported_wit, error_stack);
  error_stack_reset(error_stack);

  word_info_table_destroy(unsupported_wit);
  kwg_destroy(unsupported_kwg);
  dictionary_word_list_destroy(unsupported_words);
  word_info_table_destroy(wit);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
  error_stack_destroy(error_stack);
}

static void test_invalid_wit_layouts(void) {
  DictionaryWordList *words = make_tiny_words();
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  ErrorStack *error_stack = error_stack_create();
  for (int corruption = 0; corruption < BAD_TRIE_COUNT; corruption++) {
    WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
    make_word_plus_floater_from_kwg(kwg, wit, error_stack);
    assert(error_stack_is_empty(error_stack));
    WitTrie *trie = &wit->tries[2];
    uint32_t first_terminal = 0;
    uint32_t second_terminal = 0;
    for (uint32_t node_idx = 1; node_idx < trie->num_nodes; node_idx++) {
      if (trie->node_value[node_idx] >= 0) {
        if (first_terminal == 0) {
          first_terminal = node_idx;
        } else {
          second_terminal = node_idx;
          break;
        }
      }
    }
    assert(first_terminal != 0 && second_terminal != 0);
    switch (corruption) {
    case BAD_TRIE_ROOT:
      trie->root = trie->num_nodes;
      break;
    case BAD_TRIE_DUPLICATE_VALUE:
      trie->node_value[second_terminal] = trie->node_value[first_terminal];
      break;
    case BAD_TRIE_MISSING_VALUE:
      trie->node_value[first_terminal] = -1;
      break;
    case BAD_TRIE_CYCLE:
      trie->node_child[trie->root] = trie->root;
      break;
    case BAD_TRIE_LETTER:
      trie->node_tile[trie->root] = 27;
      break;
    case BAD_TRIE_LAST_FLAG:
      trie->node_last[trie->root] = 2;
      break;
    default:
      assert(0);
    }
    make_word_plus_floater_from_kwg(kwg, wit, error_stack);
    assert(error_stack_top(error_stack) ==
           ERROR_STATUS_CONVERT_INPUT_FILE_ERROR);
    assert_uncovered(wit);
    error_stack_reset(error_stack);
    word_info_table_destroy(wit);
  }

  // A copied graph fingerprint is insufficient: missing short or long
  // words and extra terminals must be rejected before constructing masks.
  for (int variant = 0; variant < 3; variant++) {
    DictionaryWordList *altered = dictionary_word_list_create();
    bool removed = false;
    for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
         word_idx++) {
      const DictionaryWord *word =
          dictionary_word_list_get_word(words, word_idx);
      const int length = dictionary_word_get_length(word);
      if (!removed && ((variant == 0 && length == 2) ||
                       (variant == 1 && length == BOARD_DIM))) {
        removed = true;
        continue;
      }
      dictionary_word_list_add_word(altered, dictionary_word_get_word(word),
                                    length);
    }
    if (variant == 2) {
      add_literal(altered, "ZA");
    }
    WordInfoTable *wit = make_word_info_table_from_words(altered);
    wit->kwg_hash = kwg_get_hash(kwg);
    make_word_plus_floater_from_kwg(kwg, wit, error_stack);
    assert(error_stack_top(error_stack) ==
           ERROR_STATUS_CONVERT_INPUT_FILE_ERROR);
    assert_uncovered(wit);
    error_stack_reset(error_stack);
    word_info_table_destroy(wit);
    dictionary_word_list_destroy(altered);
  }
  error_stack_destroy(error_stack);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
}

static void test_writer_preserves_existing_output(void) {
  DictionaryWordList *words = make_tiny_words();
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  WordInfoTable *wit = make_word_info_table_from_kwg(kwg);
  ErrorStack *error_stack = error_stack_create();
  make_word_plus_floater_from_kwg(kwg, wit, error_stack);
  assert(error_stack_is_empty(error_stack));
  char *filename = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, "wpf_maker_preserve",
      DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_is_empty(error_stack));

  uint32_t *const original_masks = wit->word_plus_floater[2];
  wit->word_plus_floater[2] = NULL;
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_WRITE_ERROR);
  error_stack_reset(error_stack);
  wit->word_plus_floater[2] = original_masks;
  const uint64_t original_hash = wit->kwg_hash;
  wit->kwg_hash = 0;
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_WRITE_ERROR);
  error_stack_reset(error_stack);
  wit->kwg_hash = original_hash;

  WordInfoTable *loaded = calloc_or_die(1, sizeof(WordInfoTable));
  word_info_table_load(loaded, "wpf_maker_preserve", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_tables_equal(wit, loaded);
  const int remove_status = remove(filename);
  assert(remove_status == 0);
  // An existing directory cannot be replaced by the atomic rename. The
  // writer must report the failure and leave that destination intact.
  const int made_directory = mkdir(filename, 0700);
  assert(made_directory == 0);
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_WRITE_ERROR);
  error_stack_reset(error_stack);
  const int removed_directory = rmdir(filename);
  assert(removed_directory == 0);
  free(filename);
  word_info_table_destroy(loaded);
  word_info_table_destroy(wit);
  kwg_destroy(kwg);
  dictionary_word_list_destroy(words);
  error_stack_destroy(error_stack);
}

void test_word_plus_floater_maker(void) {
  test_full_tables();
  test_no_covered_base_lengths();
  test_invalid_inputs();
  test_invalid_wit_layouts();
  test_writer_preserves_existing_output();
}

#include "word_info_table_test.h"

#include "../src/compat/endian_io.h"
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

// Brute-force reference: the union of letters outside each matching key
// occurrence in every word of the requested total length. This deliberately
// scans individual positions instead of using the maker's prefix/suffix masks.
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
    for (int start = 0; start + key_len <= wlen; start++) {
      if (memcmp(w + start, key, (size_t)key_len * sizeof(MachineLetter)) ==
          0) {
        for (int k = 0; k < wlen; k++) {
          if (k < start || k >= start + key_len) {
            result |= 1U << w[k];
          }
        }
      }
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
  assert(a->kwg_hash == b->kwg_hash);
  assert(a->version == b->version);
  for (int len = 1; len <= BOARD_DIM; len++) {
    assert_tries_equal(&a->tries[len], &b->tries[len], wit_stride_for_len(len));
  }
}

static void test_residual_letters_with_repeated_blocks(void) {
  static const MachineLetter a[] = {A};
  static const MachineLetter aa[] = {A, A};
  static const MachineLetter aaa[] = {A, A, A};
  static const MachineLetter ab[] = {A, B};
  static const MachineLetter aba[] = {A, B, A};
  static const MachineLetter ababa[] = {A, B, A, B, A};
  DictionaryWordList *words = dictionary_word_list_create();
  add_word(words, a, 1);
  add_word(words, aa, 2);
  add_word(words, aaa, 3);
  add_word(words, ab, 2);
  add_word(words, aba, 3);
  add_word(words, ababa, 5);
  WordInfoTable *wit = make_word_info_table_from_words(words);

  // A key's own letters provide no extra copy at its own word length.
  const uint32_t *ab_row = word_info_table_lookup(wit, ab, 2);
  assert(ab_row[0] == 0);
  // ABA contributes an extra A outside AB, but no extra B.
  assert(ab_row[1] == (1U << A));
  // AAA contains two overlapping AA occurrences. Each leaves another A.
  const uint32_t *aa_row = word_info_table_lookup(wit, aa, 2);
  assert(aa_row[1] == (1U << A));
  // AAA still leaves A when removing one A; ABABA leaves A and B after
  // removing either overlapping ABA occurrence.
  const uint32_t *a_row = word_info_table_lookup(wit, a, 1);
  assert((a_row[2] & (1U << A)) != 0);
  const uint32_t *aba_row = word_info_table_lookup(wit, aba, 3);
  assert(aba_row[2] == ((1U << A) | (1U << B)));

  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    assert_key_matches_reference(wit, words, dictionary_word_get_word(word),
                                 dictionary_word_get_length(word));
  }
  word_info_table_destroy(wit);
  dictionary_word_list_destroy(words);
}

static void write_fixture_bytes(const char *filename, const uint8_t *bytes,
                                size_t count) {
  FILE *stream = fopen_or_die(filename, "wb");
  if (count != 0) {
    fwrite_or_die(bytes, 1, count, stream, "WIT fixture");
  }
  fclose_or_die(stream);
}

static void assert_no_positional_rows(const WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(wit->word_plus_floater[length] == NULL);
  }
}

static void test_combined_wit_format(void) {
  DictionaryWordList *words = dictionary_word_list_create();
  static const MachineLetter at[] = {A, T};
  static const MachineLetter atat[] = {A, T, A, T};
  static const MachineLetter aa[] = {A, A};
  static const MachineLetter ta[] = {T, A};
  add_word(words, aa, 2);
  add_word(words, ta, 2);
  add_word(words, at, 2);
  add_word(words, atat, 4);
  WordInfoTable *wit = make_word_info_table_from_words(words);
  wit->kwg_hash = 1234567;
  size_t positional_bytes = 0;
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const size_t count =
        wit->tries[length].num_values * word_plus_floater_cells_per_key(length);
    positional_bytes += count * sizeof(uint32_t);
    if (count == 0) {
      continue;
    }
    wit->word_plus_floater[length] = calloc_or_die(count, sizeof(uint32_t));
    for (size_t cell = 0; cell < count; cell++) {
      wit->word_plus_floater[length][cell] = (uint32_t)cell;
    }
  }
  ErrorStack *error_stack = error_stack_create();
  char *filename = data_filepaths_get_writable_filename(
      DEFAULT_TEST_DATA_PATH, "wit_combined_format",
      DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  FILE *stream = fopen_or_die(filename, "rb");
  fseek_or_die(stream, 0, SEEK_END);
  const long file_size = ftell(stream);
  assert(file_size > 12 && (size_t)file_size > positional_bytes);
  const size_t size = (size_t)file_size;
  uint8_t *bytes = malloc_or_die(size + 1);
  fseek_or_die(stream, 0, SEEK_SET);
  const size_t read_size = fread(bytes, 1, size, stream);
  assert(read_size == size);
  fclose_or_die(stream);
  assert(bytes[0] == 4 && bytes[1] == BOARD_DIM);
  assert(bytes[2] == WIT_FLAG_WORD_PLUS_FLOATER && bytes[3] == 0);
  WordInfoTable *loaded = calloc_or_die(1, sizeof(WordInfoTable));
  word_info_table_load(loaded, "combined", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_wits_equal(wit, loaded);
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const size_t count =
        wit->tries[length].num_values * word_plus_floater_cells_per_key(length);
    if (count == 0) {
      assert(loaded->word_plus_floater[length] == NULL);
      continue;
    }
    assert(loaded->word_plus_floater[length] != NULL);
    assert(memcmp(wit->word_plus_floater[length],
                  loaded->word_plus_floater[length],
                  count * sizeof(uint32_t)) == 0);
  }

  // The ordinary prefix remains exactly v3-compatible. Reloading v3 clears
  // previously loaded positional rows, even when old sidecars still exist.
  const size_t ordinary_size = size - positional_bytes;
  bytes[0] = 3;
  bytes[2] = 0;
  write_fixture_bytes(filename, bytes, ordinary_size);
  word_info_table_load(loaded, "legacy", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded->version == 3 && loaded->kwg_hash == wit->kwg_hash);
  assert_no_positional_rows(loaded);
  for (int length = 1; length <= BOARD_DIM; length++) {
    assert_tries_equal(&wit->tries[length], &loaded->tries[length],
                       wit_stride_for_len(length));
  }
  bytes[0] = 4;
  write_fixture_bytes(filename, bytes, ordinary_size);
  word_info_table_load(loaded, "ordinary-v4", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded->version == 4);
  assert_no_positional_rows(loaded);
  bytes[2] = WIT_FLAG_WORD_PLUS_FLOATER;

  const size_t truncated_sizes[] = {0, 11, ordinary_size - 1, ordinary_size,
                                    size - 1};
  for (size_t index = 0;
       index < sizeof(truncated_sizes) / sizeof(truncated_sizes[0]); index++) {
    write_fixture_bytes(filename, bytes, truncated_sizes[index]);
    word_info_table_load(loaded, "truncated", filename, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
    assert_no_positional_rows(loaded);
    assert(loaded->tries[2].node_tile == NULL);
    error_stack_reset(error_stack);
  }
  static const struct {
    size_t offset;
    uint8_t value;
    error_code_t status;
  } corruptions[] = {
      {0, 5, ERROR_STATUS_WMP_UNSUPPORTED_VERSION},
      {1, 0, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM},
      {2, 2, ERROR_STATUS_RW_READ_ERROR},
      {3, 1, ERROR_STATUS_RW_READ_ERROR},
      {12, 0, ERROR_STATUS_RW_READ_ERROR},
      {16, 255, ERROR_STATUS_RW_READ_ERROR},
      {20, 255, ERROR_STATUS_RW_READ_ERROR},
  };
  for (size_t index = 0; index < sizeof(corruptions) / sizeof(corruptions[0]);
       index++) {
    const size_t offset = corruptions[index].offset;
    const uint8_t original = bytes[offset];
    bytes[offset] = corruptions[index].value;
    write_fixture_bytes(filename, bytes, size);
    word_info_table_load(loaded, "malformed", filename, error_stack);
    assert(error_stack_top(error_stack) == corruptions[index].status);
    assert_no_positional_rows(loaded);
    error_stack_reset(error_stack);
    bytes[offset] = original;
  }
  const WitTrie *first_trie = &wit->tries[1];
  const size_t section_offset =
      12 + 12 + ((size_t)first_trie->num_nodes * 10) +
      ((size_t)first_trie->num_values * wit_stride_for_len(1) * 4);
  const WitTrie *trie = &wit->tries[2];
  const size_t tiles_offset = section_offset + 12;
  const size_t last_offset = tiles_offset + trie->num_nodes;
  const size_t children_offset = last_offset + trie->num_nodes;
  const size_t values_offset = children_offset + ((size_t)trie->num_nodes * 4);
  uint32_t first_terminal = 0;
  uint32_t second_terminal = 0;
  for (uint32_t node = 1; node < trie->num_nodes; node++) {
    if (trie->node_value[node] >= 0) {
      if (first_terminal == 0) {
        first_terminal = node;
      } else {
        second_terminal = node;
        break;
      }
    }
  }
  assert(first_terminal != 0 && second_terminal != 0);
  assert(trie->node_last[trie->root] == 0);
  const struct {
    size_t offset;
    uint32_t value;
  } trie_corruptions[] = {
      {section_offset, UINT32_MAX},
      {children_offset + ((size_t)trie->root * 4), trie->root},
      {children_offset + ((size_t)(trie->root + 1) * 4),
       trie->node_child[trie->root]},
      {values_offset + ((size_t)second_terminal * 4),
       (uint32_t)trie->node_value[first_terminal]},
      {values_offset + ((size_t)trie->root * 4), 0},
  };
  for (size_t index = 0;
       index < sizeof(trie_corruptions) / sizeof(trie_corruptions[0]);
       index++) {
    const size_t offset = trie_corruptions[index].offset;
    uint8_t original[4];
    memcpy(original, bytes + offset, sizeof(original));
    for (size_t byte = 0; byte < sizeof(original); byte++) {
      bytes[offset + byte] =
          (uint8_t)(trie_corruptions[index].value >> (byte * 8));
    }
    write_fixture_bytes(filename, bytes, size);
    word_info_table_load(loaded, "malformed-trie", filename, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
    assert_no_positional_rows(loaded);
    assert(loaded->tries[2].node_tile == NULL);
    error_stack_reset(error_stack);
    memcpy(bytes + offset, original, sizeof(original));
  }
  const size_t invalid_byte_offsets[] = {tiles_offset + trie->root,
                                         last_offset + trie->root};
  const uint8_t invalid_byte_values[] = {MAX_ALPHABET_SIZE, 2};
  for (size_t index = 0;
       index < sizeof(invalid_byte_offsets) / sizeof(invalid_byte_offsets[0]);
       index++) {
    const size_t offset = invalid_byte_offsets[index];
    const uint8_t original = bytes[offset];
    bytes[offset] = invalid_byte_values[index];
    write_fixture_bytes(filename, bytes, size);
    word_info_table_load(loaded, "malformed-node", filename, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
    assert_no_positional_rows(loaded);
    error_stack_reset(error_stack);
    bytes[offset] = original;
  }
  uint8_t saved_fingerprint[8];
  memcpy(saved_fingerprint, bytes + 4, sizeof(saved_fingerprint));
  memset(bytes + 4, 0, sizeof(saved_fingerprint));
  write_fixture_bytes(filename, bytes, size);
  word_info_table_load(loaded, "unidentified-positional", filename,
                       error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
  assert_no_positional_rows(loaded);
  error_stack_reset(error_stack);
  memcpy(bytes + 4, saved_fingerprint, sizeof(saved_fingerprint));
  bytes[size] = 0;
  write_fixture_bytes(filename, bytes, size + 1);
  word_info_table_load(loaded, "trailing", filename, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
  error_stack_reset(error_stack);

  // A present all-zero row is still a loaded positional table.
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const size_t count =
        wit->tries[length].num_values * word_plus_floater_cells_per_key(length);
    if (count != 0) {
      memset(wit->word_plus_floater[length], 0, count * sizeof(uint32_t));
    }
  }
  word_info_table_write_to_file(wit, filename, error_stack);
  word_info_table_load(loaded, "zero", filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded->word_plus_floater[2] != NULL);
  assert(loaded->word_plus_floater[2][0] == 0);
  word_info_table_destroy(loaded);
  word_info_table_destroy(wit);
  dictionary_word_list_destroy(words);
  free(bytes);
  const int removed = remove(filename);
  assert(removed == 0);
  free(filename);
  error_stack_destroy(error_stack);
}

// Check the wire bytes independently of a writer/reader round trip so the
// same conversion mistake in both directions cannot cancel itself out.
static void test_little_endian_uint32_io(void) {
  const uint32_t values[] = {0, 1, 0x01234567U, 0x89abcdefU, UINT32_MAX};
  const unsigned char expected[] = {
      0,    0,    0,    0,    1,    0,    0,    0,    0x67, 0x45,
      0x23, 0x01, 0xef, 0xcd, 0xab, 0x89, 0xff, 0xff, 0xff, 0xff,
  };
  FILE *stream = tmpfile();
  assert(stream != NULL);
  const bool empty_write = fwrite_le_uint32s(NULL, 0, NULL);
  const bool empty_read = fread_le_uint32s(NULL, 0, NULL);
  assert(empty_write && empty_read);
  const bool written = fwrite_le_uint32s(values, 5, stream);
  assert(written);
  const int bytes_seek = fseek(stream, 0, SEEK_SET);
  assert(bytes_seek == 0);
  unsigned char bytes[sizeof(expected)];
  const size_t bytes_read = fread(bytes, 1, sizeof(bytes), stream);
  assert(bytes_read == sizeof(bytes));
  assert(memcmp(bytes, expected, sizeof(expected)) == 0);
  const int values_seek = fseek(stream, 0, SEEK_SET);
  assert(values_seek == 0);
  uint32_t actual[6] = {0};
  const bool read_success = fread_le_uint32s(actual, 5, stream);
  assert(read_success);
  assert(memcmp(actual, values, sizeof(values)) == 0);
  const int short_read_seek = fseek(stream, 0, SEEK_SET);
  assert(short_read_seek == 0);
  const bool short_read = fread_le_uint32s(actual, 6, stream);
  assert(!short_read);
  const int closed = fclose(stream);
  assert(closed == 0);
}

void test_word_info_table(void) {
  test_little_endian_uint32_io();
  test_combined_wit_format();
  test_residual_letters_with_repeated_blocks();
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

  // No new tile is added when the containing word equals the block.
  const int num_words = dictionary_word_list_get_count(words);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *dw = dictionary_word_list_get_word(words, word_idx);
    const MachineLetter *w = dictionary_word_get_word(dw);
    const int wlen = dictionary_word_get_length(dw);
    const uint32_t *row = word_info_table_lookup(wit, w, wlen);
    assert(row != NULL);
    assert(row[0] == 0); // own length is row entry 0
    assert_key_matches_reference(wit, words, w, wlen);
  }

  // Spot-check a multi-word substring relationship by hand. "AT" appears in
  // AT, CAT, CATS, RAT.
  {
    const uint32_t *row = word_info_table_lookup(wit, at, 2);
    assert(row != NULL);
    // Entry i = total length (2 + i).
    assert(row[0] == 0);                       // AT adds no letters
    assert(row[1] == ((1U << C) | (1U << R))); // CAT or RAT
    assert(row[2] == ((1U << C) | (1U << S))); // CATS
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
    assert(row[1] == ((1U << C) | (1U << R)));
    static const MachineLetter ca[] = {C, A};
    assert(word_info_table_lookup(wit_loaded, ca, 2) == NULL);
  }

  word_info_table_destroy(wit_loaded);
  free(wit_filename);
  error_stack_destroy(error_stack);
  word_info_table_destroy(wit);
  dictionary_word_list_destroy(words);
}

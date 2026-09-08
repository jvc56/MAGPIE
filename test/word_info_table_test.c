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

static void write_word_plus_floater_fixture(FILE *stream,
                                            const WordInfoTable *wit,
                                            bool sparse, int maximum_length,
                                            bool empty) {
  const unsigned char magic[8] = {'W', 'P', 'F', 'M', sparse ? '2' : '1',
                                  'L', 'E', 0};
  fwrite_or_die(magic, sizeof(magic), 1, stream, "fixture magic");
  wit_write_uint32_or_die(BOARD_DIM, stream, "dimension");
  wit_write_uint32_or_die(2, stream, "minimum length");
  wit_write_uint32_or_die((uint32_t)maximum_length, stream, "maximum length");
  wit_write_uint32_or_die(0, stream, "reserved");
  wit_write_uint32_or_die((uint32_t)wit->kwg_hash, stream, "KWG hash low");
  wit_write_uint32_or_die((uint32_t)(wit->kwg_hash >> 32), stream,
                          "KWG hash high");
  const uint64_t layout_hash = word_plus_floater_layout_hash(wit);
  wit_write_uint32_or_die((uint32_t)layout_hash, stream, "layout hash low");
  wit_write_uint32_or_die((uint32_t)(layout_hash >> 32), stream,
                          "layout hash high");
  for (int length = 2; length <= maximum_length; length++) {
    const uint32_t count = wit->tries[length].num_values;
    const uint32_t stored_count = empty ? 0 : 1;
    const uint32_t cells = (uint32_t)word_plus_floater_cells_per_key(length);
    wit_write_uint32_or_die((uint32_t)length, stream, "key length");
    wit_write_uint32_or_die(count, stream, "key count");
    wit_write_uint32_or_die(cells, stream, "cell count");
    wit_write_uint32_or_die(sparse ? stored_count : 0, stream, "stored count");
    if (sparse) {
      for (uint32_t index = 0; index < count; index++) {
        wit_write_uint32_or_die(!empty && index == count - 1 ? 0 : UINT32_MAX,
                                stream, "row ID");
      }
    }
    const size_t total = (size_t)(sparse ? stored_count : count) * cells;
    for (size_t index = 0; index < total; index++) {
      wit_write_uint32_or_die((uint32_t)(index + length), stream, "mask");
    }
  }
  fseek_or_die(stream, 0, SEEK_SET);
}

static void test_word_plus_floater_subset_loading(void) {
  ErrorStack *error_stack = error_stack_create();
  // Legacy full M1, reduced dense M1, sparse M2 and an entirely uncovered M2.
  for (int variant = 0; variant < 4; variant++) {
    WordInfoTable *wit = calloc(1, sizeof(WordInfoTable));
    assert(wit != NULL);
    wit->kwg_hash = 1234567;
    wit->tries[2].num_values = 3;
    wit->tries[3].num_values = 2;
    wit->tries[4].num_values = 1;
    const bool sparse = variant >= 2;
    const bool empty = variant == 3;
    const int maximum_length = variant == 1 ? 3 : 4;
    FILE *stream = tmpfile();
    assert(stream != NULL);
    write_word_plus_floater_fixture(stream, wit, sparse, maximum_length, empty);
    word_info_table_read_word_plus_floater(wit, stream, error_stack);
    assert(error_stack_is_empty(error_stack));
    fclose_or_die(stream);
    for (int length = 2; length <= 4; length++) {
      if (length > maximum_length) {
        assert(wit->word_plus_floater[length] == NULL);
        assert(wit->word_plus_floater_ids[length] == NULL);
        continue;
      }
      if (sparse) {
        const int32_t *ids = wit->word_plus_floater_ids[length];
        assert(ids != NULL);
        for (uint32_t index = 0; index < wit->tries[length].num_values;
             index++) {
          assert(
              ids[index] ==
              (!empty && index == wit->tries[length].num_values - 1 ? 0 : -1));
        }
      } else {
        assert(wit->word_plus_floater_ids[length] == NULL);
      }
      if (empty) {
        assert(wit->word_plus_floater[length] == NULL);
      } else {
        const size_t count = (sparse ? 1 : wit->tries[length].num_values) *
                             word_plus_floater_cells_per_key(length);
        const uint32_t *values = wit->word_plus_floater[length];
        assert(values != NULL);
        assert(values[0] == (uint32_t)length);
        assert(values[count - 1] == count - 1 + length);
      }
    }
    word_info_table_destroy(wit);
  }
  // A sidecar from a different lexicon leaves all lengths uncovered.
  WordInfoTable wit = {0};
  wit.kwg_hash = 1234567;
  wit.tries[2].num_values = 3;
  wit.tries[3].num_values = 2;
  wit.tries[4].num_values = 1;
  FILE *stream = tmpfile();
  assert(stream != NULL);
  write_word_plus_floater_fixture(stream, &wit, true, 4, false);
  wit.kwg_hash++;
  word_info_table_read_word_plus_floater(&wit, stream, error_stack);
  assert(error_stack_is_empty(error_stack));
  fclose_or_die(stream);
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(wit.word_plus_floater[length] == NULL);
    assert(wit.word_plus_floater_ids[length] == NULL);
  }
  error_stack_destroy(error_stack);
}

static void assert_word_plus_floater_uncovered(const WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    assert(wit->word_plus_floater[length] == NULL);
    assert(wit->word_plus_floater_ids[length] == NULL);
  }
}

static void test_word_plus_floater_invalid_files(void) {
  // Offsets are from the documented little-endian file format. Each sparse
  // fixture stores one row, mapping original ID 2 to compact ID 0 at length 2.
  static const struct {
    long offset;
    uint32_t value;
  } corruptions[] = {
      {0, 0},               // magic
      {12, 1},              // minimum key length
      {16, 5},              // maximum key length
      {20, 1},              // reserved header field
      {32, 0},              // key-layout fingerprint
      {40, 3},              // section key length
      {44, 4},              // original value count
      {48, 1},              // cells per key
      {52, 4},              // more stored rows than original rows
      {56, UINT32_MAX - 1}, // invalid negative ID
      {64, 1},              // out-of-range compact ID
      {56, 0},              // duplicate compact ID 0
      {64, UINT32_MAX},     // stored row has no original ID
  };
  WordInfoTable wit = {0};
  wit.kwg_hash = 1234567;
  wit.tries[2].num_values = 3;
  wit.tries[3].num_values = 2;
  wit.tries[4].num_values = 1;
  ErrorStack *error_stack = error_stack_create();
  for (size_t index = 0; index < sizeof(corruptions) / sizeof(corruptions[0]);
       index++) {
    FILE *stream = tmpfile();
    assert(stream != NULL);
    write_word_plus_floater_fixture(stream, &wit, true, 4, false);
    fseek_or_die(stream, corruptions[index].offset, SEEK_SET);
    wit_write_uint32_or_die(corruptions[index].value, stream, "corruption");
    fseek_or_die(stream, 0, SEEK_SET);
    word_info_table_read_word_plus_floater(&wit, stream, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
    assert_word_plus_floater_uncovered(&wit);
    error_stack_reset(error_stack);
    fclose_or_die(stream);
  }
  // Truncate at the magic, header, section, row map and final mask, including
  // failure after preceding lengths have allocated and loaded successfully.
  FILE *complete = tmpfile();
  assert(complete != NULL);
  write_word_plus_floater_fixture(complete, &wit, true, 4, false);
  fseek_or_die(complete, 0, SEEK_END);
  const long file_size = ftell(complete);
  assert(file_size > 68);
  const size_t truncations[] = {7, 39, 55, 67, (size_t)file_size - 1};
  unsigned char *bytes = malloc((size_t)file_size);
  assert(bytes != NULL);
  fseek_or_die(complete, 0, SEEK_SET);
  const size_t bytes_read = fread(bytes, 1, (size_t)file_size, complete);
  assert(bytes_read == (size_t)file_size);
  fclose_or_die(complete);
  for (size_t index = 0; index < sizeof(truncations) / sizeof(truncations[0]);
       index++) {
    FILE *stream = tmpfile();
    assert(stream != NULL);
    fwrite_or_die(bytes, 1, truncations[index], stream, "truncated fixture");
    fseek_or_die(stream, 0, SEEK_SET);
    word_info_table_read_word_plus_floater(&wit, stream, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
    assert_word_plus_floater_uncovered(&wit);
    error_stack_reset(error_stack);
    fclose_or_die(stream);
  }
  free(bytes);
  FILE *stream = tmpfile();
  assert(stream != NULL);
  write_word_plus_floater_fixture(stream, &wit, true, 4, false);
  fseek_or_die(stream, 0, SEEK_END);
  const int appended_byte = fputc(0, stream);
  assert(appended_byte == 0);
  fseek_or_die(stream, 0, SEEK_SET);
  word_info_table_read_word_plus_floater(&wit, stream, error_stack);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
  assert_word_plus_floater_uncovered(&wit);
  error_stack_reset(error_stack);
  fclose_or_die(stream);

  // Loading a different dimension or lexicon over an existing valid table
  // must clear its coverage. Reloading a reduced table clears omitted lengths.
  stream = tmpfile();
  assert(stream != NULL);
  write_word_plus_floater_fixture(stream, &wit, true, 4, false);
  word_info_table_read_word_plus_floater(&wit, stream, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wit.word_plus_floater[4] != NULL);
  fclose_or_die(stream);
  stream = tmpfile();
  assert(stream != NULL);
  write_word_plus_floater_fixture(stream, &wit, false, 3, false);
  word_info_table_read_word_plus_floater(&wit, stream, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wit.word_plus_floater[2] != NULL);
  assert(wit.word_plus_floater_ids[2] == NULL);
  assert(wit.word_plus_floater[4] == NULL);
  fclose_or_die(stream);
  for (int mismatch = 0; mismatch < 2; mismatch++) {
    stream = tmpfile();
    assert(stream != NULL);
    write_word_plus_floater_fixture(stream, &wit, true, 4, false);
    word_info_table_read_word_plus_floater(&wit, stream, error_stack);
    assert(wit.word_plus_floater[2] != NULL);
    fseek_or_die(stream, mismatch == 0 ? 8 : 24, SEEK_SET);
    wit_write_uint32_or_die(mismatch == 0 ? BOARD_DIM + 1 : 0, stream,
                            "foreign identity");
    fseek_or_die(stream, 0, SEEK_SET);
    word_info_table_read_word_plus_floater(&wit, stream, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert_word_plus_floater_uncovered(&wit);
    fclose_or_die(stream);
  }
  error_stack_destroy(error_stack);
}

static void write_word_plus_floater_file(const WordInfoTable *wit,
                                         const char *filename, bool empty,
                                         ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(stream != NULL);
  write_word_plus_floater_fixture(stream, wit, true, 4, empty);
  fclose_or_die(stream);
}

static void test_word_plus_floater_lexicon_loading(void) {
  static const MachineLetter at[] = {A, T};
  static const MachineLetter cat[] = {C, A, T};
  static const MachineLetter cats[] = {C, A, T, S};
  DictionaryWordList *words = dictionary_word_list_create();
  add_word(words, at, 2);
  add_word(words, cat, 3);
  add_word(words, cats, 4);
  WordInfoTable *original = make_word_info_table_from_words(words);
  original->kwg_hash = 1234567;
  ErrorStack *error_stack = error_stack_create();
  const char *names[] = {"wit_test_wpf_alpha", "wit_test_wpf_beta"};
  char *wit_paths[2];
  char *wpf_paths[2];
  for (int index = 0; index < 2; index++) {
    wit_paths[index] = data_filepaths_get_writable_filename(
        DEFAULT_TEST_DATA_PATH, names[index],
        DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
    wpf_paths[index] = data_filepaths_get_writable_filename(
        DEFAULT_TEST_DATA_PATH, names[index],
        DATA_FILEPATH_TYPE_WORD_PLUS_FLOATER, error_stack);
    assert(error_stack_is_empty(error_stack));
    word_info_table_write_to_file(original, wit_paths[index], error_stack);
    assert(error_stack_is_empty(error_stack));
    original->kwg_hash++;
  }
  // No optional file: ordinary WIT loading is unchanged. Search continues
  // through the configured data paths to the actual WIT and WPF directory.
  char *data_paths =
      get_formatted_string("missing_wpf_data:%s", DEFAULT_TEST_DATA_PATH);
  WordInfoTable *alpha =
      word_info_table_create(data_paths, names[0], error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(alpha != NULL);
  assert_word_plus_floater_uncovered(alpha);
  write_word_plus_floater_file(alpha, wpf_paths[0], false, error_stack);
  // The beta filename deliberately contains alpha's identity initially.
  write_word_plus_floater_file(alpha, wpf_paths[1], false, error_stack);
  WordInfoTable *beta =
      word_info_table_create(data_paths, names[1], error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(beta != NULL);
  assert_word_plus_floater_uncovered(beta);
  write_word_plus_floater_file(beta, wpf_paths[1], true, error_stack);
  word_info_table_destroy(alpha);
  word_info_table_destroy(beta);
  alpha = word_info_table_create(data_paths, names[0], error_stack);
  beta = word_info_table_create(data_paths, names[1], error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(alpha != NULL && beta != NULL);
  assert(alpha->kwg_hash != beta->kwg_hash);
  assert(alpha->word_plus_floater[2] != NULL);
  assert(alpha->word_plus_floater[2][0] == 2);
  assert(beta->word_plus_floater[2] == NULL);
  assert(beta->word_plus_floater_ids[2][0] == -1);
  word_info_table_destroy(beta);
  assert(alpha->word_plus_floater[2][0] == 2);
  word_info_table_destroy(alpha);

  // A matching malformed optional file reports a load error, not a silently
  // disabled optimization, and destroys the partially loaded WIT.
  FILE *stream = fopen_safe(wpf_paths[0], "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(stream != NULL);
  const int written_byte = fputc('W', stream);
  assert(written_byte == 'W');
  fclose_or_die(stream);
  alpha = word_info_table_create(data_paths, names[0], error_stack);
  assert(alpha == NULL);
  assert(error_stack_top(error_stack) == ERROR_STATUS_RW_READ_ERROR);
  error_stack_reset(error_stack);
  for (int index = 0; index < 2; index++) {
    const int wit_remove_status = remove(wit_paths[index]);
    const int wpf_remove_status = remove(wpf_paths[index]);
    assert(wit_remove_status == 0);
    assert(wpf_remove_status == 0);
    free(wit_paths[index]);
    free(wpf_paths[index]);
  }
  free(data_paths);
  error_stack_destroy(error_stack);
  word_info_table_destroy(original);
  dictionary_word_list_destroy(words);
}

void test_word_info_table(void) {
  test_word_plus_floater_subset_loading();
  test_word_plus_floater_invalid_files();
  test_word_plus_floater_lexicon_loading();
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

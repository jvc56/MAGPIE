#include "wit_upgrade_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/conversion_results.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/kwg.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/config.h"
#include "../src/impl/convert.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/impl/word_plus_floater_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

typedef struct WitUpgradeFixture {
  char *root;
  char *first;
  char *later;
  char *first_lexica;
  char *later_lexica;
  char *paths;
  char *kwg_filename;
  char *first_wit;
  char *later_wit;
  ConversionArgs args;
  ConversionResults *results;
} WitUpgradeFixture;

typedef struct WitFileSnapshot {
  uint8_t *bytes;
  size_t size;
  struct stat metadata;
} WitFileSnapshot;

static void make_directory(const char *path) {
  const int status = mkdir(path, 0700);
  assert(status == 0);
}

static void remove_directory(const char *path) {
  const int status = rmdir(path);
  assert(status == 0);
}

static void remove_if_present(const char *path) {
  const int status = remove(path);
  assert(status == 0 || errno == ENOENT);
}

static void write_bytes(const char *path, const uint8_t *bytes, size_t size) {
  FILE *stream = fopen_or_die(path, "wb");
  if (size != 0) {
    fwrite_or_die(bytes, 1, size, stream, "WIT upgrade fixture");
  }
  fclose_or_die(stream);
}

static WitFileSnapshot snapshot_file(const char *path) {
  WitFileSnapshot snapshot = {0};
  const int status = stat(path, &snapshot.metadata);
  assert(status == 0 && snapshot.metadata.st_size > 0);
  snapshot.size = (size_t)snapshot.metadata.st_size;
  snapshot.bytes = malloc_or_die(snapshot.size);
  FILE *stream = fopen_or_die(path, "rb");
  const size_t read_count = fread(snapshot.bytes, 1, snapshot.size, stream);
  assert(read_count == snapshot.size);
  fclose_or_die(stream);
  return snapshot;
}

static void assert_file_matches(const char *path,
                                const WitFileSnapshot *expected,
                                bool unchanged) {
  WitFileSnapshot actual = snapshot_file(path);
  assert(actual.size == expected->size);
  assert(memcmp(actual.bytes, expected->bytes, actual.size) == 0);
  if (unchanged) {
    assert(actual.metadata.st_dev == expected->metadata.st_dev);
    assert(actual.metadata.st_ino == expected->metadata.st_ino);
    assert(actual.metadata.st_mtime == expected->metadata.st_mtime);
    assert(actual.metadata.st_mode == expected->metadata.st_mode);
  }
  free(actual.bytes);
}

static void run_upgrade(WitUpgradeFixture *fixture, error_code_t expected) {
  ErrorStack *error_stack = error_stack_create();
  convert(&fixture->args, fixture->results, error_stack);
  const error_code_t actual = error_stack_top(error_stack);
  if (actual != expected) {
    error_stack_print_and_reset(error_stack);
    log_fatal("WIT upgrade: expected status %d, got %d", expected, actual);
  }
  error_stack_destroy(error_stack);
}

static void write_kwg(const WitUpgradeFixture *fixture, const KWG *kwg) {
  ErrorStack *error_stack = error_stack_create();
  kwg_write_to_file(kwg, fixture->kwg_filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
}

static WitUpgradeFixture make_fixture(const KWG *kwg, const char *ld_name) {
  char template[] = "/tmp/magpie_wit_upgrade_XXXXXX";
  char *root = mkdtemp(template);
  assert(root != NULL);
  WitUpgradeFixture fixture = {0};
  fixture.root = string_duplicate(root);
  fixture.first = get_formatted_string("%s/first", root);
  fixture.later = get_formatted_string("%s/later", root);
  fixture.first_lexica = get_formatted_string("%s/lexica", fixture.first);
  fixture.later_lexica = get_formatted_string("%s/lexica", fixture.later);
  fixture.paths = get_formatted_string("%s:%s:%s", fixture.first, fixture.later,
                                       DEFAULT_TEST_DATA_PATH);
  fixture.kwg_filename =
      get_formatted_string("%s/upgrade.kwg", fixture.later_lexica);
  fixture.first_wit =
      get_formatted_string("%s/upgrade.wit", fixture.first_lexica);
  fixture.later_wit =
      get_formatted_string("%s/upgrade.wit", fixture.later_lexica);
  make_directory(fixture.first);
  make_directory(fixture.later);
  make_directory(fixture.first_lexica);
  make_directory(fixture.later_lexica);
  fixture.args = (ConversionArgs){
      .conversion_type_string = "kwg2witifneeded",
      .data_paths = fixture.paths,
      .input_and_output_name = "upgrade",
      .ld_name = ld_name,
  };
  fixture.results = conversion_results_create();
  write_kwg(&fixture, kwg);
  return fixture;
}

static void destroy_fixture(WitUpgradeFixture *fixture) {
  remove_if_present(fixture->first_wit);
  remove_if_present(fixture->later_wit);
  remove_if_present(fixture->kwg_filename);
  // These removals also detect leaked atomic-writer temporary files.
  remove_directory(fixture->first_lexica);
  remove_directory(fixture->later_lexica);
  remove_directory(fixture->first);
  remove_directory(fixture->later);
  remove_directory(fixture->root);
  free(fixture->root);
  free(fixture->first);
  free(fixture->later);
  free(fixture->first_lexica);
  free(fixture->later_lexica);
  free(fixture->paths);
  free(fixture->kwg_filename);
  free(fixture->first_wit);
  free(fixture->later_wit);
  conversion_results_destroy(fixture->results);
}

static KWG *make_literal_kwg(const char *const *literals, size_t count) {
  DictionaryWordList *words = dictionary_word_list_create();
  for (size_t word_idx = 0; word_idx < count; word_idx++) {
    const size_t length = strlen(literals[word_idx]);
    assert(length <= BOARD_DIM);
    MachineLetter letters[BOARD_DIM] = {0};
    for (size_t position = 0; position < length; position++) {
      letters[position] =
          (MachineLetter)(literals[word_idx][position] - 'A' + 1);
    }
    dictionary_word_list_add_word(words, letters, (int)length);
  }
  dictionary_word_list_sort(words);
  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(words);
  return kwg;
}

static WordInfoTable *load_current(const WitUpgradeFixture *fixture,
                                   const KWG *kwg) {
  ErrorStack *error_stack = error_stack_create();
  WordInfoTable *wit =
      word_info_table_create(fixture->paths, "upgrade", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(wit != NULL && wit->version == WIT_VERSION);
  assert(wit->kwg_hash != 0 && wit->kwg_hash == kwg_get_hash(kwg));
  error_stack_destroy(error_stack);
  return wit;
}

static void write_wit_with_copied_identity(const char *filename,
                                           const KWG *contents,
                                           uint64_t copied_identity) {
  ErrorStack *error_stack = error_stack_create();
  WordInfoTable *wit = make_word_info_table_from_kwg(contents);
  make_word_plus_floater_from_kwg(contents, wit, error_stack);
  assert(error_stack_is_empty(error_stack));
  wit->kwg_hash = copied_identity;
  word_info_table_write_to_file(wit, filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  word_info_table_destroy(wit);
  error_stack_destroy(error_stack);
}

static void assert_read_only_reuse(WitUpgradeFixture *fixture,
                                   const char *filename) {
  const struct utimbuf old_times = {946684800, 946684800};
  int status = utime(filename, &old_times);
  assert(status == 0);
  status = chmod(filename, 0444);
  assert(status == 0);
  WitFileSnapshot before = snapshot_file(filename);
  run_upgrade(fixture, ERROR_STATUS_SUCCESS);
  assert_file_matches(filename, &before, true);
  free(before.bytes);
  status = chmod(filename, 0644);
  assert(status == 0);
}

static void test_upgrade_and_repair(void) {
  static const char *const words[] = {"AT",   "CAT",  "ATAT",
                                      "TATA", "CATS", "ABCDE"};
  KWG *kwg = make_literal_kwg(words, sizeof(words) / sizeof(words[0]));
  WitUpgradeFixture fixture = make_fixture(kwg, "english");

  // Missing output: the conditional command constructs the same complete
  // native table as the existing force conversion.
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  WordInfoTable *wit = load_current(&fixture, kwg);
  size_t positional_bytes = 0;
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    assert(wit->tries[length].num_values != 0);
    assert(wit->word_plus_floater[length] != NULL);
    positional_bytes += wit->tries[length].num_values *
                        word_plus_floater_cells_per_key(length) *
                        sizeof(uint32_t);
  }
  word_info_table_destroy(wit);
  WitFileSnapshot current = snapshot_file(fixture.first_wit);
  assert(current.bytes[2] == WIT_FLAG_WORD_PLUS_FLOATER);
  assert(current.size > positional_bytes);
  const size_t ordinary_size = current.size - positional_bytes;
  assert_read_only_reuse(&fixture, fixture.first_wit);
  WitFileSnapshot before_force = snapshot_file(fixture.first_wit);
  fixture.args.conversion_type_string = "kwg2wit";
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  fixture.args.conversion_type_string = "kwg2witifneeded";
  assert_file_matches(fixture.first_wit, &current, false);
  WitFileSnapshot after_force = snapshot_file(fixture.first_wit);
  assert(after_force.metadata.st_ino != before_force.metadata.st_ino);
  free(before_force.bytes);
  free(after_force.bytes);

  uint8_t *damaged = malloc_or_die(current.size + 1);
  // Both a valid v3 and an ordinary-only v4 need the positional payload.
  for (int version = WIT_EARLIEST_SUPPORTED_VERSION; version <= WIT_VERSION;
       version++) {
    memcpy(damaged, current.bytes, current.size);
    damaged[0] = (uint8_t)version;
    damaged[2] = 0;
    write_bytes(fixture.first_wit, damaged, ordinary_size);
    run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
    assert_file_matches(fixture.first_wit, &current, false);
  }
  const size_t truncated_sizes[] = {0, 11, ordinary_size - 1, ordinary_size,
                                    current.size - 1};
  for (size_t index = 0;
       index < sizeof(truncated_sizes) / sizeof(truncated_sizes[0]); index++) {
    write_bytes(fixture.first_wit, current.bytes, truncated_sizes[index]);
    run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
    assert_file_matches(fixture.first_wit, &current, false);
  }
  // Known-version malformed flags, dimension, identity, trie and trailing
  // bytes must be repaired even though the first header byte looks current.
  static const size_t corrupt_offsets[] = {1, 2, 3, 4, 16};
  for (size_t index = 0;
       index < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); index++) {
    memcpy(damaged, current.bytes, current.size);
    damaged[corrupt_offsets[index]] ^= 0x80;
    write_bytes(fixture.first_wit, damaged, current.size);
    run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
    assert_file_matches(fixture.first_wit, &current, false);
  }
  memcpy(damaged, current.bytes, current.size);
  memset(damaged + 4, 0, 8);
  write_bytes(fixture.first_wit, damaged, current.size);
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  assert_file_matches(fixture.first_wit, &current, false);
  memcpy(damaged, current.bytes, current.size);
  damaged[current.size] = 1;
  write_bytes(fixture.first_wit, damaged, current.size + 1);
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  assert_file_matches(fixture.first_wit, &current, false);

  // A copied matching fingerprint cannot make an incomplete terminal set
  // current. Include missing all indexed bases, an extra word, and a same-
  // count substitution that requires lookup rather than count comparison.
  static const char *const incomplete_words[][7] = {
      {"ABCDE", NULL},
      {"AT", "CAT", "ATAT", "TATA", "CATS", "ABCDE", "SCAT"},
      {"AT", "CAT", "ATAT", "TATA", "SCAT", "ABCDE", NULL},
  };
  static const size_t incomplete_counts[] = {1, 7, 6};
  for (size_t index = 0;
       index < sizeof(incomplete_counts) / sizeof(incomplete_counts[0]);
       index++) {
    KWG *incomplete =
        make_literal_kwg(incomplete_words[index], incomplete_counts[index]);
    write_wit_with_copied_identity(fixture.first_wit, incomplete,
                                   kwg_get_hash(kwg));
    run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
    assert_file_matches(fixture.first_wit, &current, false);
    kwg_destroy(incomplete);
  }

  // A real dictionary update under the same basename must replace a valid
  // table, not merely reject obviously damaged fingerprint bytes.
  static const char *const changed_words[] = {"AT", "CAT", "CATS", "SCAT"};
  KWG *changed = make_literal_kwg(changed_words, sizeof(changed_words) /
                                                     sizeof(changed_words[0]));
  assert(kwg_get_hash(changed) != kwg_get_hash(kwg));
  write_kwg(&fixture, changed);
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  wit = load_current(&fixture, changed);
  assert(wit->tries[4].num_values == 2);
  word_info_table_destroy(wit);
  kwg_destroy(changed);
  write_kwg(&fixture, kwg);

  // The config parser must expose the new native conversion too.
  char *command = get_formatted_string(
      "set -lex CSW21 -wmp false -wit false -path %s", fixture.paths);
  Config *config = config_create_or_die(command);
  load_and_exec_config_or_die(config,
                              "convert kwg2witifneeded upgrade english");
  assert_file_matches(fixture.first_wit, &current, false);
  config_destroy(config);
  free(command);

  // Preserve unknown future formats; force conversion remains an explicit
  // way to replace them. The automatic command must never downgrade one.
  memcpy(damaged, current.bytes, current.size);
  damaged[0] = WIT_VERSION + 1;
  write_bytes(fixture.first_wit, damaged, current.size);
  WitFileSnapshot future = snapshot_file(fixture.first_wit);
  run_upgrade(&fixture, ERROR_STATUS_WMP_UNSUPPORTED_VERSION);
  assert_file_matches(fixture.first_wit, &future, true);
  fixture.args.conversion_type_string = "kwg2wit";
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  assert_file_matches(fixture.first_wit, &current, false);
  free(future.bytes);
  free(damaged);
  free(current.bytes);
  destroy_fixture(&fixture);
  kwg_destroy(kwg);
}

static void test_upgrade_data_path_precedence(void) {
  static const char *const words[] = {"AT", "CAT"};
  KWG *kwg = make_literal_kwg(words, sizeof(words) / sizeof(words[0]));
  WitUpgradeFixture fixture = make_fixture(kwg, "english");
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  WordInfoTable *wit = load_current(&fixture, kwg);
  const size_t positional_bytes = (word_plus_floater_cells_per_key(2) +
                                   word_plus_floater_cells_per_key(3)) *
                                  sizeof(uint32_t);
  assert(wit->tries[2].num_values == 1 && wit->tries[3].num_values == 1);
  word_info_table_destroy(wit);
  WitFileSnapshot current = snapshot_file(fixture.first_wit);
  const int moved = rename(fixture.first_wit, fixture.later_wit);
  assert(moved == 0);
  remove_directory(fixture.first_lexica);

  // A current later-path file can be reused without a writable output
  // directory. Do not accidentally force a first-path permission check.
  assert_read_only_reuse(&fixture, fixture.later_wit);
  assert(access(fixture.first_wit, F_OK) != 0);

  // A future-version table in a later path must not be silently hidden by
  // a generated first-path override, even when that output path is absent.
  current.bytes[0] = WIT_VERSION + 1;
  write_bytes(fixture.later_wit, current.bytes, current.size);
  WitFileSnapshot future = snapshot_file(fixture.later_wit);
  run_upgrade(&fixture, ERROR_STATUS_WMP_UNSUPPORTED_VERSION);
  assert_file_matches(fixture.later_wit, &future, true);
  assert(access(fixture.first_wit, F_OK) != 0);
  free(future.bytes);

  current.bytes[0] = WIT_EARLIEST_SUPPORTED_VERSION;
  current.bytes[2] = 0;
  write_bytes(fixture.later_wit, current.bytes,
              current.size - positional_bytes);
  WitFileSnapshot legacy = snapshot_file(fixture.later_wit);
  run_upgrade(&fixture, ERROR_STATUS_FILEPATH_FILE_NOT_WRITABLE);
  assert_file_matches(fixture.later_wit, &legacy, true);
  assert(access(fixture.first_wit, F_OK) != 0);

  // Once the first directory exists, create its override and retain the
  // original v3 file in the later data path byte-for-byte.
  make_directory(fixture.first_lexica);
  run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
  current.bytes[0] = WIT_VERSION;
  current.bytes[2] = WIT_FLAG_WORD_PLUS_FLOATER;
  assert_file_matches(fixture.first_wit, &current, false);
  assert_file_matches(fixture.later_wit, &legacy, true);
  free(legacy.bytes);
  free(current.bytes);
  destroy_fixture(&fixture);
  kwg_destroy(kwg);
}

static void test_upgrade_ordinary_only_reuse(void) {
  static const char *const long_words[] = {"ABCDE", "ABCDEF"};
  KWG *long_kwg =
      make_literal_kwg(long_words, sizeof(long_words) / sizeof(long_words[0]));
  DictionaryWordList *words = dictionary_word_list_create();
  static const MachineLetter extended[] = {1, 27};
  dictionary_word_list_add_word(words, extended, 2);
  KWG *extended_kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(words);
  const KWG *const kwgs[] = {long_kwg, extended_kwg};
  const char *const distributions[] = {"english", "polish"};
  for (size_t index = 0; index < sizeof(kwgs) / sizeof(kwgs[0]); index++) {
    WitUpgradeFixture fixture = make_fixture(kwgs[index], distributions[index]);
    run_upgrade(&fixture, ERROR_STATUS_SUCCESS);
    WordInfoTable *wit = load_current(&fixture, kwgs[index]);
    for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
         length++) {
      assert(wit->word_plus_floater[length] == NULL);
    }
    assert(index == 0 ? wit->tries[2].num_values == 0
                      : wit->tries[2].num_values == 1);
    word_info_table_destroy(wit);
    WitFileSnapshot current = snapshot_file(fixture.first_wit);
    assert(current.bytes[2] == 0);
    free(current.bytes);
    assert_read_only_reuse(&fixture, fixture.first_wit);
    destroy_fixture(&fixture);
  }
  kwg_destroy(long_kwg);
  kwg_destroy(extended_kwg);
}

void test_wit_upgrade(void) {
  test_upgrade_and_repair();
  test_upgrade_data_path_precedence();
  test_upgrade_ordinary_only_reuse();
}

#include "builder_hash_test.h"

#include "../src/def/builder_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/conversion_results.h"
#include "../src/ent/data_filepaths.h"
#include "../src/impl/config.h"
#include "../src/impl/convert.h"
#include "../src/util/hash.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// What the builders must produce, for a lexicon small enough to build in
// milliseconds.
//
// **These are not arbitrary constants to update when the test goes red.** A
// wordmap and a rack info table are built on every contributor's own machine
// and are far too large to ship, so birdtest checks them by building its own
// copy on the server and comparing hashes (see MAGPIE_DEPENDENCY.md). That
// only works while a hash means something, and a hash means something only
// together with the builder that produced it: a CSW24 wordmap built in
// December 2025 and one built nine months later differ in 72,852,152 bytes
// with the same inputs and the same format version 3, because the builder
// changed and the format did not have to.
//
// So when this test fails, the builder's output has changed. The fix is to
// bump WMP_BUILDER_VERSION, RIT_BUILDER_VERSION, WIT_BUILDER_VERSION or
// KLV_BUILDER_VERSION in
// src/def/builder_defs.h *and* update the hash here. Updating the hash alone
// leaves every server in the world publishing old hashes under a builder name
// that no longer produces them, and every worker declining every job with
// `derived_mismatch`.
//
// Built from testdata/lexica/CSW21_ab.{kwg,klv2} against
// testdata/letterdistributions/english_ab.csv.
#define PINNED_WMP_SHA256                                                      \
  "7bb854bf2bf30cd0cb9bdc280bf5ebe9a5b7968f198bba9cad5b4e113b9af47f"
#define PINNED_RIT_SHA256                                                      \
  "b064d7729ba9117890595bdaa72e005314955b36f2377c97e5eb25a705211037"
#define PINNED_WIT_SHA256                                                      \
  "a5677f57982fd8b89aed8acce09c76cea2ae90659c8da70a822d8ed1b265397c"

// The KLV builders: `createdata klv` (every leave zero, a leave-generation
// job's generation 0) and `convert rackequity2klv` (each later generation, from
// the per-rack aggregates), both for english_ab, over the rack equity file
// write_every_full_rack writes. birdtest's "Check artifacts" rebuilds a KLV and
// says "built by a different builder" rather than "differs" only if a change in
// these bytes came with a KLV_BUILDER_VERSION bump. Both go through
// klv_create_empty, and so through the KWG maker, which is still changing.
#define PINNED_ZERO_KLV_SHA256                                                 \
  "f03b53ebd8524a3e2af31d036b664cfd7efecb53e0c48c48ad67cef704bacd59"
#define PINNED_RACK_EQUITY_KLV_SHA256                                          \
  "db18f990affd0179946247075fa3621915ac33aabb8a8c3c512a8b210462d354"

// The builder versions those hashes belong to. Bumping a version without
// updating its hash, or the other way round, is the mistake this catches:
// a pair that has moved apart is a server and a fleet that disagree.
enum {
  PINNED_WMP_BUILDER_VERSION = 1,
  PINNED_RIT_BUILDER_VERSION = 1,
  PINNED_KLV_BUILDER_VERSION = 1,
  PINNED_WIT_BUILDER_VERSION = 1,
};

static void assert_built_file_hash(const char *data_paths, const char *name,
                                   data_filepath_t type, const char *expected,
                                   const char *what, int builder_version,
                                   int pinned_builder_version) {
  ErrorStack *error_stack = error_stack_create();
  char *path =
      data_filepaths_get_readable_filename(data_paths, name, type, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("%s builder wrote no file for %s", what, name);
  }
  char *actual = sha256_hash_file(path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("could not hash %s", path);
  }
  if (!strings_equal(actual, expected)) {
    printf(
        "\nthe %s builder's output has changed.\n"
        "  built:    %s\n"
        "  expected: %s\n"
        "  file:     %s\n\n"
        "A derived file's hash is only meaningful together with the builder\n"
        "that produced it, and birdtest's server publishes both. Bump\n"
        "%s_BUILDER_VERSION in src/def/builder_defs.h AND update the pinned\n"
        "hash in test/builder_hash_test.c. Updating the hash alone leaves\n"
        "every server publishing hashes under a builder name that no longer\n"
        "produces them.\n\n",
        what, actual, expected, path, what);
    assert(0);
  }
  if (builder_version != pinned_builder_version) {
    printf("\n%s_BUILDER_VERSION is %d but the hash pinned here belongs to "
           "version %d.\n"
           "The two move together: a bump means the output changed, so the "
           "hash must change too.\n\n",
           what, builder_version, pinned_builder_version);
    assert(0);
  }
  free(actual);
  free(path);
  error_stack_destroy(error_stack);
}

// Builds the three derived files for a two-letter lexicon and compares them
// with the hashes pinned above.
//
// Written to the real testdata directory rather than a temporary one, like the
// other conversion tests: data_filepaths resolves writable names within the
// data path search list, and the two outputs are deleted afterwards.
static void test_derived_builders_match_their_pinned_hashes(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -wmp false -rit false");
  const char *data_paths = config_get_data_paths(config);
  ConversionResults *results = conversion_results_create();
  ErrorStack *error_stack = error_stack_create();

  // Single-threaded, because the hash has to be a property of the inputs and
  // not of how many cores the machine running the test happens to have. That
  // it is a property of the inputs is what the design depends on and what was
  // measured: one and eight threads produce byte-identical output for CSW24's
  // wordmap and rack info table.
  const ConversionArgs wmp_args = {
      .conversion_type_string = "dawg2wordmap",
      .data_paths = data_paths,
      .input_and_output_name = "CSW21_ab",
      .ld_name = "english_ab",
      .num_threads = 1,
  };
  convert(&wmp_args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("dawg2wordmap failed");
  }
  assert_built_file_hash(data_paths, "CSW21_ab", DATA_FILEPATH_TYPE_WORDMAP,
                         PINNED_WMP_SHA256, "WMP", WMP_BUILDER_VERSION,
                         PINNED_WMP_BUILDER_VERSION);

  const ConversionArgs rit_args = {
      .conversion_type_string = "klvwmp2rit",
      .data_paths = data_paths,
      .input_and_output_name = "CSW21_ab",
      .ld_name = "english_ab",
      .num_threads = 1,
  };
  convert(&rit_args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("klvwmp2rit failed");
  }
  assert_built_file_hash(data_paths, "CSW21_ab",
                         DATA_FILEPATH_TYPE_RACK_INFO_TABLE, PINNED_RIT_SHA256,
                         "RIT", RIT_BUILDER_VERSION,
                         PINNED_RIT_BUILDER_VERSION);

  // From the .kwg alone, as birdtest's server and its workers build it.
  const ConversionArgs wit_args = {
      .conversion_type_string = "kwg2wit",
      .data_paths = data_paths,
      .input_and_output_name = "CSW21_ab",
      .ld_name = "english_ab",
      .num_threads = 1,
  };
  convert(&wit_args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("kwg2wit failed");
  }
  assert_built_file_hash(data_paths, "CSW21_ab",
                         DATA_FILEPATH_TYPE_WORD_INFO_TABLE, PINNED_WIT_SHA256,
                         "WIT", WIT_BUILDER_VERSION,
                         PINNED_WIT_BUILDER_VERSION);

  char *wmp_path = data_filepaths_get_readable_filename(
      data_paths, "CSW21_ab", DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  char *rit_path = data_filepaths_get_readable_filename(
      data_paths, "CSW21_ab", DATA_FILEPATH_TYPE_RACK_INFO_TABLE, error_stack);
  char *wit_path = data_filepaths_get_readable_filename(
      data_paths, "CSW21_ab", DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  error_stack_reset(error_stack);
  delete_file(wmp_path);
  delete_file(rit_path);
  delete_file(wit_path);
  free(wmp_path);
  free(rit_path);
  free(wit_path);

  error_stack_destroy(error_stack);
  conversion_results_destroy(results);
  config_destroy(config);
}

// english_ab draws only A and B, so its full racks are the eight
// A^k B^(7-k). Each gets k + 1 occurrences and a mean equity of 8k - 20, both
// exact in binary, so the file and what is parsed from it are the same on
// every platform.
static void write_every_full_rack(const char *data_paths, const char *name) {
  ErrorStack *error_stack = error_stack_create();
  char *path = data_filepaths_get_writable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_LEAVES, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("no writable path for %s", name);
  }
  FILE *stream = fopen(path, "we");
  assert(stream);
  for (int a = 0; a <= RACK_SIZE; a++) {
    char rack[RACK_SIZE + 1];
    for (int i = 0; i < RACK_SIZE; i++) {
      rack[i] = i < a ? 'A' : 'B';
    }
    rack[RACK_SIZE] = '\0';
    const int count = a + 1;
    (void)fprintf(stream, "%s,%d,%d\n", rack, count, (8 * a - 20) * count);
  }
  (void)fclose(stream);
  free(path);
  error_stack_destroy(error_stack);
}

static void delete_data_file(const char *data_paths, const char *name,
                             data_filepath_t type) {
  ErrorStack *error_stack = error_stack_create();
  char *path =
      data_filepaths_get_readable_filename(data_paths, name, type, error_stack);
  if (error_stack_is_empty(error_stack)) {
    delete_file(path);
  }
  free(path);
  error_stack_destroy(error_stack);
}

// The KLV builders against their pinned hashes, run through the commands
// birdtest's server runs, so that a changed argument breaks this too.
static void test_klv_builders_match_their_pinned_hashes(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -wmp false -rit false");
  const char *data_paths = config_get_data_paths(config);

  load_and_exec_config_or_die(config,
                              "createdata klv builderhash_zero english_ab");
  assert_built_file_hash(data_paths, "builderhash_zero", DATA_FILEPATH_TYPE_KLV,
                         PINNED_ZERO_KLV_SHA256, "KLV", KLV_BUILDER_VERSION,
                         PINNED_KLV_BUILDER_VERSION);

  write_every_full_rack(data_paths, "builderhash_racks");
  load_and_exec_config_or_die(
      config, "convert rackequity2klv builderhash_racks english_ab -threads 1");
  assert_built_file_hash(data_paths, "builderhash_racks",
                         DATA_FILEPATH_TYPE_KLV, PINNED_RACK_EQUITY_KLV_SHA256,
                         "KLV", KLV_BUILDER_VERSION,
                         PINNED_KLV_BUILDER_VERSION);

  delete_data_file(data_paths, "builderhash_zero", DATA_FILEPATH_TYPE_KLV);
  delete_data_file(data_paths, "builderhash_racks", DATA_FILEPATH_TYPE_KLV);
  delete_data_file(data_paths, "builderhash_racks", DATA_FILEPATH_TYPE_LEAVES);
  config_destroy(config);
}

// Two builds from the same inputs produce the same bytes, whatever the thread
// count.
//
// This is the property birdtest's whole derived-file check rests on, and it is
// not obvious: both builders divide work across threads, and a reduction whose
// order depends on scheduling would produce different bytes from run to run.
// Measured for CSW24 at one and eight threads before anything depended on it;
// this keeps it true.
static void test_a_builder_does_not_depend_on_thread_count(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21_ab -ld english_ab -wmp false -rit false");
  const char *data_paths = config_get_data_paths(config);
  ConversionResults *results = conversion_results_create();
  ErrorStack *error_stack = error_stack_create();

  char *digests[2] = {NULL, NULL};
  const int thread_counts[2] = {1, 4};
  for (int i = 0; i < 2; i++) {
    const ConversionArgs args = {
        .conversion_type_string = "dawg2wordmap",
        .data_paths = data_paths,
        .input_and_output_name = "CSW21_ab",
        .ld_name = "english_ab",
        .num_threads = thread_counts[i],
    };
    convert(&args, results, error_stack);
    assert(error_stack_is_empty(error_stack));
    char *path = data_filepaths_get_readable_filename(
        data_paths, "CSW21_ab", DATA_FILEPATH_TYPE_WORDMAP, error_stack);
    assert(error_stack_is_empty(error_stack));
    digests[i] = sha256_hash_file(path, error_stack);
    assert(error_stack_is_empty(error_stack));
    if (i == 1) {
      delete_file(path);
    }
    free(path);
  }
  assert(strings_equal(digests[0], digests[1]));
  free(digests[0]);
  free(digests[1]);

  error_stack_destroy(error_stack);
  conversion_results_destroy(results);
  config_destroy(config);
}

void test_builder_hash(void) {
  test_derived_builders_match_their_pinned_hashes();
  test_klv_builders_match_their_pinned_hashes();
  test_a_builder_does_not_depend_on_thread_count();
}

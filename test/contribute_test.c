#include "contribute_test.h"

#include "../src/compat/endian_conv.h"
#include "../src/def/board_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/contribute_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/autoplay_results.h"
#include "../src/ent/autoplay_solver_settings.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/client_state.h"
#include "../src/ent/conversion_results.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/contribute.h"
#include "../src/impl/convert.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/rack_list.h"
#include "../src/impl/simmer.h"
#include "../src/util/hash.h"
#include "../src/util/http_client.h"
#include "../src/util/io_util.h"
#include "../src/util/json.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static void test_version_comparison(void) {
  assert(contribute_compare_versions("1.4.0", "1.4.0") == 0);
  // Missing components count as zero rather than as "less than".
  assert(contribute_compare_versions("1.4", "1.4.0") == 0);
  assert(contribute_compare_versions("1.4.0", "1.4") == 0);

  assert(contribute_compare_versions("1.4.0", "1.5.0") < 0);
  assert(contribute_compare_versions("2.0.0", "1.9.9") > 0);
  assert(contribute_compare_versions("1.4.1", "1.4") > 0);

  // The reason this is not a string comparison: "1.10" sorts below "1.9"
  // lexicographically, which would let an out-of-date client accept a job it
  // cannot run.
  assert(contribute_compare_versions("1.10.0", "1.9.0") > 0);
  assert(contribute_compare_versions("1.9.0", "1.10.0") < 0);
  assert(contribute_compare_versions("0.0.0", "1.0.0") < 0);
}

static void test_json_wrapper(void) {
  ErrorStack *error_stack = error_stack_create();

  JsonValue *value =
      json_parse("{\"a\":1,\"b\":\"text\",\"c\":2.5,\"d\":null,\"e\":[1,2,3],"
                 "\"seed\":\"18446744073709551615\",\"f\":true}",
                 error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(value);

  assert(json_get_int(value, "a", error_stack) == 1);
  assert_strings_equal(json_get_string(value, "b", error_stack), "text");
  assert(json_get_double(value, "c", error_stack) == 2.5);
  assert(error_stack_is_empty(error_stack));

  assert(json_is_null(json_object_get(value, "d")));
  assert(json_array_length(json_object_get(value, "e")) == 3);
  assert(json_get_bool_or(value, "f", false));
  assert(json_get_bool_or(value, "missing", true));

  // A full uint64 must survive: JSON numbers are doubles and would lose the
  // low bits, which is why the server sends seeds as decimal strings.
  const uint64_t seed = json_get_uint64_string(value, "seed", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(seed == UINT64_MAX);

  // A missing field is an error rather than a silent zero.
  json_get_int(value, "nope", error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // So is a field of the wrong type.
  json_get_int(value, "b", error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  json_destroy(value);

  assert(!json_parse("{not json", error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  error_stack_destroy(error_stack);
}

static void test_json_serialization(void) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_int_field(sb, "games", 20, &first);
  json_write_double_field(sb, "mean", 429.5, &first);
  json_write_string_field(sb, "quoted", "a\"b\\c", &first);
  json_write_object_end(sb);
  char *text = string_builder_dump_and_destroy(sb, NULL);

  // Round-tripping is the real assertion: escaping that looks right but does
  // not parse would be caught here rather than by the server.
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *const parsed = json_parse(text, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(json_get_int(parsed, "games", error_stack) == 20);
  assert(json_get_double(parsed, "mean", error_stack) == 429.5);
  assert_strings_equal(json_get_string(parsed, "quoted", error_stack),
                       "a\"b\\c");
  assert(error_stack_is_empty(error_stack));
  json_destroy(parsed);
  error_stack_destroy(error_stack);
  free(text);
}

static void write_settings_file(const char *path, const char *contents) {
  FILE *stream = fopen(path, "we");
  assert(stream);
  (void)fputs(contents, stream);
  (void)fclose(stream);
}

static void test_client_state(void) {
  const char *path = "contribute_test_settings.txt";
  ErrorStack *error_stack = error_stack_create();

  // A file with no uuid leaves worker_uuid unset: the client never invents
  // one, the server assigns it on the first claimed task.
  write_settings_file(path, "# a comment\n"
                            "server   https://birdtest.example\n"
                            "apikey   bt_secret\n"
                            "threads  3\n"
                            "maxtasks 7\n"
                            "idlewait 11\n");
  ClientState *state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state);
  assert_strings_equal(state->server_url, "https://birdtest.example");
  assert_strings_equal(state->api_key, "bt_secret");
  assert(state->threads == 3);
  assert(state->max_tasks == 7);
  assert(state->idle_wait_seconds == 11);
  assert(state->worker_uuid == NULL);

  // Once the server assigns a UUID, it is appended rather than rewritten, so
  // the comment survives, and a later load reads it back.
  client_state_set_worker_uuid(state, "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  client_state_destroy(state);

  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  client_state_destroy(state);
  char *contents = get_string_from_file(path, error_stack);
  assert(strstr(contents, "# a comment"));
  free(contents);

  // An unknown key is an error: a typo'd apikey must not silently downgrade
  // someone to anonymous.
  write_settings_file(path, "server https://birdtest.example\napikeys x\n");
  assert(!client_state_load(path, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // A uuid line that is not a UUID -- the start of one, left by a save a full
  // disk cut short -- is refused rather than sent.
  write_settings_file(path, "server https://birdtest.example\nuuid 6f3d7198-1");
  assert(!client_state_load(path, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // Only the last uuid line counts: a partial one followed by a whole one --
  // what adding the line by hand after a failed save leaves -- loads.
  write_settings_file(path, "server https://birdtest.example\nuuid 6f3d7198-1\n"
                            "uuid 6f3d7198-178a-47c8-9ccc-6aa6995a5a9c\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  client_state_destroy(state);

  // No message quotes a value or an unknown word that could be a key: one
  // appended to a last line with no newline, or pasted alone.
  const char *leaks[] = {
      "server https://birdtest.example\nmaxtasks 0apikey bt_SECRETKEY123\n",
      "server https://birdtest.example apikey bt_SECRETKEY123\n",
      "server https://birdtest.example\nbt_SECRETKEY123\n",
      "server https://birdtest.example\napikey bt_x server bt_SECRETKEY123\n",
      "server https://birdtest.example\napikey bt_SECRETKEY123\xc2\xa0\n",
  };
  for (size_t i = 0; i < sizeof(leaks) / sizeof(leaks[0]); i++) {
    write_settings_file(path, leaks[i]);
    assert(!client_state_load(path, error_stack));
    assert(!error_stack_is_empty(error_stack));
    char *message = error_stack_get_string_and_reset(error_stack);
    assert(!strstr(message, "SECRETKEY"));
    free(message);
  }

  // A byte-order mark is skipped; a count past an int's range is refused
  // rather than truncated (4294967296 read as 0, "no limit"); a setting in
  // the wrong case says so.
  write_settings_file(path, "\xEF\xBB\xBFserver https://birdtest.example\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(state->server_url, "https://birdtest.example");
  client_state_destroy(state);
  write_settings_file(path,
                      "server https://birdtest.example\nmaxtasks 4294967296\n");
  assert(!client_state_load(path, error_stack));
  error_stack_reset(error_stack);
  write_settings_file(path, "Server https://birdtest.example\n");
  assert(!client_state_load(path, error_stack));
  char *lowercase = error_stack_get_string_and_reset(error_stack);
  assert(strstr(lowercase, "settings are lowercase"));
  free(lowercase);

  // A negative task count is refused without quoting it.
  write_settings_file(path, "server https://birdtest.example\nmaxtasks -7\n");
  assert(!client_state_load(path, error_stack));
  char *negative = error_stack_get_string_and_reset(error_stack);
  assert(!strstr(negative, "-7"));
  free(negative);

  // Every `#` line is a comment, a key commented out on purpose included,
  // and none is refused. One holding `apikey` then a key -- what appending
  // the setting to a last comment line with no newline makes, whatever the
  // comment ended in -- is recorded, so a run with no key can say where.
  const char *commented[] = {
      "# apikey bt_oldkey123 (laptop, deactivated)",
      "#apikey bt_older",
      "# my laptopapikey   bt_x",
      "# my laptop apikey bt_x",
      "##########apikey bt_x",
      "#apikey\tbt_x",
      "# paste yours as \"apikey bt_...\" below",
  };
  for (size_t i = 0; i < sizeof(commented) / sizeof(commented[0]); i++) {
    char *settings_text = get_formatted_string(
        "server https://birdtest.example\n%s\n", commented[i]);
    write_settings_file(path, settings_text);
    free(settings_text);
    state = client_state_load(path, error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(state->api_key == NULL);
    assert(state->commented_key_line == 2);
    client_state_destroy(state);
  }
  write_settings_file(path, "server https://birdtest.example\n# no key here\n");
  state = client_state_load(path, error_stack);
  assert(state->commented_key_line == 0);
  client_state_destroy(state);

  // An empty one still means none.
  write_settings_file(path, "server https://birdtest.example\nuuid \n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state->worker_uuid == NULL);
  client_state_destroy(state);

  // A negative task count is refused rather than stopping after one task.
  write_settings_file(path, "server https://birdtest.example\nmaxtasks -1\n");
  assert(!client_state_load(path, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // A file that states only an API key contributes under it to the default
  // server, with every other setting its default.
  write_settings_file(path, "apikey bt_x\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state->settings_file_found);
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  assert(!state->server_stated);
  assert_strings_equal(state->api_key, "bt_x");
  assert(state->worker_uuid == NULL);
  assert(state->threads == 0);
  assert(state->max_tasks == 0 && !state->max_tasks_stated);
  assert(state->idle_wait_seconds == 5 && !state->idle_wait_stated);
  client_state_destroy(state);

  // So does one that states only the identity it was issued.
  write_settings_file(path, "uuid 6f3d7198-178a-47c8-9ccc-6aa6995a5a9c\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  assert(state->api_key == NULL);
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  client_state_destroy(state);

  // And an empty one, or one of comments.
  write_settings_file(path, "# nothing set\n\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  client_state_destroy(state);

  // As does a named file that is there but empty.
  write_settings_file(path, "");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state->settings_file_found);
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  assert(state->api_key == NULL);
  client_state_destroy(state);

  // But a named file that is not there is refused, naming it: a typo in the
  // name would otherwise start a new anonymous worker on the default server.
  const char *missing = "contribute_test_does_not_exist.txt";
  (void)remove(missing);
  assert(!client_state_load(missing, error_stack));
  char *refusal = error_stack_get_string_and_reset(error_stack);
  assert(
      strstr(refusal, "'contribute_test_does_not_exist.txt' does not exist"));
  free(refusal);
  // And nothing was created in its place.
  assert(access(missing, F_OK) != 0);

  // A server line with no address is refused rather than taken for the
  // default: deleting the line is how to ask for that.
  write_settings_file(path, "server\napikey bt_x\n");
  assert(!client_state_load(path, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // A file that is there but cannot be read is an error, not the defaults:
  // it may hold the API key the contributor means to run under. (Skipped
  // where permissions do not stop a read, as for root.)
  write_settings_file(path, "apikey bt_x\n");
  assert(chmod(path, 0) == 0);
  FILE *unreadable = fopen(path, "re");
  if (unreadable) {
    (void)fclose(unreadable);
  } else {
    assert(!client_state_load(path, error_stack));
    assert(!error_stack_is_empty(error_stack));
    error_stack_reset(error_stack);
  }
  assert(chmod(path, S_IRUSR | S_IWUSR) == 0);

  // So is a directory of that name, which read as a file 2^63 bytes long
  // ended the process.
  const char *directory = "contribute_test_settings_dir";
  (void)rmdir(directory);
  assert(mkdir(directory, S_IRWXU) == 0);
  assert(!client_state_load(directory, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  assert(rmdir(directory) == 0);

  // What the server sends as an identity is taken only in canonical form: it
  // becomes a header and a settings line, and a newline in it wrote a
  // `server` line every later run obeyed.
  assert(client_state_is_worker_uuid("6f3d7198-178a-47c8-9ccc-6aa6995a5a9c"));
  assert(client_state_is_worker_uuid("6F3D7198-178A-47C8-9CCC-6AA6995A5A9C"));
  assert(!client_state_is_worker_uuid(NULL));
  assert(!client_state_is_worker_uuid(""));
  assert(!client_state_is_worker_uuid("u-1\nserver http://127.0.0.1:1/x"));
  assert(
      !client_state_is_worker_uuid("6f3d7198-178a-47c8-9ccc-6aa6995a5a9c\n"));
  assert(!client_state_is_worker_uuid("6f3d7198-178a-47c8-9ccc-6aa6995a5a9"));
  assert(!client_state_is_worker_uuid("6f3d7198x178a-47c8-9ccc-6aa6995a5a9c"));
  assert(!client_state_is_worker_uuid("6f3d7198-178a-47c8-9ccc-6aa6995a5a9g"));
  assert(!client_state_is_worker_uuid("6f3d7198-178a-47c8-9ccc\r6aa6995a5a9c"));

  // A settings file that cannot be written is reported, not passed over: the
  // identity would last one run and every restart would be a new worker.
  write_settings_file(path, "server https://birdtest.example\n");
  state = client_state_load(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(state->settings_path);
  state->settings_path =
      string_duplicate("contribute_test_no_such_dir/contribute.txt");
  assert(!client_state_set_worker_uuid(state,
                                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c"));
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  client_state_destroy(state);

  (void)remove(path);
  error_stack_destroy(error_stack);
}

// No contribute.txt in the working directory is every setting at its default,
// and the first identity the server issues creates it. Skipped where the
// working directory has a contribute.txt of its own, which this would read
// and must not touch.
static void test_the_default_settings_file_may_be_missing(void) {
  const char *default_path = CONTRIBUTE_SETTINGS_DEFAULT_FILENAME;
  if (access(default_path, F_OK) == 0) {
    return;
  }
  ErrorStack *error_stack = error_stack_create();
  ClientState *state = client_state_load(NULL, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state);
  assert(!state->settings_file_found);
  assert_strings_equal(state->settings_path, default_path);
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  assert(!state->server_stated);
  assert(state->api_key == NULL);
  assert(state->worker_uuid == NULL);
  assert(state->threads == 0);
  assert(state->max_tasks == 0 && !state->max_tasks_stated);
  assert(state->idle_wait_seconds == 5 && !state->idle_wait_stated);
  assert(state->commented_key_line == 0);

  // The first identity the server issues creates it, holding a header and
  // the uuid line and nothing defaulted -- not the server, which a later
  // change of default should still reach.
  assert(client_state_set_worker_uuid(state,
                                      "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c"));
  client_state_destroy(state);
  char *created = get_string_from_file(default_path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(created, CONTRIBUTE_SETTINGS_HEADER
                       "\nuuid 6f3d7198-178a-47c8-9ccc-6aa6995a5a9c\n");
  assert(!strstr(created, "server"));
  free(created);

  // And a later run reads it back, on the default server still.
  state = client_state_load(NULL, error_stack);
  (void)remove(default_path);
  assert(error_stack_is_empty(error_stack));
  assert(state->settings_file_found);
  assert_strings_equal(state->worker_uuid,
                       "6f3d7198-178a-47c8-9ccc-6aa6995a5a9c");
  assert_strings_equal(state->server_url, CONTRIBUTE_DEFAULT_SERVER);
  assert(!state->server_stated);
  client_state_destroy(state);
  error_stack_destroy(error_stack);
}

// The known vectors everyone quotes. A hash that is merely self-consistent
// would verify nothing: the server computes these digests independently, so
// this implementation has to agree with the rest of the world.
static void test_sha256(void) {
  char *empty = sha256_hash_bytes("", 0);
  assert_strings_equal(
      empty,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  free(empty);

  char *abc = sha256_hash_bytes("abc", 3);
  assert_strings_equal(
      abc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  free(abc);

  // Longer than one 64-byte block, so the padding path runs too.
  const char *long_input =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  char *long_digest = sha256_hash_bytes(long_input, strlen(long_input));
  assert_strings_equal(
      long_digest,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  free(long_digest);

  // And the same content through the file path, which streams in chunks.
  const char *path = "contribute_test_hash.bin";
  FILE *file = fopen_or_die(path, "wb");
  fwrite_or_die("abc", 1, 3, file, "hash test bytes");
  fclose_or_die(file);

  ErrorStack *error_stack = error_stack_create();
  char *from_file = sha256_hash_file(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(
      from_file,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  free(from_file);

  // A file that is not there is an error, not a digest of nothing.
  assert(!sha256_hash_file("contribute_test_does_not_exist.bin", error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// The collision the cache key exists to close: same path, same size, same
// mtime, different bytes. Extracting an archive sets mtimes rather than
// letting them fall to now, so this is the realistic case, not a contrived
// one -- and a stale digest here is the one way a bad file passes
// verification.
static void test_digest_cache_key_notices_a_same_size_replacement(void) {
  const char *path = "contribute_test_cache_key.bin";
  FILE *file = fopen_or_die(path, "wb");
  fwrite_or_die("aaaa", 1, 4, file, "cache key test bytes");
  fclose_or_die(file);

  // NOLINTNEXTLINE(misc-include-cleaner)
  struct stat before;
  assert(stat(path, &before) == 0);
  char *first = contribute_digest_cache_key(path);
  assert(first);

  // Replace the contents with different bytes of the same length, then force
  // the modification time back to what it was.
  file = fopen_or_die(path, "wb");
  fwrite_or_die("bbbb", 1, 4, file, "cache key test bytes");
  fclose_or_die(file);
  // NOLINTNEXTLINE(misc-include-cleaner)
  struct utimbuf times = {.actime = before.st_atime,
                          // NOLINTNEXTLINE(misc-include-cleaner)
                          .modtime = before.st_mtime};
  assert(utime(path, &times) == 0);

  struct stat after;
  assert(stat(path, &after) == 0);
  assert(after.st_size == before.st_size);
  assert(after.st_mtime == before.st_mtime);

  char *second = contribute_digest_cache_key(path);
  assert(second);
  // Size and mtime match, so a (path, size, mtime) key would have hit the
  // cache and reported the old digest for the new bytes.
  assert(!strings_equal(first, second));

  free(first);
  free(second);
  (void)remove(path);
}

// birdtest's contract fixtures, copied from its contract-fixtures/. The server
// asserts its half of the contract against the same files.
static const char *const BIRDTEST_GAMES_FIXTURE =
    "test/birdtest_contract/assignment-games.json";
static const char *const BIRDTEST_LEAVE_FIXTURE =
    "test/birdtest_contract/assignment-leave-generation.json";
static const char *const BIRDTEST_OPENING_RACK_FIXTURE =
    "test/birdtest_contract/assignment-opening-rack.json";
static const char *const BIRDTEST_DECLINE_FIXTURE =
    "test/birdtest_contract/decline-missing-data.json";
// Captured from a real exchange between this client and birdtest by
// birdtest's scripts/capture_contract.py, not written by hand.
static const char *const BIRDTEST_GAME_PAIRS_FIXTURE =
    "test/birdtest_contract/assignment-game-pairs.json";
static const char *const BIRDTEST_ANON_UUID_FIXTURE =
    "test/birdtest_contract/anon-uuid-assignment.json";
static const char *const BIRDTEST_EXPECTED_DATA_FIXTURE =
    "test/birdtest_contract/expected-data.json";
static const char *const BIRDTEST_HEARTBEAT_FIXTURE =
    "test/birdtest_contract/heartbeat.json";
static const char *const BIRDTEST_RESULT_GAMES_FIXTURE =
    "test/birdtest_contract/result-games.json";
static const char *const BIRDTEST_RESULT_GAMES_INFERENCE_FIXTURE =
    "test/birdtest_contract/result-games-inference.json";
static const char *const BIRDTEST_RESULT_GAME_PAIRS_FIXTURE =
    "test/birdtest_contract/result-game-pairs.json";
static const char *const BIRDTEST_RESULT_OPENING_RACK_FIXTURE =
    "test/birdtest_contract/result-opening-rack.json";
static const char *const BIRDTEST_RESULT_LEAVE_FIXTURE =
    "test/birdtest_contract/result-leave-generation.json";

static JsonValue *load_fixture(const char *path) {
  ErrorStack *error_stack = error_stack_create();
  char *text = get_string_from_file_or_die(path);
  JsonValue *fixture = json_parse(text, error_stack);
  free(text);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return fixture;
}

static JsonValue *load_task_request_fixture(const char *path,
                                            const JsonValue **request) {
  ErrorStack *error_stack = error_stack_create();
  char *text = get_string_from_file_or_die(path);
  JsonValue *fixture = json_parse(text, error_stack);
  free(text);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  *request = json_object_get(fixture, "task_request");
  assert(*request);
  return fixture;
}

static void assert_fixture_has_keys(const JsonValue *object,
                                    const char *const *keys, int num_keys,
                                    const char *where) {
  for (int i = 0; i < num_keys; i++) {
    if (!json_object_get(object, keys[i])) {
      log_fatal("birdtest's %s has no '%s', which contribute reads", where,
                keys[i]);
    }
  }
}

// Every key the contribute executors read has to be one birdtest sends. This
// is the check that would have caught "plies" and "top_plays" being read while
// birdtest sent "num_plies" and "num_plays": every simming player ran on the
// worker's own ambient settings and nothing failed.
// Every derived file the claim pins has to carry what
// config_contribute_ensure_wordmap, config_contribute_ensure_rack_info_table
// and config_contribute_ensure_word_info_table read off it. A wordmap, a rack
// info table or a word info table is built on this machine and never shipped,
// so this hash is the only thing that says the bytes are the ones the job means
// -- a fixture missing a field here is a worker running unverified, which is
// the state this whole mechanism replaces.
static void assert_expected_data_pins_derived_files(const JsonValue *expected) {
  assert(expected);
  const JsonValue *derived = json_object_get(expected, "derived");
  if (!derived) {
    log_fatal("birdtest's assignment has no expected_data.derived");
  }
  const int count = json_array_length(derived);
  assert(count > 0);
  for (int i = 0; i < count; i++) {
    const JsonValue *entry = json_array_get(derived, i);
    const char *role = json_get_string_or_null(entry, "role");
    assert(role);
    assert(strings_equal(role, "wmp") || strings_equal(role, "rit") ||
           strings_equal(role, "wit"));
    assert(json_get_string_or_null(entry, "name"));
    assert(json_get_string_or_null(entry, "sha256"));
    // Diagnostic rather than load-bearing -- a worker does not refuse work
    // over them -- but the server states both, and a decline that could not
    // name the builder would be unactionable.
    assert(json_get_string_or_null(entry, "builder"));
    assert(json_get_string_or_null(entry, "build_target"));
  }
}

static void assert_fixture_pins_derived_files(const JsonValue *fixture) {
  assert_expected_data_pins_derived_files(
      json_object_get(fixture, "expected_data"));
}

// What contribute_claim_task reads off every assignment, whatever its job
// type. `worker_uuid` is not among them: the server sends it only to a worker
// that arrived with no identity, which is its own fixture.
static void assert_fixture_is_an_assignment(const JsonValue *fixture,
                                            const char *where) {
  const char *const envelope_keys[] = {
      "claim_token",        "job_id",        "task_request",
      "min_magpie_version", "expected_data",
  };
  assert_fixture_has_keys(fixture, envelope_keys,
                          sizeof(envelope_keys) / sizeof(envelope_keys[0]),
                          where);
  assert(json_get_string_or_null(json_object_get(fixture, "task_request"),
                                 "job_type"));
}

static void test_contract_fixtures_carry_every_key_contribute_reads(void) {
  const JsonValue *request = NULL;
  const JsonValue *games =
      load_task_request_fixture(BIRDTEST_GAMES_FIXTURE, &request);
  const char *const request_keys[] = {
      CONTRIBUTE_KEY_VARIANT,      CONTRIBUTE_KEY_LETTER_DISTRIBUTION,
      CONTRIBUTE_KEY_BOARD_LAYOUT, CONTRIBUTE_KEY_SEED,
      CONTRIBUTE_KEY_NUM_GAMES,    CONTRIBUTE_KEY_CAPTURE_POSITIONS,
      CONTRIBUTE_KEY_PLAYER1,      CONTRIBUTE_KEY_PLAYER2,
      CONTRIBUTE_KEY_BINGO_BONUS,  CONTRIBUTE_KEY_SIM_CUTOFF,
  };
  assert_fixture_has_keys(request, request_keys,
                          sizeof(request_keys) / sizeof(request_keys[0]),
                          "games task_request");
  const char *const player_keys[] = {
      CONTRIBUTE_KEY_PLAYER_LEXICON,     CONTRIBUTE_KEY_LEAVES,
      CONTRIBUTE_KEY_RECORDER_TYPE,      CONTRIBUTE_KEY_SORT_STRATEGY,
      CONTRIBUTE_KEY_MAX_ITERATIONS,     CONTRIBUTE_KEY_NUM_PLIES,
      CONTRIBUTE_KEY_NUM_PLIES_RECORDED, CONTRIBUTE_KEY_NUM_PLAYS,
      CONTRIBUTE_KEY_NUM_PLAYS_RECORDED, CONTRIBUTE_KEY_STOPPING_PCT,
      CONTRIBUTE_KEY_USE_INFERENCE,      CONTRIBUTE_KEY_TIME_LIMIT_SECS,
      CONTRIBUTE_KEY_USE_WORDMAP,        CONTRIBUTE_KEY_USE_RIT,
      CONTRIBUTE_KEY_RIT_NAME,           CONTRIBUTE_KEY_MIN_PLAY_ITERATIONS,
      CONTRIBUTE_KEY_THRESHOLD,          CONTRIBUTE_KEY_SAMPLING_RULE,
      CONTRIBUTE_KEY_INFERENCE_MARGIN,   CONTRIBUTE_KEY_UTILITY_W_WINPCT,
      CONTRIBUTE_KEY_UTILITY_W_SPREAD,   CONTRIBUTE_KEY_UTILITY_SPREAD_SCALE,
      CONTRIBUTE_KEY_WIN_PCT_MODEL,      CONTRIBUTE_KEY_MOVEGEN_MARGIN,
      CONTRIBUTE_KEY_ENDGAME_PLIES,      CONTRIBUTE_KEY_PEG_MAX_BAG,
  };
  const int num_player_keys = sizeof(player_keys) / sizeof(player_keys[0]);
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER1),
                          player_keys, num_player_keys, "games player1");
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER2),
                          player_keys, num_player_keys, "games player2");
  assert_fixture_pins_derived_files(games);
  json_destroy(games);

  // Opening racks are the one job type whose request carries "racks" and a
  // single "player" rather than a player pair, so nothing else pins those two
  // names -- a rename on either side would have passed both test suites and
  // broken every opening-rack contributor.
  const JsonValue *opening_rack =
      load_task_request_fixture(BIRDTEST_OPENING_RACK_FIXTURE, &request);
  const char *const opening_rack_keys[] = {
      CONTRIBUTE_KEY_VARIANT,      CONTRIBUTE_KEY_LETTER_DISTRIBUTION,
      CONTRIBUTE_KEY_BOARD_LAYOUT, CONTRIBUTE_KEY_RACKS,
      CONTRIBUTE_KEY_SEED,         CONTRIBUTE_KEY_PLAYER,
      CONTRIBUTE_KEY_BINGO_BONUS,  CONTRIBUTE_KEY_SIM_CUTOFF,
  };
  assert_fixture_has_keys(request, opening_rack_keys,
                          sizeof(opening_rack_keys) /
                              sizeof(opening_rack_keys[0]),
                          "opening_rack task_request");
  // The batch is the racks themselves, not a range: an empty one is a task
  // with nothing to do, which the executor refuses.
  assert(json_array_length(json_object_get(request, CONTRIBUTE_KEY_RACKS)) > 0);
  // The executor applies the player the same way the games executor applies
  // player1, so it needs the same keys -- including the win% model, which it
  // loads itself when the player simulates.
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER),
                          player_keys, num_player_keys, "opening_rack player");
  assert_fixture_pins_derived_files(opening_rack);
  json_destroy(opening_rack);

  const JsonValue *leave =
      load_task_request_fixture(BIRDTEST_LEAVE_FIXTURE, &request);
  const char *const leave_keys[] = {
      CONTRIBUTE_KEY_LEXICON,
      CONTRIBUTE_KEY_VARIANT,
      CONTRIBUTE_KEY_LETTER_DISTRIBUTION,
      CONTRIBUTE_KEY_BOARD_LAYOUT,
      CONTRIBUTE_KEY_SEED,
      CONTRIBUTE_KEY_FORCED_RACKS,
      CONTRIBUTE_KEY_NUM_GAMES,
      CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_KEY,
      CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_SHA256,
      CONTRIBUTE_KEY_BINGO_BONUS,
      CONTRIBUTE_KEY_PLAYER,
  };
  assert_fixture_has_keys(request, leave_keys,
                          sizeof(leave_keys) / sizeof(leave_keys[0]),
                          "leave_generation task_request");
  // The generation's rack target is server-only state; a client that
  // required it would fail every leave_generation task.
  assert(!json_object_get(request, "target_rack_count"));
  assert(!json_object_get(request, "target_rack_counts"));
  // Both seats are the job's player, applied as the other executors apply
  // theirs, so it states every key they read. Its wordmap flag is the one the
  // executor reads: the request no longer carries one of its own.
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER),
                          player_keys, num_player_keys,
                          "leave_generation player");
  assert(!json_object_get(request, CONTRIBUTE_KEY_USE_WORDMAP));
  json_destroy(leave);

  // A game_pairs task is run by the games executor, told apart only by its
  // job_type -- the request carries no flag of its own saying so -- so it
  // needs every key a games request does.
  const JsonValue *pairs =
      load_task_request_fixture(BIRDTEST_GAME_PAIRS_FIXTURE, &request);
  assert_fixture_is_an_assignment(pairs, "game_pairs assignment");
  assert_strings_equal(json_get_string_or_null(request, "job_type"),
                       "game_pairs");
  assert(!json_object_get(request, "game_pairs"));
  assert_fixture_has_keys(request, request_keys,
                          sizeof(request_keys) / sizeof(request_keys[0]),
                          "game_pairs task_request");
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER1),
                          player_keys, num_player_keys, "game_pairs player1");
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER2),
                          player_keys, num_player_keys, "game_pairs player2");
  assert_fixture_pins_derived_files(pairs);
  assert(!json_object_get(pairs, "worker_uuid"));
  json_destroy(pairs);

  // The first task a worker with no identity is given carries the identity
  // the server minted for it, which adopt_server_assigned_uuid persists.
  const JsonValue *anon =
      load_task_request_fixture(BIRDTEST_ANON_UUID_FIXTURE, &request);
  assert_fixture_is_an_assignment(anon, "first assignment");
  // In the form the client takes: the server's format and the client's
  // check are tied here.
  assert(client_state_is_worker_uuid(
      json_get_string_or_null(anon, "worker_uuid")));
  json_destroy(anon);

  // Everything expected_data_matches reads off the digest list.
  const JsonValue *expected = load_fixture(BIRDTEST_EXPECTED_DATA_FIXTURE);
  assert_strings_equal(json_get_string_or_null(expected, "algorithm"),
                       "sha256");
  const JsonValue *files = json_object_get(expected, "files");
  assert(json_array_length(files) > 0);
  const char *const file_keys[] = {"role", "name", "sha256", "path",
                                   "tarball_date"};
  for (int i = 0; i < json_array_length(files); i++) {
    const JsonValue *file = json_array_get(files, i);
    assert_fixture_has_keys(file, file_keys,
                            sizeof(file_keys) / sizeof(file_keys[0]),
                            "expected_data file");
    const char *role = json_get_string_or_null(file, "role");
    // The roles role_to_filepath_type resolves; any other is skipped, which
    // would be a file the job pins and this client never checks.
    assert(strings_equal(role, "kwg") || strings_equal(role, "klv") ||
           strings_equal(role, "winpct") || strings_equal(role, "letterdist") ||
           strings_equal(role, "layout"));
  }
  assert_expected_data_pins_derived_files(expected);
  json_destroy(expected);

  // A heartbeat is the claim token and nothing else.
  const JsonValue *heartbeat = load_fixture(BIRDTEST_HEARTBEAT_FIXTURE);
  assert(json_object_size(heartbeat) == 1);
  assert(json_get_string_or_null(heartbeat, "claim_token"));
  json_destroy(heartbeat);
}

// Every key `fixture` has, at every depth, `produced` has too. An array is
// matched member by member against whichever produced element carries the
// key, so a member only some elements have -- a captured position's
// previous_move -- counts once any element produces it.
// Whether `produced` has every key `fixture` has, at every depth, with the
// same matching of array members as assert_produces_every_fixture_key.
static bool produces_every_fixture_key(const JsonValue *fixture,
                                       const JsonValue *produced) {
  if (json_is_object(fixture)) {
    if (!json_is_object(produced)) {
      return false;
    }
    for (int i = 0; i < json_object_size(fixture); i++) {
      const char *key = json_object_key_at(fixture, i);
      const JsonValue *mine = json_object_get(produced, key);
      if (!mine ||
          !produces_every_fixture_key(json_object_get(fixture, key), mine)) {
        return false;
      }
    }
    return true;
  }
  if (!json_is_array(fixture)) {
    return true;
  }
  if (!json_is_array(produced)) {
    return false;
  }
  for (int i = 0; i < json_array_length(fixture); i++) {
    const JsonValue *element = json_array_get(fixture, i);
    for (int k = 0; k < json_object_size(element); k++) {
      const char *key = json_object_key_at(element, k);
      bool found = false;
      for (int j = 0; j < json_array_length(produced) && !found; j++) {
        const JsonValue *mine =
            json_object_get(json_array_get(produced, j), key);
        found = mine &&
                produces_every_fixture_key(json_object_get(element, key), mine);
      }
      if (!found) {
        return false;
      }
    }
  }
  return true;
}

static void assert_produces_every_fixture_key(const JsonValue *fixture,
                                              const JsonValue *produced,
                                              const char *where) {
  if (json_is_object(fixture)) {
    if (!json_is_object(produced)) {
      log_fatal("%s: birdtest's fixture has an object where MAGPIE's result "
                "does not",
                where);
    }
    for (int i = 0; i < json_object_size(fixture); i++) {
      const char *key = json_object_key_at(fixture, i);
      const JsonValue *mine = json_object_get(produced, key);
      if (!mine) {
        log_fatal("%s: birdtest's fixture has '%s', which MAGPIE's result "
                  "does not",
                  where, key);
      }
      char *path = get_formatted_string("%s.%s", where, key);
      assert_produces_every_fixture_key(json_object_get(fixture, key), mine,
                                        path);
      free(path);
    }
    return;
  }
  if (!json_is_array(fixture)) {
    return;
  }
  if (!json_is_array(produced)) {
    log_fatal("%s: birdtest's fixture has an array where MAGPIE's result "
              "does not",
              where);
  }
  for (int i = 0; i < json_array_length(fixture); i++) {
    const JsonValue *element = json_array_get(fixture, i);
    for (int k = 0; k < json_object_size(element); k++) {
      const char *key = json_object_key_at(element, k);
      // Members differ in shape -- a captured position decided by a solver
      // carries keys a static one does not -- so a member's key is matched
      // against whichever produced member carries everything under it.
      const JsonValue *mine = NULL;
      const JsonValue *any = NULL;
      for (int j = 0; j < json_array_length(produced) && !mine; j++) {
        const JsonValue *candidate =
            json_object_get(json_array_get(produced, j), key);
        if (candidate && !any) {
          any = candidate;
        }
        if (candidate && produces_every_fixture_key(
                             json_object_get(element, key), candidate)) {
          mine = candidate;
        }
      }
      if (!any) {
        log_fatal("%s[]: birdtest's fixture has '%s', which no element of "
                  "MAGPIE's result does",
                  where, key);
      }
      if (!mine) {
        // Recursing into the first that has it names what is missing.
        mine = any;
      }
      char *path = get_formatted_string("%s[].%s", where, key);
      assert_produces_every_fixture_key(json_object_get(element, key), mine,
                                        path);
      free(path);
    }
  }
}

// The result a fixture holds, under the envelope contribute_result_body
// writes around it: every key of the fixture's envelope is one that body
// has. (A fixture captured before results carried `movegens` has none.)
static const JsonValue *fixture_result(const JsonValue *fixture) {
  char *body = contribute_result_body("token", "{}", 1);
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *envelope = json_parse(body, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int i = 0; i < json_object_size(fixture); i++) {
    const char *key = json_object_key_at(fixture, i);
    if (!json_object_get(envelope, key)) {
      log_fatal("birdtest's result fixture has '%s' beside the result, which "
                "MAGPIE's submission does not",
                key);
    }
  }
  json_destroy(envelope);
  free(body);
  assert(json_get_string_or_null(fixture, "claim_token"));
  if (json_object_get(fixture, "movegens")) {
    assert(json_get_int(fixture, "movegens", error_stack) >= 0);
    assert(error_stack_is_empty(error_stack));
  }
  error_stack_destroy(error_stack);
  const JsonValue *result = json_object_get(fixture, "result");
  assert(json_is_object(result));
  return result;
}

static void assert_result_produces_fixture_keys(const char *fixture_path,
                                                const char *produced_json) {
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *produced = json_parse(produced_json, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  const JsonValue *fixture = load_fixture(fixture_path);
  assert_produces_every_fixture_key(fixture_result(fixture), produced,
                                    fixture_path);
  json_destroy(fixture);
  json_destroy(produced);
}

// birdtest's result fixtures are what this client submitted in a real
// exchange, and birdtest parses each against the types that store it. This is
// the other half: the serializers a task's result is built with still produce
// every key those fixtures carry -- so a key renamed here fails this test
// rather than every submission, which the server would refuse as malformed.
static void test_results_carry_every_key_the_server_reads(void) {
  // Both players solve their endgames and small pre-endgames, so the captured
  // positions include solver analyses alongside the static turns before them.
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 score -r1 best -r2 best "
      "-threads 1 -maxnumdplays 5 -eplies1 2 -eplies2 2 -pegbag1 2 -pegbag2 2 "
      "-pegtopk1 2 -pegtopk2 2 -pegnested1 false -pegnested2 false");

  // games, with capture on: the game recorder and the positions recorder,
  // together, as config_contribute_games asks for them. Four games, so a
  // pre-endgame turn comes up.
  load_and_exec_config_or_die(config, "autoplay games,positions 4 -seed 3");
  assert_result_produces_fixture_keys(
      BIRDTEST_RESULT_GAMES_FIXTURE,
      autoplay_results_get_json(config_get_autoplay_results(config), false));

  // game_pairs: all_games, the pentanomial and the divergent subset, and each
  // diverging pair's positions at its first divergence.
  load_and_exec_config_or_die(
      config, "autoplay games,divergentpositions 2 -seed 3 -gp true");
  assert_result_produces_fixture_keys(
      BIRDTEST_RESULT_GAME_PAIRS_FIXTURE,
      autoplay_results_get_json(config_get_autoplay_results(config), true));

  // leave_generation: every rack that occurred, from the rack list the task
  // accumulates.
  ErrorStack *error_stack = error_stack_create();
  const LetterDistribution *ld = config_get_ld(config);
  const char *forced_racks[] = {"AEINRST"};
  RackList *rack_list = rack_list_create(ld, 1, forced_racks, 1, error_stack);
  assert(error_stack_is_empty(error_stack));
  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  rack_set_to_string(ld, &rack, "AEINRST");
  rack_list_add_rack(rack_list, &rack, 25.0);
  char *leave_json = rack_list_get_rack_equity_json(rack_list, ld);
  assert_result_produces_fixture_keys(BIRDTEST_RESULT_LEAVE_FIXTURE,
                                      leave_json);
  free(leave_json);
  rack_list_destroy(rack_list);
  config_destroy(config);

  // opening_rack: a simulated analysis, written the way
  // config_contribute_analyze_rack writes one -- the rack, the ranked plays
  // with their simulation statistics, and how many were ranked.
  config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 5 -plies 2 -threads 1 -iter 30 -scond none -seed 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack AEINRST");
  load_and_exec_config_or_die(config, "gen");
  SimResults *sim_results = config_get_sim_results(config);
  assert(config_simulate_and_return_status(config, NULL, NULL, sim_results) ==
         ERROR_STATUS_SUCCESS);
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_array_start(sb, CONTRIBUTE_KEY_RACKS, &first);
  bool rack_first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, CONTRIBUTE_KEY_RACK, "AEINRST", &rack_first);
  const int num_moves = autoplay_results_write_ranked_plays_json(
      sb, &rack_first, config_get_game(config), config_get_move_list(config),
      sim_results, 5, 2);
  json_write_int_field(sb, CONTRIBUTE_KEY_NUM_MOVES, num_moves, &rack_first);
  json_write_object_end(sb);
  json_write_array_end(sb);
  json_write_object_end(sb);
  assert_result_produces_fixture_keys(BIRDTEST_RESULT_OPENING_RACK_FIXTURE,
                                      string_builder_peek(sb));
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// A submission is the claim token, the task's result as it was computed, and
// beside it the move generations the task took -- what the contributor is
// credited with, a whole number however large.
static void test_the_result_body_carries_movegens(void) {
  const char *result_json = "{\"racks\":[{\"rack\":\"AEINRST\"}]}";
  char *body = contribute_result_body("2b1c4e7a", result_json, 12345);
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *parsed = json_parse(body, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(json_object_size(parsed) == 3);
  assert_strings_equal(json_get_string(parsed, "claim_token", error_stack),
                       "2b1c4e7a");
  const JsonValue *result = json_object_get(parsed, "result");
  assert(json_is_object(result));
  assert_strings_equal(
      json_get_string(json_array_get(json_object_get(result, "racks"), 0),
                      "rack", error_stack),
      "AEINRST");
  assert(json_get_int(parsed, "movegens", error_stack) == 12345);
  assert(error_stack_is_empty(error_stack));
  json_destroy(parsed);
  free(body);

  // Written in full, not as a double: a count past 2^53 keeps its last digit.
  body = contribute_result_body("t", "{}", UINT64_MAX);
  assert(strstr(body, "\"movegens\":18446744073709551615}"));
  parsed = json_parse(body, error_stack);
  assert(error_stack_is_empty(error_stack));
  json_destroy(parsed);
  free(body);
  body = contribute_result_body("t", "{}", 0);
  assert(strstr(body, "\"movegens\":0}"));
  free(body);
  error_stack_destroy(error_stack);
}

// A static player in a request: no simulation, no derived files, no solving.
#define STATIC_CSW21_PLAYER                                                    \
  "{\"lexicon\": \"CSW21\", \"leaves\": \"CSW21\", "                           \
  "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "               \
  "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "          \
  "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0, "                      \
  "\"use_wordmap\": false, \"use_rit\": false, \"use_wit\": false, "           \
  "\"endgame_plies\": 0, \"peg_max_bag\": 0}"

// Runs a task through the executor contribute runs every claim through,
// returning its result and setting *movegens.
static char *execute_task(const char *job_type, const char *request_json,
                          int threads, uint64_t *movegens) {
  Config *config = config_create_default_test();
  ErrorStack *error_stack = error_stack_create();
  Config *task_config = config_create_for_contribute(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  const JsonValue *request = json_parse(request_json, error_stack);
  assert(error_stack_is_empty(error_stack));
  char *result_json = NULL;
  assert(config_contribute_execute(task_config, job_type, request, threads,
                                   NULL, &result_json, movegens, error_stack));
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(false);
  }
  assert(result_json);
  json_destroy(request);
  error_stack_destroy(error_stack);
  config_destroy_for_contribute(task_config);
  config_destroy(config);
  return result_json;
}

// A task reports the move generations it made, on every thread it ran. The
// cases this pins: a static opening-rack analysis is one generation a rack;
// a batch of static games is at least one a turn, and the same count however
// many threads play it, since the games themselves are the same; and a job
// type this build does not know runs nothing.
static void test_a_task_counts_its_move_generations(void) {
  const char *racks[] = {"AEINRST", "?AEILNT", "AABBCDE", "QI", "EEEIIOU"};
  const int num_racks = (int)(sizeof(racks) / sizeof(racks[0]));
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(
      sb, "{\"job_type\": \"opening_rack\", \"variant\": \"classic\", "
          "\"letter_distribution\": \"english\", "
          "\"board_layout\": \"standard15\", \"seed\": \"7\", "
          "\"bingo_bonus\": 50, \"sim_cutoff\": 0.005, \"racks\": [");
  for (int i = 0; i < num_racks; i++) {
    string_builder_add_formatted_string(sb, "%s\"%s\"", i ? ", " : "",
                                        racks[i]);
  }
  string_builder_add_string(sb, "], \"player\": " STATIC_CSW21_PLAYER "}");
  uint64_t movegens = 0;
  char *result =
      execute_task("opening_rack", string_builder_peek(sb), 2, &movegens);
  string_builder_destroy(sb);
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *parsed = json_parse(result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(json_array_length(json_object_get(parsed, CONTRIBUTE_KEY_RACKS)) ==
         num_racks);
  json_destroy(parsed);
  free(result);
  assert(movegens == (uint64_t)num_racks);

  const char *games_request =
      "{\"job_type\": \"games\", \"variant\": \"classic\", "
      "\"letter_distribution\": \"english\", "
      "\"board_layout\": \"standard15\", \"seed\": \"41\", "
      "\"num_games\": 6, \"capture_positions\": true, "
      "\"capture_first_divergence\": false, \"bingo_bonus\": 50, "
      "\"sim_cutoff\": 0.005, \"player1\": " STATIC_CSW21_PLAYER
      ", \"player2\": " STATIC_CSW21_PLAYER "}";
  uint64_t first_movegens = 0;
  const int thread_counts[] = {1, 3};
  for (size_t t = 0; t < sizeof(thread_counts) / sizeof(thread_counts[0]);
       t++) {
    result = execute_task("games", games_request, thread_counts[t], &movegens);
    parsed = json_parse(result, error_stack);
    assert(error_stack_is_empty(error_stack));
    // A captured position for every turn played.
    const int turns =
        json_array_length(json_object_get(parsed, CONTRIBUTE_KEY_POSITIONS));
    assert(turns >= 6 * 10);
    assert(movegens >= (uint64_t)turns);
    if (t == 0) {
      first_movegens = movegens;
    } else {
      assert(movegens == first_movegens);
    }
    json_destroy(parsed);
    free(result);
  }

  // Nothing run, nothing counted.
  Config *config = config_create_default_test();
  const JsonValue *request = json_parse("{}", error_stack);
  char *none = NULL;
  movegens = 99;
  assert(!config_contribute_execute(config, "no_such_job", request, 1, NULL,
                                    &none, &movegens, error_stack));
  assert(error_stack_is_empty(error_stack));
  assert(!none && movegens == 0);
  json_destroy(request);
  config_destroy(config);
  error_stack_destroy(error_stack);
}

// The board of a CGP: everything before its first space.
static char *cgp_board(const char *cgp) {
  const char *space = strchr(cgp, ' ');
  assert(space);
  const size_t length = (size_t)(space - cgp);
  char *board = malloc_or_die(length + 1);
  memcpy(board, cgp, length);
  board[length] = '\0';
  return board;
}

// Runs a paired autoplay keeping first divergences and checks what it kept:
// for each pair whose games diverged, exactly two positions -- one per game,
// at one turn, on one board with one rack to play from -- and from a pair
// played identically, none. Returns how many positions were kept.
static int assert_first_divergences(Config *config, const char *settings,
                                    int pairs) {
  load_and_exec_config_or_die(config, settings);
  char *command = get_formatted_string(
      "autoplay games,divergentpositions %d -seed 5 -gp true", pairs);
  load_and_exec_config_or_die(config, command);
  free(command);
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *result = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), true),
      error_stack);
  assert(error_stack_is_empty(error_stack));
  const JsonValue *positions =
      json_object_get(result, CONTRIBUTE_KEY_POSITIONS);
  const int count = json_array_length(positions);
  const int64_t divergent_games =
      json_get_int(json_object_get(result, CONTRIBUTE_KEY_DIVERGENT_GAMES),
                   CONTRIBUTE_KEY_GAMES, error_stack);
  assert(error_stack_is_empty(error_stack));
  // Two per divergent pair, as the games are two per pair.
  assert(count == divergent_games);

  int *kept = calloc_or_die((size_t)pairs, sizeof(int));
  int *turn = calloc_or_die((size_t)pairs, sizeof(int));
  char **board = calloc_or_die((size_t)pairs, sizeof(char *));
  char **rack = calloc_or_die((size_t)pairs, sizeof(char *));
  bool *game_seen = calloc_or_die((size_t)pairs * 2, sizeof(bool));
  for (int i = 0; i < count; i++) {
    const JsonValue *position = json_array_get(positions, i);
    const int game_index =
        (int)json_get_int(position, CONTRIBUTE_KEY_GAME_INDEX, error_stack);
    const int turn_number =
        (int)json_get_int(position, CONTRIBUTE_KEY_TURN_NUMBER, error_stack);
    const char *position_rack =
        json_get_string(position, CONTRIBUTE_KEY_RACK, error_stack);
    char *position_board = cgp_board(
        json_get_string(position, CONTRIBUTE_KEY_POSITION, error_stack));
    assert(error_stack_is_empty(error_stack));
    assert(game_index >= 0 && game_index < pairs * 2);
    // One position per game.
    assert(!game_seen[game_index]);
    game_seen[game_index] = true;
    const int pair = game_index / 2;
    if (kept[pair] == 0) {
      turn[pair] = turn_number;
      board[pair] = position_board;
      rack[pair] = string_duplicate(position_rack);
    } else {
      // The other game of the pair: the same turn of the same position, the
      // players to move in the two games holding the same tiles.
      assert(turn[pair] == turn_number);
      assert(strings_equal(board[pair], position_board));
      assert(strings_equal(rack[pair], position_rack));
      free(position_board);
    }
    kept[pair]++;
  }
  for (int pair = 0; pair < pairs; pair++) {
    assert(kept[pair] == 0 || kept[pair] == 2);
    free(board[pair]);
    free(rack[pair]);
  }
  free(kept);
  free(turn);
  free(board);
  free(rack);
  free(game_seen);
  json_destroy(result);
  error_stack_destroy(error_stack);
  return count;
}

// A game pair's first divergence: both games' positions at the first turn the
// two games play different moves, and nothing else from the pair.
static void test_a_pairs_first_divergence_is_both_games_at_one_turn(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -r1 best -r2 best -threads 1 "
      "-maxnumdplays 3");
  // Equity against score: nearly every pair diverges, at whatever turn the
  // two first disagree.
  assert(assert_first_divergences(config, "set -s1 equity -s2 score", 12) > 0);
  // One static player against itself plays every pair identically, so
  // nothing is kept: every turn's positions were held, and discarded.
  assert(assert_first_divergences(config, "set -s1 equity -s2 equity", 6) == 0);

  // A first divergence is a pair's, and it is the positions recorder in a
  // second mode, not a second recorder.
  assert_config_exec_status(config,
                            "autoplay games,divergentpositions 2 -gp false",
                            ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  assert_config_exec_status(
      config, "autoplay games,positions,divergentpositions 2 -gp true",
      ERROR_STATUS_AUTOPLAY_INVALID_OPTIONS);
  config_destroy(config);
}

// Capturing positions decides what a games task reports, never what it plays.
// The case this pins: with capture on, a static player read the first element
// of its move list as the move to play, and recording every move leaves that
// list a min-heap -- so it played its lowest-ranked move, a pass whenever it
// had one, and each captured position reported the worst plays as its
// analysis. A capture job played entirely different, far weaker games than the
// same job without capture, and nothing failed.
static void test_capturing_positions_does_not_change_the_games(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-threads 1 -maxnumdplays 5");
  ErrorStack *error_stack = error_stack_create();

  load_and_exec_config_or_die(config, "autoplay games 4 -seed 21");
  const JsonValue *plain = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), false),
      error_stack);
  load_and_exec_config_or_die(config, "autoplay games,positions 4 -seed 21");
  const JsonValue *captured = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), false),
      error_stack);
  assert(error_stack_is_empty(error_stack));

  const JsonValue *plain_games = json_object_get(plain, "all_games");
  const JsonValue *captured_games = json_object_get(captured, "all_games");
  const char *const tallies[] = {"games", "wins", "losses", "ties"};
  for (size_t i = 0; i < sizeof(tallies) / sizeof(tallies[0]); i++) {
    assert(json_get_int(plain_games, tallies[i], error_stack) ==
           json_get_int(captured_games, tallies[i], error_stack));
  }
  const char *const means[] = {"p1_score_mean", "p2_score_mean"};
  for (size_t i = 0; i < sizeof(means) / sizeof(means[0]); i++) {
    assert(json_get_double(plain_games, means[i], error_stack) ==
           json_get_double(captured_games, means[i], error_stack));
  }
  assert(error_stack_is_empty(error_stack));

  // And every position's plays come best-first, as the server stores them:
  // the first is the one played.
  const JsonValue *positions = json_object_get(captured, "positions");
  assert(json_array_length(positions) > 0);
  for (int i = 0; i < json_array_length(positions); i++) {
    const JsonValue *moves =
        json_object_get(json_array_get(positions, i), "moves");
    assert(json_array_length(moves) > 0);
    for (int m = 1; m < json_array_length(moves); m++) {
      assert(json_get_double(json_array_get(moves, m - 1), "equity",
                             error_stack) >=
             json_get_double(json_array_get(moves, m), "equity", error_stack));
    }
  }
  assert(error_stack_is_empty(error_stack));

  json_destroy(captured);
  json_destroy(plain);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// A null in a request means MAGPIE's default, not whatever the process last
// had. The case this pins: a static player applied after a simming one -- or
// after the contributor's own settings asked for plies -- must not simulate.
static void test_player_settings_do_not_leak_between_tasks(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -plies 5 -sm1 true -sm2 true");
  const JsonValue *request = NULL;
  const JsonValue *games =
      load_task_request_fixture(BIRDTEST_GAMES_FIXTURE, &request);
  const JsonValue *simmer = json_object_get(request, CONTRIBUTE_KEY_PLAYER1);
  const JsonValue *static_player =
      json_object_get(request, CONTRIBUTE_KEY_PLAYER2);
  ErrorStack *error_stack = error_stack_create();

  assert(config_get_player_sim_plies(config, 1) == 5);
  assert(config_get_player_sim_margin_forecast(config, 0));
  assert(config_get_player_sim_margin_forecast(config, 1));
  config_contribute_apply_player_settings(config, simmer, 0, error_stack);
  config_contribute_apply_player_settings(config, static_player, 1,
                                          error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_player_sim_plies(config, 0) == 4);
  assert(config_get_player_num_plays(config, 0) == 10);
  assert(config_get_player_max_iterations(config, 0) == 1000);
  // The contributor's -plies 5 does not survive a request that says static.
  assert(config_get_player_sim_plies(config, 1) == 0);
  // Nor does a margin forecast, which no request field states: it changes a
  // simmed play's equity, so it is off rather than the contributor's.
  assert(!config_get_player_sim_margin_forecast(config, 0));
  assert(!config_get_player_sim_margin_forecast(config, 1));

  // Nor does the previous task's simulation.
  config_contribute_apply_player_settings(config, static_player, 0,
                                          error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_player_sim_plies(config, 0) == 0);
  assert(config_get_player_num_plays(config, 0) == 100);

  error_stack_destroy(error_stack);
  json_destroy(games);
  config_destroy(config);
}

// Run-wide settings no request states are reset rather than inherited. The
// case this pins: a contributor whose settings.txt changes the bingo bonus
// would otherwise score every game of every job differently from the fleet.
static void test_shared_settings_do_not_leak_between_tasks(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -bb 35 -sp true -smargin true");
  assert(config_get_bingo_bonus(config) == 35);
  assert(config_get_use_small_plays(config));
  assert(config_get_sim_margin_forecast(config));

  config_contribute_reset_shared_settings(config);
  assert(config_get_bingo_bonus(config) == DEFAULT_BINGO_BONUS);
  assert(!config_get_use_small_plays(config));
  // The opening-rack analysis simulates with the run-wide settings, so the
  // run-wide margin forecast is reset as each player's is.
  assert(!config_get_sim_margin_forecast(config));

  // The bingo bonus is then the request's, not this build's default.
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *run =
      json_parse("{\"bingo_bonus\": 40, \"sim_cutoff\": 0.01}", error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_run_settings(config, run, /*states_cutoff=*/true,
                                       error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_bingo_bonus(config) == 40);

  // A request that leaves the cutoff to this build is refused...
  const JsonValue *no_cutoff = json_parse("{\"bingo_bonus\": 40}", error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_run_settings(config, no_cutoff,
                                       /*states_cutoff=*/true, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);
  // ...unless the task cannot simulate, as leave generation cannot.
  config_contribute_apply_run_settings(config, no_cutoff,
                                       /*states_cutoff=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));

  json_destroy(no_cutoff);
  json_destroy(run);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Nothing a player object leaves out is taken from this build's defaults. The
// case this pins: a server that did not state a setting got whatever default
// this release compiled in, so two releases played the same task differently
// and a version floor, being a minimum, could not keep either out.
static void test_a_player_must_state_every_setting(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  ErrorStack *error_stack = error_stack_create();

  // A static player states no simulation settings, and needs none.
  const JsonValue *static_player = json_parse(
      "{\"lexicon\": \"CSW21\", \"leaves\": \"CSW21\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0, "
      "\"endgame_plies\": 0, \"peg_max_bag\": 0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, static_player, 0,
                                          error_stack);
  assert(error_stack_is_empty(error_stack));

  // One that leaves its play count to this build is refused.
  const JsonValue *no_plays = json_parse(
      "{\"lexicon\": \"CSW21\", \"leaves\": \"CSW21\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": null, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0, "
      "\"endgame_plies\": 0, \"peg_max_bag\": 0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, no_plays, 0, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // So is a simulating player that states only what a static one must.
  const JsonValue *bare_simmer = json_parse(
      "{\"lexicon\": \"CSW21\", \"leaves\": \"CSW21\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 2, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0, "
      "\"endgame_plies\": 0, \"peg_max_bag\": 0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, bare_simmer, 0, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // And one that leaves its leaves to whatever is loaded: another job's, or
  // a leave task's fetched KLV.
  const JsonValue *no_leaves = json_parse(
      "{\"lexicon\": \"CSW21\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0, "
      "\"endgame_plies\": 0, \"peg_max_bag\": 0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, no_leaves, 0, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  json_destroy(no_leaves);
  json_destroy(bare_simmer);
  json_destroy(no_plays);
  json_destroy(static_player);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// The opening-rack executor analyses through impl_move_gen and impl_sim,
// which read the run-wide simulation settings rather than a player's. The
// case this pins: a contributor running -plies 5 analysed every rack of a
// 4-ply job at 5 plies.
static void test_opening_rack_analysis_uses_the_players_settings(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -plies 5 -numplays 7 -iterations 99");
  const JsonValue *request = NULL;
  const JsonValue *opening_rack =
      load_task_request_fixture(BIRDTEST_OPENING_RACK_FIXTURE, &request);
  const JsonValue *player = json_object_get(request, CONTRIBUTE_KEY_PLAYER);
  ErrorStack *error_stack = error_stack_create();

  config_contribute_apply_player_settings(config, player, 0, error_stack);
  assert(error_stack_is_empty(error_stack));
  // Applying the player alone leaves the run-wide settings untouched...
  assert(config_get_plies(config) == 5);

  // ...until they are copied across for the analysis.
  config_contribute_use_player_settings_for_analysis(config, 0);
  assert(config_get_plies(config) == 4);
  assert(config_get_num_plays(config) == 10);
  assert(config_get_max_iterations(config) == 1000);
  assert(config_get_stop_cond_pct(config) == 99.0);

  error_stack_destroy(error_stack);
  json_destroy(opening_rack);
  config_destroy(config);
}

// Wordmap and rack info table use are decided as a task's lexical data loads,
// so a task's flags have to be in place before it. The cases this pins: a task
// that asked for no wordmap, run after one that asked for one, loaded a wordmap
// nothing had checked against its .kwg; and a rack info table switched on by a
// contributor's settings stayed on, putting its own leave values in place of
// the leaves the task pins.
static void test_lexical_flags_are_set_before_the_load(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  PlayersData *players_data = config_get_players_data(config);
  assert(players_data_get_wmp(players_data, 0));
  // The table and the word info table flags are forced on the way a
  // contributor's settings.txt or an earlier command would leave them (-wit
  // itself refuses to set the flag without a file to load): a task that asks
  // for neither must have both switched off before the load decides what to
  // open.
  for (int player_index = 0; player_index < 2; player_index++) {
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_RIT,
                                        player_index, true);
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WIT,
                                        player_index, true);
  }
  ErrorStack *error_stack = error_stack_create();
  // A request always states its layout; this build's own is the one that
  // loads at either BOARD_DIM.
  char *layout = board_layout_get_default_name();

  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", "english", layout, "CSW21", "CSW21", "CSW21",
      "CSW21", false, false, NULL, NULL, false, false, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int player_index = 0; player_index < 2; player_index++) {
    assert(!players_data_get_wmp(players_data, player_index));
    assert(!players_data_get_rack_info_table(players_data, player_index));
    assert(!players_data_get_use_when_available(
        players_data, PLAYERS_DATA_TYPE_WIT, player_index));
  }

  // A task that asks for a wordmap gets it for itself, not for the next task.
  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", "english", layout, "CSW21", "CSW21", "CSW21",
      "CSW21", true, false, NULL, NULL, false, false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(players_data_get_wmp(players_data, 0));
  assert(!players_data_get_wmp(players_data, 1));

  // And a word info table the same way, for the player that asks: built here
  // as the worker builds it, for a lexicon small enough to take milliseconds.
  const ConversionArgs wit_args = {
      .conversion_type_string = "kwg2wit",
      .data_paths = config_get_data_paths(config),
      .input_and_output_name = "CSW21_ab",
      .ld_name = "english_ab",
      .num_threads = 1,
  };
  ConversionResults *results = conversion_results_create();
  convert(&wit_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_load_lexicon_and_variant(
      config, "CSW21_ab", "classic", "english_ab", layout, "CSW21_ab",
      "CSW21_ab", "CSW21_ab", "CSW21_ab", false, false, NULL, NULL, true, false,
      error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(players_data_get_word_info_table(players_data, 0));
  assert(!players_data_get_word_info_table(players_data, 1));
  config_contribute_load_lexicon_and_variant(
      config, "CSW21_ab", "classic", "english_ab", layout, "CSW21_ab",
      "CSW21_ab", "CSW21_ab", "CSW21_ab", false, false, NULL, NULL, false,
      false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!players_data_get_word_info_table(players_data, 0));
  assert(!players_data_get_word_info_table(players_data, 1));
  char *wit_path = data_filepaths_get_readable_filename(
      config_get_data_paths(config), "CSW21_ab",
      DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));
  (void)remove(wit_path);
  free(wit_path);
  conversion_results_destroy(results);

  free(layout);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// The letter distribution and the board layout change what a task computes as
// surely as a player's plies do, and they were the last two settings a request
// could leave to the build that ran it: absent, the distribution was inferred
// from the lexicon's name and the layout was the one named for this build's
// BOARD_DIM. birdtest states both on every request, so one that does not is
// refused, the way a player object missing a setting is.
static void test_a_request_must_state_its_distribution_and_layout(void) {
  ErrorStack *error_stack = error_stack_create();
  const char *lexicon = NULL;
  const char *variant = NULL;
  const char *letter_distribution = NULL;
  const char *board_layout = NULL;

  const JsonValue *stated = json_parse("{\"variant\":\"classic\","
                                       "\"letter_distribution\":\"english\","
                                       "\"board_layout\":\"standard15\"}",
                                       error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_contribute_validate_common(stated, &lexicon, &variant,
                                           &letter_distribution, &board_layout,
                                           error_stack));
  assert(error_stack_is_empty(error_stack));
  // Only leave generation states a top-level lexicon.
  assert(!lexicon);
  assert_strings_equal(variant, "classic");
  assert_strings_equal(letter_distribution, "english");
  assert_strings_equal(board_layout, "standard15");
  json_destroy(stated);

  const char *const incomplete[] = {
      // No distribution.
      "{\"variant\":\"classic\",\"board_layout\":\"standard15\"}",
      // No layout.
      "{\"variant\":\"classic\",\"letter_distribution\":\"english\"}",
      // A null is not a statement either.
      "{\"variant\":\"classic\",\"letter_distribution\":null,"
      "\"board_layout\":\"standard15\"}",
      // A name that would leave the data directory.
      "{\"variant\":\"classic\",\"letter_distribution\":\"../english\","
      "\"board_layout\":\"standard15\"}",

      // Every name in a player object becomes a path as well.
      "{\"variant\":\"classic\",\"letter_distribution\":\"english\","
      "\"board_layout\":\"standard15\",\"player1\":{\"leaves\":\"../x\"}}",
      "{\"variant\":\"classic\",\"letter_distribution\":\"english\","
      "\"board_layout\":\"standard15\",\"player2\":{\"rit_name\":"
      "\"../../home/x\"}}",
      "{\"variant\":\"classic\",\"letter_distribution\":\"english\","
      "\"board_layout\":\"standard15\",\"player\":{\"win_pct_model\":"
      "\"/etc/passwd\"}}",
      "{\"variant\":\"classic\",\"letter_distribution\":\"english\","
      "\"board_layout\":\"standard15\",\"player1\":{\"lexicon\":"
      "\"NWL23..x\"}}",
  };
  for (size_t i = 0; i < sizeof(incomplete) / sizeof(incomplete[0]); i++) {
    const JsonValue *request = json_parse(incomplete[i], error_stack);
    assert(error_stack_is_empty(error_stack));
    assert(!config_contribute_validate_common(request, &lexicon, &variant,
                                              &letter_distribution,
                                              &board_layout, error_stack));
    assert(!error_stack_is_empty(error_stack));
    error_stack_reset(error_stack);
    json_destroy(request);
  }

  error_stack_destroy(error_stack);

  // A rack info table's name joins a lexicon and leaves with one dot.
  assert(data_filepaths_is_safe_name("NWL23.CSW21"));
  assert(data_filepaths_is_safe_name("CSW24"));
  assert(!data_filepaths_is_safe_name(".."));
  assert(!data_filepaths_is_safe_name("a..b"));
  assert(!data_filepaths_is_safe_name("a.b.c"));
  assert(!data_filepaths_is_safe_name("a."));
  assert(!data_filepaths_is_safe_name(".a"));
  assert(!data_filepaths_is_safe_name("a/b"));
  assert(!data_filepaths_is_safe_name(""));
}

// The retry budget exists to outlast a birdtest deployment, which stops the
// one server instance before it starts the next: a minute or more of refused
// connections and load-balancer 503s. It used to be 31 seconds, after which the
// run ended -- so every deploy stopped whichever contributors asked for work
// during it. Held here to at least ten minutes, with no single wait long enough
// to leave a returned server idle for more than a minute.
static void test_http_retries_outlast_a_server_deployment(void) {
  assert(http_client_backoff_seconds(0) == 1);
  assert(http_client_backoff_seconds(1) == 2);
  assert(http_client_backoff_seconds(5) == 32);
  int total_seconds = 0;
  for (int retry_idx = 0; retry_idx < HTTP_CLIENT_MAX_TRANSIENT_RETRIES;
       retry_idx++) {
    const int wait = http_client_backoff_seconds(retry_idx);
    assert(wait >= 1);
    assert(wait <= HTTP_CLIENT_MAX_BACKOFF_SECONDS);
    if (retry_idx > 0) {
      assert(wait >= http_client_backoff_seconds(retry_idx - 1));
    }
    total_seconds += wait;
  }
  assert(total_seconds >= 600);
  // Far past the last retry, still capped rather than overflowing, and the
  // cap is at most a minute.
  const int capped = http_client_backoff_seconds(1000);
  assert(capped == HTTP_CLIENT_MAX_BACKOFF_SECONDS);
  assert(capped <= 60);

  // A 429's Retry-After is obeyed, but never for long: it was once read from
  // the wrong libcurl field, the connect time in microseconds, and a worker
  // slept for hours on its first 429.
  assert(http_client_rate_limit_wait_seconds(0) == 1);
  assert(http_client_rate_limit_wait_seconds(3) == 3);
  assert(http_client_rate_limit_wait_seconds(45000) ==
         HTTP_CLIENT_MAX_RATE_LIMIT_WAIT_SECONDS);
  // cppcheck-suppress knownConditionTrueFalse
  assert(HTTP_CLIENT_MAX_RATE_LIMIT_WAIT_SECONDS <= 60);
}

// Copies src's bytes over dst, the way contribute writes a fetched KLV.
static void copy_file_bytes(const char *src, const char *dst) {
  FILE *in = fopen_or_die(src, "rb");
  FILE *out = fopen_or_die(dst, "wb");
  char buffer[65536];
  size_t read_count;
  while ((read_count = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    fwrite_or_die(buffer, 1, read_count, out, "copied klv bytes");
  }
  fclose_or_die(in);
  fclose_or_die(out);
}

static bool klv_values_match(const KLV *a, const KLV *b) {
  if (klv_get_number_of_leaves(a) != klv_get_number_of_leaves(b)) {
    return false;
  }
  for (uint32_t i = 0; i < klv_get_number_of_leaves(a); i++) {
    if (klv_get_indexed_leave_value(a, i) !=
        klv_get_indexed_leave_value(b, i)) {
      return false;
    }
  }
  return true;
}

// A leave task writes the KLV it fetched under one fixed name and loads it.
// Lexical data is cached by name, so from the second leave task in a process
// the load found the previous generation's KLV already in memory under that
// name and played it, whatever the file now held. A file that changes on disk
// must be read again; one that has not must not be.
static void test_a_rewritten_klv_is_read_again(void) {
  const char *name = "CSW21_contribute_klv_reload";
  const char *path = "testdata/lexica/CSW21_contribute_klv_reload.klv2";
  KLV *csw21 = klv_create_or_die(DEFAULT_TEST_DATA_PATH, "CSW21");
  KLV *csw24 = klv_create_or_die(DEFAULT_TEST_DATA_PATH, "CSW24");
  assert(!klv_values_match(csw21, csw24));

  Config *config = config_create_or_die("set -lex CSW21");
  const PlayersData *players_data = config_get_players_data(config);
  ErrorStack *error_stack = error_stack_create();
  // A request always states its distribution and layout, which are never NULL
  // on this path; this build's own layout is the one that loads at either
  // BOARD_DIM.
  char *layout = board_layout_get_default_name();

  // From wherever the test data path resolves each name, which is where the
  // two reference KLVs above were read from.
  char *csw21_path = data_filepaths_get_readable_filename(
      DEFAULT_TEST_DATA_PATH, "CSW21", DATA_FILEPATH_TYPE_KLV, error_stack);
  char *csw24_path = data_filepaths_get_readable_filename(
      DEFAULT_TEST_DATA_PATH, "CSW24", DATA_FILEPATH_TYPE_KLV, error_stack);
  assert(error_stack_is_empty(error_stack));

  copy_file_bytes(csw21_path, path);
  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", "english", layout, NULL, NULL, name, name,
      false, false, NULL, NULL, false, false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(klv_values_match(players_data_get_klv(players_data, 0), csw21));
  const KLV *first = players_data_get_klv(players_data, 0);

  // Unchanged on disk: the loaded copy is kept.
  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", "english", layout, NULL, NULL, name, name,
      false, false, NULL, NULL, false, false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(players_data_get_klv(players_data, 0) == first);

  // The next generation's artifact, written under the same name.
  copy_file_bytes(csw24_path, path);
  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", "english", layout, NULL, NULL, name, name,
      false, false, NULL, NULL, false, false, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int player_index = 0; player_index < 2; player_index++) {
    assert(klv_values_match(players_data_get_klv(players_data, player_index),
                            csw24));
  }

  error_stack_destroy(error_stack);
  config_destroy(config);
  klv_destroy(csw21);
  klv_destroy(csw24);
  free(layout);
  free(csw21_path);
  free(csw24_path);
  (void)remove(path);
}

static void touch_file(const char *path, time_t age_seconds) {
  FILE *file = fopen_or_die(path, "wb");
  fwrite_or_die("x", 1, 1, file, "temporary test byte");
  fclose_or_die(file);
  if (age_seconds > 0) {
    const time_t then = time(NULL) - age_seconds;
    // NOLINTNEXTLINE(misc-include-cleaner)
    struct utimbuf times = {.actime = then, .modtime = then};
    assert(utime(path, &times) == 0);
  }
}

static bool file_exists(const char *path) {
  // NOLINTNEXTLINE(misc-include-cleaner)
  struct stat info;
  return stat(path, &info) == 0;
}

// A writer killed mid-write leaves its whole temporary behind (a rack info
// table's is 1.9 GB); the next write of the same file removes it, and nothing
// that could still be in progress or belongs to another name.
static void test_an_abandoned_temporary_is_removed(void) {
  const char *target = "contribute_test_table.rit";
  const char *abandoned = "contribute_test_table.rit.12345-0.tmp";
  const char *abandoned_old_form = "contribute_test_table.rit.12345.tmp";
  const char *in_progress = "contribute_test_table.rit.23456.tmp";
  const char *not_ours = "contribute_test_table.rit.backup.tmp";
  const char *other_file = "contribute_test_other.rit.12345.tmp";
  touch_file(abandoned, (time_t)2 * 60 * 60);
  touch_file(abandoned_old_form, (time_t)2 * 60 * 60);
  touch_file(in_progress, 0);
  touch_file(not_ours, (time_t)2 * 60 * 60);
  touch_file(other_file, (time_t)2 * 60 * 60);

  char *temporary = temporary_sibling(target);
  assert(!file_exists(abandoned));
  assert(!file_exists(abandoned_old_form));
  // Two writes in one process get two names.
  char *second = temporary_sibling(target);
  assert(!strings_equal(temporary, second));
  free(second);
  assert(file_exists(in_progress));
  assert(file_exists(not_ours));
  assert(file_exists(other_file));
  free(temporary);

  (void)remove(in_progress);
  (void)remove(not_ours);
  (void)remove(other_file);
}

// A run's state starts with every field set: fields added to it without a
// line in contribute_state_create were read uninitialized and freed at the
// end of every run (the sanitizer build's allocator fills new memory, so a
// garbage pointer fails here). A leave KLV's mismatch is recorded without
// touching anything else.
static void test_a_runs_state_starts_clean(void) {
  const char *path = "contribute_test_state_settings.txt";
  write_settings_file(path, "server https://birdtest.example\n");
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(path, thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(state);
  assert(!contribute_should_stop(state));
  contribute_record_derived_mismatch(state, "klv", "leaves/x/generation-1.klv2",
                                     "expected", NULL);
  contribute_artifact_verified(state);
  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// A run starts by saying which settings it uses and which of them are
// defaults -- the sign of a run from the wrong folder, which with the file
// optional is a new anonymous worker rather than an error -- and says only
// whether there is an API key, never the key.
// With `path` NULL, the default file, which the caller has checked is not
// there.
static char *settings_printed_for(const char *path, const char *contents) {
  if (contents) {
    write_settings_file(path, contents);
  }
  FILE *captured = tmpfile();
  assert(captured);
  io_set_stream_out(captured);
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(path, thread_control, error_stack);
  io_reset_stream_out();
  assert(error_stack_is_empty(error_stack));
  assert(state);
  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  if (path) {
    (void)remove(path);
  }
  rewind(captured);
  char *printed =
      get_string_from_file_handle(captured, "(captured)", error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return printed;
}

static void test_a_run_says_which_settings_are_defaults(void) {
  const char *path = "contribute_test_printed_settings.txt";

  // Skipped where the working directory has a contribute.txt of its own.
  if (access(CONTRIBUTE_SETTINGS_DEFAULT_FILENAME, F_OK) != 0) {
    char *printed = settings_printed_for(NULL, NULL);
    assert(strstr(printed, CONTRIBUTE_SETTINGS_DEFAULT_FILENAME
                  " not found; every setting is its default"));
    assert(strstr(printed,
                  "  server   " CONTRIBUTE_DEFAULT_SERVER " (default)\n"));
    assert(strstr(printed, "  apikey   none (anonymous)\n"));
    assert(strstr(printed, "  uuid     none yet"));
    assert(strstr(printed, "(default)\n  maxtasks 0 (default: no limit)\n"));
    assert(strstr(printed, "  idlewait 5 (default)\n"));
    free(printed);
  }

  char *printed = settings_printed_for(path, "apikey bt_SECRETKEY123\n");
  assert(!strstr(printed, "not found"));
  assert(
      strstr(printed, "  server   " CONTRIBUTE_DEFAULT_SERVER " (default)\n"));
  assert(strstr(printed, "  apikey   set\n"));
  assert(!strstr(printed, "SECRETKEY"));
  free(printed);

  printed = settings_printed_for(path, "server http://127.0.0.1:9\n"
                                       "threads 2\nmaxtasks 3\nidlewait 4\n");
  assert(strstr(printed, "  server   http://127.0.0.1:9\n"));
  assert(strstr(printed, "  threads  2\n"));
  assert(strstr(printed, "  maxtasks 3\n"));
  assert(strstr(printed, "  idlewait 4\n"));
  assert(!strstr(printed, "default"));
  free(printed);
}

// contribute.txt's thread count is capped where a task can still get a move
// generator on every thread it runs: at N threads a simulated game per thread
// simulates on N more, and past move generation's pool magpie exits on the
// task. 512, the CLI's cap, was not enough.
static void test_a_runs_threads_are_capped(void) {
  assert(2 * CONTRIBUTE_MAX_THREADS + 1 <= MAX_THREADS);
  const char *path = "contribute_test_threads_settings.txt";
  write_settings_file(path, "server https://birdtest.example\nthreads 5000\n");
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(path, thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(contribute_get_threads(state) == CONTRIBUTE_MAX_THREADS);
  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// A job whose server KLV is missing or wrong is set aside for a doubling
// interval, named in the claim's unsupported list meanwhile (after the jobs
// set aside for good), and the claim says it named one -- which is what lets
// a data shutdown answered because of it be waited out, not obeyed.
static void test_a_set_aside_job_is_left_out_of_claims_for_a_while(void) {
  const char *path = "contribute_test_defer_settings.txt";
  write_settings_file(path, "server https://birdtest.example\nidlewait 3\n");
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(path, thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));

  char *body = contribute_claim_body(state, "0.1.1");
  char *expected_body = get_formatted_string(
      "{\"magpie_version\":\"0.1.1\",\"board_dim\":%d,\"rack_size\":%d,"
      "\"unsupported_jobs\":[]}",
      BOARD_DIM, RACK_SIZE);
  assert_strings_equal(body, expected_body);
  free(expected_body);
  free(body);

  assert(contribute_defer_job(state, "job-a") == 3);
  assert(contribute_defer_job(state, "job-a") == 6);
  body = contribute_claim_body(state, "0.1.1");
  assert(strstr(body, "\"unsupported_jobs\":[\"job-a\"]"));
  free(body);
  for (int i = 0; i < 20; i++) {
    (void)contribute_defer_job(state, "job-b");
  }
  assert(contribute_defer_job(state, "job-b") ==
         CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS);
  body = contribute_claim_body(state, "0.1.1");
  assert(strstr(body, "\"job-a\",\"job-b\"") ||
         strstr(body, "\"job-b\",\"job-a\""));
  free(body);

  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// The wait-or-exit decision on a shutdown, against birdtest's own shutdown
// fixtures: a data shutdown answering a claim that named a set-aside job is
// waited out; a version or build shutdown never is; and nothing is waited out
// for a claim that named none. A reason renamed on either side fails here
// rather than sending every worker away with the wrong advice.
static void test_only_a_data_shutdown_is_waited_out_for_a_set_aside_job(void) {
  const struct {
    const char *path;
    bool waits;
  } cases[] = {
      {"test/birdtest_contract/shutdown-data-out-of-date.json", true},
      {"test/birdtest_contract/shutdown-both.json", true},
      {"test/birdtest_contract/shutdown-magpie-too-old.json", false},
      {"test/birdtest_contract/shutdown-unsupported-build.json", false},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const JsonValue *fixture = load_fixture(cases[i].path);
    const JsonValue *shutdown = json_object_get(fixture, "shutdown");
    assert(shutdown);
    assert(contribute_shutdown_waits_for_deferral(shutdown, true) ==
           cases[i].waits);
    assert(!contribute_shutdown_waits_for_deferral(shutdown, false));
    json_destroy(fixture);
  }
}

// An assignment whose input data this build cannot check is refused, not run
// unverified: one with no `expected_data`, one whose digests use an
// algorithm other than sha256, and one listing a file it cannot check. Every
// fixture assignment passes.
static void test_an_unverifiable_assignment_is_refused(void) {
  ErrorStack *error_stack = error_stack_create();
  const char *const refused[] = {
      "{\"claim_token\":\"t\",\"job_id\":\"j\"}",
      "{\"expected_data\":{\"files\":[]}}",
      "{\"expected_data\":{\"algorithm\":\"blake3\",\"files\":[]}}",
      // A file list this build cannot read is no check at all.
      "{\"expected_data\":{\"algorithm\":\"sha256\"}}",
      // An entry missing its digest, or naming a role this build does not
      // know, was skipped, and the file it named loaded unchecked.
      "{\"expected_data\":{\"algorithm\":\"sha256\",\"files\":["
      "{\"role\":\"kwg\",\"name\":\"CSW21\",\"sha256\":\"ab\"},"
      "{\"role\":\"klv\",\"name\":\"CSW21\"}]}}",
      "{\"expected_data\":{\"algorithm\":\"sha256\",\"files\":["
      "{\"role\":\"lexicon\",\"name\":\"CSW21\",\"sha256\":\"ab\"}]}}",
      "{\"expected_data\":{\"algorithm\":\"sha256\",\"files\":["
      "{\"name\":\"CSW21\",\"sha256\":\"ab\"}]}}",
  };
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
    const JsonValue *assignment = json_parse(refused[i], error_stack);
    assert(error_stack_is_empty(error_stack));
    contribute_check_expected_data(assignment, error_stack);
    assert(error_stack_top(error_stack) ==
           ERROR_STATUS_CONTRIBUTE_SERVER_ERROR);
    error_stack_reset(error_stack);
    json_destroy(assignment);
  }
  const char *const fixtures[] = {
      BIRDTEST_GAMES_FIXTURE, BIRDTEST_GAME_PAIRS_FIXTURE,
      BIRDTEST_OPENING_RACK_FIXTURE, BIRDTEST_LEAVE_FIXTURE};
  for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
    const JsonValue *assignment = load_fixture(fixtures[i]);
    contribute_check_expected_data(assignment, error_stack);
    assert(error_stack_is_empty(error_stack));
    json_destroy(assignment);
  }
  error_stack_destroy(error_stack);
}

// The claim body has the shape of birdtest's claim-request fixture: built
// from the fixture's own version and ids, it has the same keys and the same
// values -- except the board dimension and rack size, which are this build's
// (BOARD_DIM and RACK_SIZE), whatever the fixture's build was.
static void test_the_claim_body_matches_the_claim_fixture(void) {
  const char *path = "contribute_test_claim_settings.txt";
  write_settings_file(path, "server https://birdtest.example\n");
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();
  ContributeState *state =
      contribute_state_create(path, thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  const JsonValue *fixture =
      load_fixture("test/birdtest_contract/claim-request.json");
  const JsonValue *fixture_jobs = json_object_get(fixture, "unsupported_jobs");
  const int job_count = json_array_length(fixture_jobs);
  for (int i = 0; i < job_count; i++) {
    (void)contribute_defer_job(state, json_array_get_string(fixture_jobs, i));
  }
  char *body = contribute_claim_body(
      state, json_get_string_or_null(fixture, "magpie_version"));
  const JsonValue *ours = json_parse(body, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(json_object_size(ours) == json_object_size(fixture));
  assert_strings_equal(json_get_string_or_null(ours, "magpie_version"),
                       json_get_string_or_null(fixture, "magpie_version"));
  assert(json_get_int_or(fixture, "board_dim", 0) > 0);
  assert(json_get_int_or(fixture, "rack_size", 0) > 0);
  assert(json_get_int_or(ours, "board_dim", 0) == BOARD_DIM);
  assert(json_get_int_or(ours, "rack_size", 0) == RACK_SIZE);
  const JsonValue *jobs = json_object_get(ours, "unsupported_jobs");
  assert(json_array_length(jobs) == job_count);
  for (int i = 0; i < job_count; i++) {
    assert_strings_equal(json_array_get_string(jobs, i),
                         json_array_get_string(fixture_jobs, i));
  }
  json_destroy(fixture);
  json_destroy(ours);
  free(body);
  contribute_state_destroy(state);
  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(path);
}

// The decline body, against birdtest's decline-missing-data.json, built the
// way a missing_data decline builds it: one entry per file the fixture names,
// one not found and one found with other bytes. The same keys at every depth
// and the same values -- except that a file not found has no "actual" here,
// which birdtest reads as the fixture's null. birdtest parses the same file
// as the body it accepts, so a key renamed on either side fails a test.
static void test_the_decline_body_matches_the_decline_fixture(void) {
  const JsonValue *fixture = load_fixture(BIRDTEST_DECLINE_FIXTURE);
  const JsonValue *fixture_missing = json_object_get(fixture, "missing");
  const int missing_count = json_array_length(fixture_missing);
  StringBuilder *sb = string_builder_create();
  bool names_an_absent_file = false;
  bool names_a_mismatched_file = false;
  for (int missing_idx = 0; missing_idx < missing_count; missing_idx++) {
    const JsonValue *file = json_array_get(fixture_missing, missing_idx);
    const char *actual = json_get_string_or_null(file, "actual");
    if (actual) {
      names_a_mismatched_file = true;
    } else {
      names_an_absent_file = true;
    }
    if (missing_idx > 0) {
      string_builder_add_string(sb, ",");
    }
    char *entry = contribute_missing_file_json(
        json_get_string_or_null(file, "role"),
        json_get_string_or_null(file, "name"),
        json_get_string_or_null(file, "expected"), actual);
    string_builder_add_string(sb, entry);
    free(entry);
  }
  assert(names_an_absent_file && names_a_mismatched_file);
  char *missing_json = string_builder_dump_and_destroy(sb, NULL);
  char *body = contribute_decline_body(
      json_get_string_or_null(fixture, "claim_token"),
      json_get_string_or_null(fixture, "reason"), missing_json);
  free(missing_json);

  ErrorStack *error_stack = error_stack_create();
  const JsonValue *ours = json_parse(body, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_produces_every_fixture_key(fixture, ours, BIRDTEST_DECLINE_FIXTURE);
  assert(json_object_size(ours) == json_object_size(fixture));
  assert_strings_equal(json_get_string_or_null(ours, "claim_token"),
                       json_get_string_or_null(fixture, "claim_token"));
  assert_strings_equal(json_get_string_or_null(ours, "reason"),
                       json_get_string_or_null(fixture, "reason"));
  const JsonValue *our_missing = json_object_get(ours, "missing");
  assert(json_array_length(our_missing) == missing_count);
  const char *const entry_keys[] = {"role", "name", "expected"};
  for (int missing_idx = 0; missing_idx < missing_count; missing_idx++) {
    const JsonValue *mine = json_array_get(our_missing, missing_idx);
    const JsonValue *theirs = json_array_get(fixture_missing, missing_idx);
    for (size_t key_idx = 0;
         key_idx < sizeof(entry_keys) / sizeof(entry_keys[0]); key_idx++) {
      assert_strings_equal(
          json_get_string_or_null(mine, entry_keys[key_idx]),
          json_get_string_or_null(theirs, entry_keys[key_idx]));
    }
    const char *actual = json_get_string_or_null(theirs, "actual");
    if (actual) {
      assert(json_object_size(mine) == json_object_size(theirs));
      assert_strings_equal(json_get_string_or_null(mine, "actual"), actual);
    } else {
      assert(!json_object_get(mine, "actual"));
    }
  }
  json_destroy(ours);
  json_destroy(fixture);
  free(body);
  error_stack_destroy(error_stack);
}

// contribute's tasks run in a config of their own. The caller's session and
// the settings file the REPL saves from it stay as they were: a task's
// lexicon and derived-file flags -- a wordmap or word info table this machine
// lacks -- never reach them, so there is nothing to snapshot or undo. (Undone
// by replaying a snapshot, a replay that failed part way left the session
// unable to load and, one save later, the task's lexicon in settings.txt.)
static void test_contribute_runs_in_a_config_of_its_own(void) {
  Config *config = config_create_default_test();
  load_and_exec_config_or_die(config, "set -lex CSW21 -numplays 7");
  StringBuilder *before = string_builder_create();
  config_add_settings_to_string_builder(config, before);
  ErrorStack *error_stack = error_stack_create();

  Config *task_config = config_create_for_contribute(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(task_config);
  assert(config_get_thread_control(task_config) ==
         config_get_thread_control(config));
  assert(!config_get_save_settings(task_config));
  // What a task does: another lexicon, with every derived file asked for
  // and none of them on this machine for it.
  load_and_exec_config_or_die(
      task_config, "set -lex OSPS49 -wmp false -rit false -numplays 3");
  config_destroy_for_contribute(task_config);

  StringBuilder *after = string_builder_create();
  config_add_settings_to_string_builder(config, after);
  assert(
      strings_equal(string_builder_peek(before), string_builder_peek(after)));
  // And the caller's session still loads.
  load_and_exec_config_or_die(config, "set -numplays 8");

  string_builder_destroy(after);
  string_builder_destroy(before);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// contribute maps a rack info table unless its own command says not to. A
// saved settings file's `-ritmmap false` -- which every saved one records --
// does not count: only the contribute command's argument does.
static void test_contribute_maps_rack_info_tables_unless_told_not_to(void) {
  Config *config = config_create_default_test();
  // What loading a saved settings.txt does.
  load_and_exec_config_or_die(config, "set -ritmmap false");
  ErrorStack *error_stack = error_stack_create();

  config_load_command(config, "contribute unused.txt", error_stack);
  assert(error_stack_is_empty(error_stack));
  Config *task_config = config_create_for_contribute(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_use_mmap_for_rit(task_config) == IS_LITTLE_ENDIAN);
  config_destroy_for_contribute(task_config);

  config_load_command(config, "contribute unused.txt -ritmmap false",
                      error_stack);
  assert(error_stack_is_empty(error_stack));
  task_config = config_create_for_contribute(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!config_get_use_mmap_for_rit(task_config));
  config_destroy_for_contribute(task_config);

  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Workers sharing a data directory build a derived file one at a time: a
// second build of the same file waits for the first, and a stop request ends
// the wait rather than the build going ahead.
static void test_a_derived_build_is_held_by_one_process(void) {
  const char *output_path = "contribute_test_build.rit";
  char *lock_path = get_formatted_string("%s.lock", output_path);
  ErrorStack *error_stack = error_stack_create();
  ThreadControl *thread_control = thread_control_create();

  const int held = contribute_lock_build(output_path, "the test table",
                                         thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(held >= 0);

  // A second open of the lock is another holder, as another process is.
  thread_control_set_status(thread_control,
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  const int waited = contribute_lock_build(output_path, "the test table",
                                           thread_control, error_stack);
  assert(waited == -1);
  assert(error_stack_top(error_stack) == ERROR_STATUS_CONTRIBUTE_INTERRUPTED);
  error_stack_reset(error_stack);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);

  contribute_unlock_build(held);
  const int again = contribute_lock_build(output_path, "the test table",
                                          thread_control, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(again >= 0);
  contribute_unlock_build(again);
  contribute_unlock_build(-1);

  thread_control_destroy(thread_control);
  error_stack_destroy(error_stack);
  (void)remove(lock_path);
  free(lock_path);
}

// A simulating player ranks every play up to its num_plays, whatever its
// recorder -- as autoplay's simulating player does. The case this pins: an
// opening-rack job's simulating player with a `best` recorder (birdtest takes
// it with num_plays_recorded 1) generated one candidate, so the "simulation"
// reported the static top play for every rack.
static void test_a_simulating_rack_analysis_ranks_every_play(void) {
  Config *config = config_create_or_die("set -lex CSW21 -numplays 8 -r1 best");
  ErrorStack *error_stack = error_stack_create();

  // Static: the recorder's one best play is the answer.
  assert(config_contribute_generate_for_rack(config, "AEGINRV", 1, false,
                                             error_stack));
  assert(move_list_get_count(config_get_move_list(config)) == 1);

  // Simulating: every candidate, up to num_plays.
  assert(config_contribute_generate_for_rack(config, "AEGINRV", 1, true,
                                             error_stack));
  assert(error_stack_is_empty(error_stack));
  assert(move_list_get_count(config_get_move_list(config)) == 8);

  // An unusable rack is refused either way.
  assert(
      !config_contribute_generate_for_rack(config, "", 1, true, error_stack));
  assert(!error_stack_is_empty(error_stack));

  error_stack_destroy(error_stack);
  config_destroy(config);
}

// A layout comes from the server's data; a byte above 0x7f was a negative
// index into the square table, and `\xc3` read as a square.
static void test_a_high_byte_is_not_a_bonus_square(void) {
  for (int byte = 0x80; byte <= 0xff; byte++) {
    assert(bonus_square_is_invalid(bonus_square_from_char((char)byte)));
  }
  assert(!bonus_square_is_invalid(bonus_square_from_char('=')));
}

// A static player's games are a function of the seed alone, whatever the thread
// count: birdtest relies on it for game pairs. Simulating and
// solving players are multithreaded and make no such promise; this pins that
// adding the solvers did not disturb the static case.
static void test_static_games_are_identical_across_thread_counts(void) {
  const int thread_counts[] = {1, 2, 8};
  char *first = NULL;
  for (size_t i = 0; i < sizeof(thread_counts) / sizeof(thread_counts[0]);
       i++) {
    char *settings = get_formatted_string(
        "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
        "-threads %d",
        thread_counts[i]);
    Config *config = config_create_or_die(settings);
    free(settings);
    load_and_exec_config_or_die(config, "autoplay games 8 -seed 41");
    ErrorStack *error_stack = error_stack_create();
    const JsonValue *result = json_parse(
        autoplay_results_get_json(config_get_autoplay_results(config), false),
        error_stack);
    assert(error_stack_is_empty(error_stack));
    const JsonValue *games = json_object_get(result, "all_games");
    char *summary = get_formatted_string(
        "%lld %lld %lld %lld %.6f %.6f %.6f %.6f",
        (long long)json_get_int(games, "games", error_stack),
        (long long)json_get_int(games, "wins", error_stack),
        (long long)json_get_int(games, "losses", error_stack),
        (long long)json_get_int(games, "ties", error_stack),
        json_get_double(games, "p1_score_mean", error_stack),
        json_get_double(games, "p1_score_sd", error_stack),
        json_get_double(games, "p2_score_mean", error_stack),
        json_get_double(games, "p2_score_sd", error_stack));
    assert(error_stack_is_empty(error_stack));
    if (first == NULL) {
      first = summary;
    } else {
      assert_strings_equal(first, summary);
      free(summary);
    }
    json_destroy(result);
    error_stack_destroy(error_stack);
    config_destroy(config);
  }
  free(first);
}

// A player's endgame and pre-endgame settings come from the request alone,
// and a request that states a contradictory or partial set is refused. The
// cases this pins: a request that leaves the switches out playing whatever
// this build defaults to, and a player told to run the pre-endgame without
// solving endgames, which PEG needs.
static void test_a_player_states_its_solving(void) {
  Config *config = config_create_or_die("set -lex CSW21 -eplies1 5 -pegbag1 3");
  ErrorStack *error_stack = error_stack_create();
  const char *const base =
      "\"lexicon\": \"CSW21\", \"leaves\": \"CSW21\", "
      "\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0";
  const char *const peg =
      "\"peg_stage_top_k\": [8, 4, 2], \"peg_scenario_stride\": 3, "
      "\"peg_opp_model\": \"pessimistic\"";
  const char *const nested =
      "\"peg_nested_cand_caps\": [6, 3], \"peg_nested_max_depth\": 2, "
      "\"peg_nested_strides\": [1, 2, 3, 4]";
  // A contributor's own -eplies1/-pegbag1 do not survive a request.
  assert(config_get_player_solver_settings(config, 0)->endgame_plies == 5);
  struct {
    const char *rest;
    bool accepted;
  } cases[] = {
      // Neither switch stated: refused, not defaulted.
      {"", false},
      {", \"endgame_plies\": 0", false},
      // Off, which is what a player that states nothing else is.
      {", \"endgame_plies\": 0, \"peg_max_bag\": 0", true},
      // The endgame alone needs nothing more.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 0", true},
      // PEG without endgame solving is refused.
      {", \"endgame_plies\": 0, \"peg_max_bag\": 2", false},
      // Out of range.
      {", \"endgame_plies\": 26, \"peg_max_bag\": 0", false},
      {", \"endgame_plies\": 4, \"peg_max_bag\": 5", false},
      // A PEG setting a player that runs no PEG states is refused.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 0, "
       "\"peg_scenario_stride\": 1",
       false},
      // PEG with only some of its settings is refused.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_scenario_stride\": 1",
       false},
      // A stage that keeps one play, or a zero stride, is refused.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_stage_top_k\": [4, 1], \"peg_scenario_stride\": 1, "
       "\"peg_opp_model\": \"rational\", \"peg_nested\": false",
       false},
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_stage_top_k\": [4, 2], \"peg_scenario_stride\": 0, "
       "\"peg_opp_model\": \"rational\", \"peg_nested\": false",
       false},
      // Nested settings without nested lookahead are refused...
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_stage_top_k\": [4, 2], \"peg_scenario_stride\": 1, "
       "\"peg_opp_model\": \"rational\", \"peg_nested\": false, "
       "\"peg_nested_max_depth\": 1",
       false},
      // ...and nested lookahead without them.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_stage_top_k\": [4, 2], \"peg_scenario_stride\": 1, "
       "\"peg_opp_model\": \"rational\", \"peg_nested\": true",
       false},
      // Strides for every bag size or none.
      {", \"endgame_plies\": 4, \"peg_max_bag\": 2, "
       "\"peg_stage_top_k\": [4, 2], \"peg_scenario_stride\": 1, "
       "\"peg_opp_model\": \"rational\", \"peg_nested\": true, "
       "\"peg_nested_cand_caps\": [2], \"peg_nested_max_depth\": 1, "
       "\"peg_nested_strides\": [1, 1, 5]",
       false},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *text = get_formatted_string("{%s%s}", base, cases[i].rest);
    const JsonValue *player = json_parse(text, error_stack);
    assert(error_stack_is_empty(error_stack));
    config_contribute_apply_player_settings(config, player, 0, error_stack);
    if (error_stack_is_empty(error_stack) != cases[i].accepted) {
      log_fatal("solver settings case %d: expected %s: %s", (int)i,
                cases[i].accepted ? "accepted" : "refused", text);
    }
    error_stack_reset(error_stack);
    json_destroy(player);
    free(text);
  }

  // A full nested PEG player is applied as stated.
  char *text =
      get_formatted_string("{%s, \"endgame_plies\": 7, \"peg_max_bag\": 3, %s, "
                           "\"peg_nested\": true, %s}",
                           base, peg, nested);
  const JsonValue *player = json_parse(text, error_stack);
  free(text);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, player, 1, error_stack);
  assert(error_stack_is_empty(error_stack));
  const AutoplaySolverSettings *settings =
      config_get_player_solver_settings(config, 1);
  assert(settings->endgame_plies == 7);
  assert(settings->peg_max_bag == 3);
  assert(settings->peg_num_stages == 3);
  assert(settings->peg_stage_top_k[0] == 8 &&
         settings->peg_stage_top_k[2] == 2);
  assert(settings->peg_scenario_stride == 3);
  assert(settings->peg_pessimistic);
  assert(settings->peg_nested);
  assert(settings->peg_nested_num_cand_caps == 2);
  assert(settings->peg_nested_cand_caps[1] == 3);
  assert(settings->peg_nested_max_depth == 2);
  assert(settings->peg_nested_strides[1] == 1);
  assert(settings->peg_nested_strides[4] == 4);
  json_destroy(player);

  // And the next task's player starts from off, not from this one.
  text = get_formatted_string("{%s, \"endgame_plies\": 0, \"peg_max_bag\": 0}",
                              base);
  player = json_parse(text, error_stack);
  free(text);
  config_contribute_apply_player_settings(config, player, 1, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_player_solver_settings(config, 1)->endgame_plies == 0);
  assert(config_get_player_solver_settings(config, 1)->peg_max_bag == 0);
  json_destroy(player);

  error_stack_destroy(error_stack);
  config_destroy(config);
}

// PEG scores its emptier scenarios with endgame solves, so endgame_plies 0
// turns it off however large peg_max_bag is.
static void test_no_endgame_depth_means_no_pre_endgame(void) {
  AutoplaySolverSettings settings;
  autoplay_solver_settings_set_defaults(&settings);
  settings.peg_max_bag = PEG_MAX_BAG;
  assert(!autoplay_solver_settings_solves(&settings));
  for (int bag = 0; bag <= PEG_MAX_BAG + 1; bag++) {
    assert(!autoplay_solver_settings_runs_peg(&settings, bag));
  }
  settings.endgame_plies = 2;
  assert(autoplay_solver_settings_solves(&settings));
  assert(!autoplay_solver_settings_runs_peg(&settings, 0));
  for (int bag = PEG_MIN_BAG; bag <= PEG_MAX_BAG; bag++) {
    assert(autoplay_solver_settings_runs_peg(&settings, bag));
  }
  assert(!autoplay_solver_settings_runs_peg(&settings, PEG_MAX_BAG + 1));
  settings.peg_max_bag = 2;
  assert(autoplay_solver_settings_runs_peg(&settings, 2));
  assert(!autoplay_solver_settings_runs_peg(&settings, 3));

  // The same through the CLI: -pegbag1 without -eplies1 solves nothing.
  Config *config = config_create_or_die("set -lex CSW21 -pegbag1 4");
  assert(!autoplay_solver_settings_solves(
      config_get_player_solver_settings(config, 0)));
  config_destroy(config);
}

// The CLI options round-trip through saved settings, so a run saved by one
// session is the run the next one plays.
static void test_solver_options_round_trip(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -eplies2 3 -pegbag2 2 -pegtopk2 6,3 -pegstride2 2 "
      "-pegpess2 true -pegnested2 false -pegncaps 5,2 -pegndepth 2 "
      "-pegnstrides 1,1,4,6");
  StringBuilder *sb = string_builder_create();
  config_add_settings_to_string_builder(config, sb);
  Config *reloaded = config_create_or_die(string_builder_peek(sb));
  string_builder_destroy(sb);
  for (int player_index = 0; player_index < 2; player_index++) {
    const AutoplaySolverSettings *a =
        config_get_player_solver_settings(config, player_index);
    const AutoplaySolverSettings *b =
        config_get_player_solver_settings(reloaded, player_index);
    assert(a->endgame_plies == b->endgame_plies);
    assert(a->peg_max_bag == b->peg_max_bag);
    assert(a->peg_num_stages == b->peg_num_stages);
    for (int i = 0; i < a->peg_num_stages; i++) {
      assert(a->peg_stage_top_k[i] == b->peg_stage_top_k[i]);
    }
    assert(a->peg_scenario_stride == b->peg_scenario_stride);
    assert(a->peg_pessimistic == b->peg_pessimistic);
    assert(a->peg_nested == b->peg_nested);
    assert(a->peg_nested_num_cand_caps == b->peg_nested_num_cand_caps);
    assert(a->peg_nested_max_depth == b->peg_nested_max_depth);
    for (int bag = 1; bag <= PEG_MAX_BAG; bag++) {
      assert(a->peg_nested_strides[bag] == b->peg_nested_strides[bag]);
    }
  }
  const AutoplaySolverSettings *p2 =
      config_get_player_solver_settings(config, 1);
  assert(p2->endgame_plies == 3 && p2->peg_max_bag == 2);
  assert(p2->peg_num_stages == 2 && p2->peg_stage_top_k[1] == 3);
  assert(p2->peg_pessimistic && !p2->peg_nested);
  assert(p2->peg_nested_strides[3] == 4);
  assert(config_get_player_solver_settings(config, 0)->endgame_plies == 0);
  config_destroy(reloaded);
  config_destroy(config);

  // A stage that keeps one play is refused, as is a short stride list.
  Config *bad = config_create_default_test();
  assert_config_exec_status(bad, "set -pegtopk1 4,1",
                            ERROR_STATUS_AUTOPLAY_INVALID_SOLVER_SETTINGS);
  assert_config_exec_status(bad, "set -pegnstrides 1,1,5",
                            ERROR_STATUS_AUTOPLAY_INVALID_SOLVER_SETTINGS);
  config_destroy(bad);
}

// A player that solves plays its endgame and pre-endgame turns with the
// solvers, and a captured position on such a turn reports the solver's ranking:
// best first, with each play's projected final spread and the depth it was
// ranked at, and a PEG play's win percentage. The rest of the game is captured
// as before.
static void test_solving_players_report_their_solves(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 best -r2 best "
      "-threads 2 -maxnumdplays 5 -eplies1 2 -eplies2 2 -pegbag1 2 -pegbag2 2 "
      "-pegtopk1 2 -pegtopk2 2 -pegnested1 false -pegnested2 false");
  load_and_exec_config_or_die(config, "autoplay games,positions 4 -seed 7");
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *result = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), false),
      error_stack);
  assert(error_stack_is_empty(error_stack));
  const JsonValue *positions = json_object_get(result, "positions");
  int counts[4] = {0};
  for (int i = 0; i < json_array_length(positions); i++) {
    const JsonValue *position = json_array_get(positions, i);
    const char *analysis = json_get_string_or_null(position, "analysis");
    const JsonValue *moves = json_object_get(position, "moves");
    assert(json_array_length(moves) > 0);
    if (strings_equal(analysis, CONTRIBUTE_ANALYSIS_STATIC)) {
      counts[0]++;
      assert(!json_object_get(json_array_get(moves, 0), "mean_spread"));
      continue;
    }
    assert(strings_equal(analysis, CONTRIBUTE_ANALYSIS_PEG) ||
           strings_equal(analysis, CONTRIBUTE_ANALYSIS_ENDGAME));
    const bool is_peg = strings_equal(analysis, CONTRIBUTE_ANALYSIS_PEG);
    counts[is_peg ? 2 : 3]++;
    // An endgame position reports the one play its solve chose.
    if (!is_peg) {
      assert(json_array_length(moves) == 1);
    }
    for (int m = 0; m < json_array_length(moves); m++) {
      const JsonValue *move = json_array_get(moves, m);
      const double spread = json_get_double(move, "mean_spread", error_stack);
      const int64_t fidelity =
          json_get_int(move, "fidelity_plies", error_stack);
      assert(error_stack_is_empty(error_stack));
      assert(spread > -1000.0 && spread < 1000.0);
      assert(fidelity >= 0 && fidelity <= 25);
      if (is_peg) {
        const double win = json_get_double(move, "win_percentage", error_stack);
        assert(error_stack_is_empty(error_stack));
        assert(win >= 0.0 && win <= 100.0);
        // Best first: no deeper-ranked play follows a shallower one.
        if (m > 0) {
          assert(json_get_int(json_array_get(moves, m - 1), "fidelity_plies",
                              error_stack) >= fidelity);
        }
      } else {
        assert(!json_object_get(move, "win_percentage"));
      }
    }
  }
  assert(counts[0] > 0);
  assert(counts[2] > 0);
  assert(counts[3] > 0);
  json_destroy(result);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// A simming player's turn with one legal play -- a forced pass -- runs no
// simulation, and says so. The sim results then still hold the last
// simulation's plays, which autoplay's positions recorder used to capture as
// that turn's analysis, under "sim": it now records the turn as the static
// analysis of its one play. A position with more than one play is simulated.
static void test_a_forced_turn_is_not_simulated(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 3 -plies 2 -minp 2 -iter 10 -threads 1 -sinfer false");
  load_and_exec_config_or_die(config, "cgp " OPENING_CGP);
  ErrorStack *error_stack = error_stack_create();
  // As a simulation loads it, lazily: the win percentages sim_args point at.
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack nontarget_known_tiles;
  rack_set_dist_size_and_reset(&nontarget_known_tiles, ld_size);
  Rack target_known_inference_tiles;
  rack_set_dist_size_and_reset(&target_known_inference_tiles, ld_size);
  SimArgs sim_args;
  config_fill_sim_args(config, NULL, &target_played_tiles,
                       &nontarget_known_tiles, &target_known_inference_tiles,
                       &sim_args);
  MoveList *move_list = move_list_create(3);
  sim_args.move_list = move_list;
  SimResults *sim_results = config_get_sim_results(config);
  SimCtx *sim_ctx = NULL;

  Game *game = config_get_game(config);
  bool simulated = false;
  const Move *move = get_top_simming_move(game, move_list, &sim_args, &sim_ctx,
                                          sim_results, &simulated, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move && simulated);
  assert(sim_results_get_number_of_plays(sim_results) == 3);

  // UNPLAYABLE_V_CGP with a lone V: nothing to play, and with the opponent's
  // rack full, too few tiles in the bag to exchange. Only a pass is legal.
  load_and_exec_config_or_die(config, "cgp " UNPLAYABLE_V_CGP);
  game = config_get_game(config);
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "V");
  draw_to_full_rack(game, 1);
  sim_args.game = game;
  move = get_top_simming_move(game, move_list, &sim_args, &sim_ctx, sim_results,
                              &simulated, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!simulated);
  assert(move_list_get_count(move_list) == 1);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  // The opening's simulation, which describes another position.
  assert(sim_results_get_number_of_plays(sim_results) == 3);

  // A stop request -- the user's, or a task's time limit -- that lands before
  // the simulation runs (an inferring player's stops during its inference)
  // leaves the results describing the opening, whose best play is not on
  // this rack. The turn plays its top static play instead, not simulated:
  // playing the stale one took tiles the rack did not have.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "RSTUVWY/HIJKLMN 0/0 0 -lex CSW21;");
  game = config_get_game(config);
  sim_args.game = game;
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  move = get_top_simming_move(game, move_list, &sim_args, &sim_ctx, sim_results,
                              &simulated, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(move && !simulated);
  Rack rack;
  rack_copy(&rack, player_get_rack(game_get_player(game, 0)));
  for (int i = 0; i < move_get_tiles_length(move); i++) {
    const MachineLetter tile = move_get_tile(move, i);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const MachineLetter letter =
        get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    assert(rack_get_letter(&rack, letter) > 0);
    rack_take_letter(&rack, letter);
  }
  // The best of this rack's plays, as the list ranks them.
  assert(move == move_list_get_move(move_list, 0));
  for (int i = 1; i < move_list_get_count(move_list); i++) {
    assert(move_get_equity(move_list_get_move(move_list, i)) <=
           move_get_equity(move));
  }

  error_stack_destroy(error_stack);
  sim_ctx_destroy(sim_ctx);
  move_list_destroy(move_list);
  config_destroy(config);
}

// A simming player that infers its opponent's leave before simming reports,
// on each captured position it inferred for, how many distinct leaves the
// inference found, how many it drew, their mean equity, and the most drawn
// of them, most drawn first. A position it did not infer for -- the first
// turn of a game, with no previous move to infer from -- reports none, and a
// player that does not infer reports none anywhere.
static void test_inferring_players_report_their_inference(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 3 -plies 2 -minp 2 -iter 10 -threads 2 -maxnumdplays 3 "
      "-sinfer true");
  load_and_exec_config_or_die(config, "autoplay games,positions 2 -seed 4");
  ErrorStack *error_stack = error_stack_create();
  const JsonValue *result = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), false),
      error_stack);
  assert(error_stack_is_empty(error_stack));
  const JsonValue *positions = json_object_get(result, "positions");
  int inferred = 0;
  for (int i = 0; i < json_array_length(positions); i++) {
    const JsonValue *position = json_array_get(positions, i);
    const JsonValue *inference =
        json_object_get(position, CONTRIBUTE_KEY_INFERENCE);
    if (json_get_int(position, CONTRIBUTE_KEY_TURN_NUMBER, error_stack) == 0 ||
        !json_object_get(position, CONTRIBUTE_KEY_PREVIOUS_MOVE)) {
      assert(!inference);
      continue;
    }
    if (strings_equal(json_get_string_or_null(position, "analysis"),
                      CONTRIBUTE_ANALYSIS_STATIC)) {
      // A turn with one legal play, which is not simulated: no inference,
      // and the one play as its analysis.
      assert(!inference);
      assert(json_get_int(position, CONTRIBUTE_KEY_NUM_MOVES, error_stack) ==
             1);
      continue;
    }
    assert(strings_equal(json_get_string_or_null(position, "analysis"),
                         CONTRIBUTE_ANALYSIS_SIM));
    assert(json_get_int(position, CONTRIBUTE_KEY_NUM_MOVES, error_stack) > 1);
    if (!inference) {
      // The opponent passed: nothing to infer from.
      continue;
    }
    inferred++;
    const int64_t num_leaves =
        json_get_int(inference, CONTRIBUTE_KEY_NUM_LEAVES, error_stack);
    const int64_t total_draws =
        json_get_int(inference, CONTRIBUTE_KEY_TOTAL_DRAWS, error_stack);
    const double average =
        json_get_double(inference, CONTRIBUTE_KEY_AVERAGE_EQUITY, error_stack);
    const JsonValue *leaves = json_object_get(inference, CONTRIBUTE_KEY_LEAVES);
    assert(error_stack_is_empty(error_stack));
    assert(average > -1000.0 && average < 1000.0);
    const int listed = json_array_length(leaves);
    assert(listed <= AUTOPLAY_CAPTURED_INFERENCE_LEAVES);
    assert(num_leaves >= listed);
    assert(listed > 0 || num_leaves == 0);
    int64_t previous = INT64_MAX;
    for (int l = 0; l < listed; l++) {
      const JsonValue *leave = json_array_get(leaves, l);
      const int64_t draws =
          json_get_int(leave, CONTRIBUTE_KEY_DRAWS, error_stack);
      assert(json_get_string_or_null(leave, CONTRIBUTE_KEY_LEAVE));
      (void)json_get_double(leave, CONTRIBUTE_KEY_EQUITY, error_stack);
      assert(error_stack_is_empty(error_stack));
      assert(draws > 0 && draws <= total_draws && draws <= previous);
      previous = draws;
    }
  }
  assert(inferred > 0);
  json_destroy(result);
  // And birdtest's fixture for such a result holds nothing this output lacks.
  assert_result_produces_fixture_keys(
      BIRDTEST_RESULT_GAMES_INFERENCE_FIXTURE,
      autoplay_results_get_json(config_get_autoplay_results(config), false));

  // Without inference, no position reports one.
  load_and_exec_config_or_die(config, "set -sinfer false");
  load_and_exec_config_or_die(config, "autoplay games,positions 1 -seed 4");
  result = json_parse(
      autoplay_results_get_json(config_get_autoplay_results(config), false),
      error_stack);
  assert(error_stack_is_empty(error_stack));
  positions = json_object_get(result, "positions");
  assert(json_array_length(positions) > 0);
  for (int i = 0; i < json_array_length(positions); i++) {
    assert(!json_object_get(json_array_get(positions, i),
                            CONTRIBUTE_KEY_INFERENCE));
  }
  json_destroy(result);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_contribute(void) {
  test_static_games_are_identical_across_thread_counts();
  test_a_player_states_its_solving();
  test_no_endgame_depth_means_no_pre_endgame();
  test_solver_options_round_trip();
  test_solving_players_report_their_solves();
  test_a_high_byte_is_not_a_bonus_square();
  test_a_simulating_rack_analysis_ranks_every_play();
  test_contribute_runs_in_a_config_of_its_own();
  test_http_retries_outlast_a_server_deployment();
  test_a_request_must_state_its_distribution_and_layout();
  test_lexical_flags_are_set_before_the_load();
  test_shared_settings_do_not_leak_between_tasks();
  test_a_player_must_state_every_setting();
  test_opening_rack_analysis_uses_the_players_settings();
  test_version_comparison();
  test_json_wrapper();
  test_json_serialization();
  test_client_state();
  test_the_default_settings_file_may_be_missing();
  test_sha256();
  test_digest_cache_key_notices_a_same_size_replacement();
  test_contract_fixtures_carry_every_key_contribute_reads();
  test_results_carry_every_key_the_server_reads();
  test_capturing_positions_does_not_change_the_games();
  test_inferring_players_report_their_inference();
  test_a_forced_turn_is_not_simulated();
  test_a_pairs_first_divergence_is_both_games_at_one_turn();
  test_player_settings_do_not_leak_between_tasks();
  test_a_rewritten_klv_is_read_again();
  test_an_abandoned_temporary_is_removed();
  test_a_runs_state_starts_clean();
  test_a_run_says_which_settings_are_defaults();
  test_contribute_maps_rack_info_tables_unless_told_not_to();
  test_a_derived_build_is_held_by_one_process();
  test_a_runs_threads_are_capped();
  test_a_set_aside_job_is_left_out_of_claims_for_a_while();
  test_only_a_data_shutdown_is_waited_out_for_a_set_aside_job();
  test_the_claim_body_matches_the_claim_fixture();
  test_the_decline_body_matches_the_decline_fixture();
  test_the_result_body_carries_movegens();
  test_a_task_counts_its_move_generations();
  test_an_unverifiable_assignment_is_refused();
}

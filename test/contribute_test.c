#include "contribute_test.h"

#include "../src/def/contribute_defs.h"
#include "../src/ent/client_state.h"
#include "../src/impl/config.h"
#include "../src/impl/contribute.h"
#include "../src/util/hash.h"
#include "../src/util/io_util.h"
#include "../src/util/json.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

  // So is a missing server.
  write_settings_file(path, "apikey bt_x\n");
  assert(!client_state_load(path, error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // As is a file that is not there at all.
  assert(!client_state_load("contribute_test_does_not_exist.txt", error_stack));
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  (void)remove(path);
  error_stack_destroy(error_stack);
}

// The known vectors everyone quotes. A hash that is merely self-consistent
// would verify nothing: the server computes these digests independently, so
// this implementation has to agree with the rest of the world.
static void test_sha256(void) {
  char *empty = sha256_hash_bytes("", 0);
  assert_strings_equal(
      empty, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
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
  FILE *file = fopen(path, "wb");
  assert(file);
  fwrite("abc", 1, 3, file);
  fclose(file);

  ErrorStack *error_stack = error_stack_create();
  char *from_file = sha256_hash_file(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(
      from_file, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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
  FILE *file = fopen(path, "wb");
  assert(file);
  fwrite("aaaa", 1, 4, file);
  fclose(file);

  struct stat before;
  assert(stat(path, &before) == 0);
  char *first = contribute_digest_cache_key(path);
  assert(first);

  // Replace the contents with different bytes of the same length, then force
  // the modification time back to what it was.
  file = fopen(path, "wb");
  assert(file);
  fwrite("bbbb", 1, 4, file);
  fclose(file);
  struct utimbuf times = {.actime = before.st_atime, .modtime = before.st_mtime};
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
static void test_contract_fixtures_carry_every_key_contribute_reads(void) {
  const JsonValue *request = NULL;
  JsonValue *games =
      load_task_request_fixture(BIRDTEST_GAMES_FIXTURE, &request);
  const char *const request_keys[] = {
      CONTRIBUTE_KEY_VARIANT,      CONTRIBUTE_KEY_LETTER_DISTRIBUTION,
      CONTRIBUTE_KEY_BOARD_LAYOUT, CONTRIBUTE_KEY_SEED,
      CONTRIBUTE_KEY_NUM_GAMES,    CONTRIBUTE_KEY_CAPTURE_POSITIONS,
      CONTRIBUTE_KEY_PLAYER1,      CONTRIBUTE_KEY_PLAYER2,
  };
  assert_fixture_has_keys(request, request_keys,
                          sizeof(request_keys) / sizeof(request_keys[0]),
                          "games task_request");
  const char *const player_keys[] = {
      CONTRIBUTE_KEY_PLAYER_LEXICON,       CONTRIBUTE_KEY_LEAVES,
      CONTRIBUTE_KEY_RECORDER_TYPE,        CONTRIBUTE_KEY_SORT_STRATEGY,
      CONTRIBUTE_KEY_MAX_ITERATIONS,       CONTRIBUTE_KEY_NUM_PLIES,
      CONTRIBUTE_KEY_NUM_PLIES_RECORDED,   CONTRIBUTE_KEY_NUM_PLAYS,
      CONTRIBUTE_KEY_NUM_PLAYS_RECORDED,   CONTRIBUTE_KEY_STOPPING_PCT,
      CONTRIBUTE_KEY_USE_INFERENCE,        CONTRIBUTE_KEY_TIME_LIMIT_SECS,
      CONTRIBUTE_KEY_USE_WORDMAP,          CONTRIBUTE_KEY_USE_RIT,
      CONTRIBUTE_KEY_MIN_PLAY_ITERATIONS,  CONTRIBUTE_KEY_THRESHOLD,
      CONTRIBUTE_KEY_SAMPLING_RULE,        CONTRIBUTE_KEY_INFERENCE_MARGIN,
      CONTRIBUTE_KEY_UTILITY_W_WINPCT,     CONTRIBUTE_KEY_UTILITY_W_SPREAD,
      CONTRIBUTE_KEY_UTILITY_SPREAD_SCALE, CONTRIBUTE_KEY_WIN_PCT_MODEL,
      CONTRIBUTE_KEY_MOVEGEN_MARGIN,
  };
  const int num_player_keys = sizeof(player_keys) / sizeof(player_keys[0]);
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER1),
                          player_keys, num_player_keys, "games player1");
  assert_fixture_has_keys(json_object_get(request, CONTRIBUTE_KEY_PLAYER2),
                          player_keys, num_player_keys, "games player2");
  json_destroy(games);

  JsonValue *leave =
      load_task_request_fixture(BIRDTEST_LEAVE_FIXTURE, &request);
  const char *const leave_keys[] = {
      CONTRIBUTE_KEY_LEXICON,
      CONTRIBUTE_KEY_VARIANT,
      CONTRIBUTE_KEY_LETTER_DISTRIBUTION,
      CONTRIBUTE_KEY_BOARD_LAYOUT,
      CONTRIBUTE_KEY_FORCED_RACKS,
      CONTRIBUTE_KEY_NUM_GAMES,
      CONTRIBUTE_KEY_PREVIOUS_ARTIFACT_KEY,
      CONTRIBUTE_KEY_USE_WORDMAP,
  };
  assert_fixture_has_keys(request, leave_keys,
                          sizeof(leave_keys) / sizeof(leave_keys[0]),
                          "leave_generation task_request");
  // The generation's rack target is server-only state; a client that
  // required it would fail every leave_generation task.
  assert(!json_object_get(request, "target_rack_count"));
  json_destroy(leave);
}

// A null in a request means MAGPIE's default, not whatever the process last
// had. The case this pins: a static player applied after a simming one -- or
// after the contributor's own settings asked for plies -- must not simulate.
static void test_player_settings_do_not_leak_between_tasks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -plies 5");
  const JsonValue *request = NULL;
  JsonValue *games =
      load_task_request_fixture(BIRDTEST_GAMES_FIXTURE, &request);
  const JsonValue *simmer = json_object_get(request, CONTRIBUTE_KEY_PLAYER1);
  const JsonValue *static_player =
      json_object_get(request, CONTRIBUTE_KEY_PLAYER2);
  ErrorStack *error_stack = error_stack_create();

  assert(config_get_player_sim_plies(config, 1) == 5);
  config_contribute_apply_player_settings(config, simmer, 0, error_stack);
  config_contribute_apply_player_settings(config, static_player, 1,
                                          error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(config_get_player_sim_plies(config, 0) == 4);
  assert(config_get_player_num_plays(config, 0) == 10);
  assert(config_get_player_max_iterations(config, 0) == 1000);
  // The contributor's -plies 5 does not survive a request that says static.
  assert(config_get_player_sim_plies(config, 1) == 0);

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

void test_contribute(void) {
  test_version_comparison();
  test_json_wrapper();
  test_json_serialization();
  test_client_state();
  test_sha256();
  test_digest_cache_key_notices_a_same_size_replacement();
  test_contract_fixtures_carry_every_key_contribute_reads();
  test_player_settings_do_not_leak_between_tasks();
}

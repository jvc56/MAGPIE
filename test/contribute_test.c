#include "contribute_test.h"

#include "../src/def/config_defs.h"
#include "../src/def/contribute_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/autoplay_results.h"
#include "../src/ent/client_state.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/config.h"
#include "../src/impl/contribute.h"
#include "../src/impl/rack_list.h"
#include "../src/util/hash.h"
#include "../src/util/http_client.h"
#include "../src/util/io_util.h"
#include "../src/util/json.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
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
// config_contribute_ensure_wordmap and
// config_contribute_ensure_rack_info_table read off it. A wordmap or a rack
// info table is built on this machine and never shipped, so this hash is the
// only thing that says the bytes are the ones the job means -- a fixture
// missing a field here is a worker running unverified, which is the state this
// whole mechanism replaces.
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
    assert(strings_equal(role, "wmp") || strings_equal(role, "rit"));
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
      CONTRIBUTE_KEY_USE_WORDMAP,
      CONTRIBUTE_KEY_BINGO_BONUS,
  };
  assert_fixture_has_keys(request, leave_keys,
                          sizeof(leave_keys) / sizeof(leave_keys[0]),
                          "leave_generation task_request");
  // The generation's rack target is server-only state; a client that
  // required it would fail every leave_generation task.
  assert(!json_object_get(request, "target_rack_count"));
  json_destroy(leave);

  // A game_pairs task is run by the games executor, told apart only by its
  // job_type, so it needs every key a games request does.
  const JsonValue *pairs =
      load_task_request_fixture(BIRDTEST_GAME_PAIRS_FIXTURE, &request);
  assert_fixture_is_an_assignment(pairs, "game_pairs assignment");
  assert_strings_equal(json_get_string_or_null(request, "job_type"),
                       "game_pairs");
  assert(json_get_bool_or(request, "game_pairs", false));
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
  assert(json_get_string_or_null(anon, "worker_uuid"));
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
      const JsonValue *mine = NULL;
      for (int j = 0; j < json_array_length(produced) && !mine; j++) {
        mine = json_object_get(json_array_get(produced, j), key);
      }
      if (!mine) {
        log_fatal("%s[]: birdtest's fixture has '%s', which no element of "
                  "MAGPIE's result does",
                  where, key);
      }
      char *path = get_formatted_string("%s[].%s", where, key);
      assert_produces_every_fixture_key(json_object_get(element, key), mine,
                                        path);
      free(path);
    }
  }
}

// The result a fixture holds, under the envelope submit_result_over_http
// writes around it.
static const JsonValue *fixture_result(const JsonValue *fixture) {
  assert(json_object_size(fixture) == 2);
  assert(json_get_string_or_null(fixture, "claim_token"));
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
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 score -r1 best -r2 best "
      "-threads 1 -maxnumdplays 5");

  // games, with capture on: the game recorder and the positions recorder,
  // together, as config_contribute_games asks for them.
  load_and_exec_config_or_die(config, "autoplay games,positions 2 -seed 3");
  assert_result_produces_fixture_keys(
      BIRDTEST_RESULT_GAMES_FIXTURE,
      autoplay_results_get_json(config_get_autoplay_results(config), false));

  // game_pairs: all_games, the pentanomial and the divergent subset.
  load_and_exec_config_or_die(config, "autoplay games 2 -seed 3 -gp true");
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
  Config *config = config_create_or_die("set -lex CSW21 -plies 5");
  const JsonValue *request = NULL;
  const JsonValue *games =
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

// Run-wide settings no request states are reset rather than inherited. The
// case this pins: a contributor whose settings.txt changes the bingo bonus
// would otherwise score every game of every job differently from the fleet.
static void test_shared_settings_do_not_leak_between_tasks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -bb 35 -sp true");
  assert(config_get_bingo_bonus(config) == 35);
  assert(config_get_use_small_plays(config));

  config_contribute_reset_shared_settings(config);
  assert(config_get_bingo_bonus(config) == DEFAULT_BINGO_BONUS);
  assert(!config_get_use_small_plays(config));

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
      "{\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, static_player, 0,
                                          error_stack);
  assert(error_stack_is_empty(error_stack));

  // One that leaves its play count to this build is refused.
  const JsonValue *no_plays = json_parse(
      "{\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 0, \"num_plays\": null, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, no_plays, 0, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

  // So is a simulating player that states only what a static one must.
  const JsonValue *bare_simmer = json_parse(
      "{\"recorder_type\": \"best\", \"sort_strategy\": \"equity\", "
      "\"num_plies\": 2, \"num_plays\": 100, \"num_plies_recorded\": 2, "
      "\"num_plays_recorded\": 10, \"movegen_margin\": 5.0}",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_contribute_apply_player_settings(config, bare_simmer, 0, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_reset(error_stack);

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
  // itself refuses to set the flag without a file to load): birdtest offers
  // neither setting, so contribute must switch both off before the load
  // decides what to open.
  for (int player_index = 0; player_index < 2; player_index++) {
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_RIT,
                                        player_index, true);
    players_data_set_use_when_available(players_data, PLAYERS_DATA_TYPE_WIT,
                                        player_index, true);
  }
  ErrorStack *error_stack = error_stack_create();

  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", NULL, NULL, "CSW21", "CSW21", "CSW21",
      "CSW21", false, false, NULL, NULL, error_stack);
  assert(error_stack_is_empty(error_stack));
  for (int player_index = 0; player_index < 2; player_index++) {
    assert(!players_data_get_wmp(players_data, player_index));
    assert(!players_data_get_rack_info_table(players_data, player_index));
    assert(!players_data_get_use_when_available(
        players_data, PLAYERS_DATA_TYPE_WIT, player_index));
  }

  // A task that asks for a wordmap gets it for itself, not for the next task.
  config_contribute_load_lexicon_and_variant(
      config, "CSW21", "classic", NULL, NULL, "CSW21", "CSW21", "CSW21",
      "CSW21", true, false, NULL, NULL, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(players_data_get_wmp(players_data, 0));
  assert(!players_data_get_wmp(players_data, 1));

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
}

void test_contribute(void) {
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
  test_sha256();
  test_digest_cache_key_notices_a_same_size_replacement();
  test_contract_fixtures_carry_every_key_contribute_reads();
  test_results_carry_every_key_the_server_reads();
  test_capturing_positions_does_not_change_the_games();
  test_player_settings_do_not_leak_between_tasks();
}

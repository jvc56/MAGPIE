#include "contribute.h"

#include "../compat/cfile.h"
#include "../compat/chttp.h"
#include "../compat/cpath.h"
#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/board_defs.h"
#include "../def/contribute_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/client_state.h"
#include "../ent/data_filepaths.h"
#include "../ent/thread_control.h"
#include "../util/hash.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include "contribute_tables.h"
#include "data_fetch.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  HEARTBEAT_INTERVAL_SECONDS = 30,
  // A task that fails does not count toward maxtasks, so without this a job
  // this build cannot execute would spin forever, claiming and abandoning.
  MAX_CONSECUTIVE_FAILURES = 5,
  MAX_PRINTED_JOB_NAME = 200,
};

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------

// Compares dotted numeric versions. Returns <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" compare equal.
int contribute_compare_versions(const char *left, const char *right) {
  StringSplitter *left_parts = split_string(left, '.', true);
  StringSplitter *right_parts = split_string(right, '.', true);
  const int left_count = string_splitter_get_number_of_items(left_parts);
  const int right_count = string_splitter_get_number_of_items(right_parts);
  const int num_parts = left_count > right_count ? left_count : right_count;

  ErrorStack *conversion_errors = error_stack_create();
  int result = 0;
  for (int part_idx = 0; part_idx < num_parts; part_idx++) {
    // Missing components count as zero, so "1.4" and "1.4.0" compare equal.
    const int left_part =
        part_idx < left_count
            ? string_to_int(string_splitter_get_item(left_parts, part_idx),
                            conversion_errors)
            : 0;
    const int right_part =
        part_idx < right_count
            ? string_to_int(string_splitter_get_item(right_parts, part_idx),
                            conversion_errors)
            : 0;
    error_stack_reset(conversion_errors);
    if (left_part != right_part) {
      result = left_part < right_part ? -1 : 1;
      break;
    }
  }

  error_stack_destroy(conversion_errors);
  string_splitter_destroy(left_parts);
  string_splitter_destroy(right_parts);
  return result;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

typedef struct Heartbeat {
  HttpClient *client;
  char *claim_token;
  bool stop;
  cpthread_mutex_t mutex;
  cpthread_t thread;
  bool running;
} Heartbeat;

static void *heartbeat_worker(void *arg) {
  Heartbeat *heartbeat = (Heartbeat *)arg;
  StringBuilder *sb = string_builder_create();
  ErrorStack *errors = error_stack_create();
  while (true) {
    // Poll the stop flag on a short interval so stopping is responsive even
    // though the heartbeat itself is infrequent.
    bool stop = false;
    for (int i = 0; i < HEARTBEAT_INTERVAL_SECONDS; i++) {
      ctime_nap(1.0);
      cpthread_mutex_lock(&heartbeat->mutex);
      stop = heartbeat->stop;
      cpthread_mutex_unlock(&heartbeat->mutex);
      if (stop) {
        break;
      }
    }
    if (stop) {
      break;
    }

    bool first = true;
    json_write_object_start(sb);
    json_write_string_field(sb, "claim_token", heartbeat->claim_token, &first);
    json_write_object_end(sb);
    char *body = string_builder_dump(sb, NULL);
    string_builder_clear(sb);

    // A failed heartbeat is not actionable here: the server treats a missed
    // one as a lapsed claim and reassigns the task, which is the design. One
    // attempt, not the client's retry schedule: the next heartbeat is thirty
    // seconds away whatever happened to this one, and a heartbeat backing off
    // through a server restart would keep heartbeat_stop -- and so the task's
    // submission -- waiting for as long as it retried.
    ChttpResponse response;
    http_client_post_json_once(heartbeat->client, "/api/worker/heartbeat", body,
                               &response, errors);
    if (error_stack_is_empty(errors)) {
      chttp_response_destroy(&response);
    }
    error_stack_reset(errors);
    free(body);
  }

  string_builder_destroy(sb);
  error_stack_destroy(errors);
  return NULL;
}

static void heartbeat_start(Heartbeat *heartbeat, HttpClient *client,
                            const char *claim_token) {
  heartbeat->client = client;
  heartbeat->claim_token = string_duplicate(claim_token);
  heartbeat->stop = false;
  heartbeat->running = true;
  cpthread_mutex_init(&heartbeat->mutex);
  cpthread_create(&heartbeat->thread, heartbeat_worker, heartbeat);
}

static void heartbeat_stop(Heartbeat *heartbeat) {
  if (!heartbeat->running) {
    return;
  }
  cpthread_mutex_lock(&heartbeat->mutex);
  heartbeat->stop = true;
  cpthread_mutex_unlock(&heartbeat->mutex);
  cpthread_join(heartbeat->thread);
  free(heartbeat->claim_token);
  heartbeat->claim_token = NULL;
  heartbeat->running = false;
}

// ---------------------------------------------------------------------------
// Task deadline
// ---------------------------------------------------------------------------

// Nanoseconds on the monotonic clock.
static int64_t now_ns(void) { return ctimer_monotonic_ns(); }

// How often the deadline thread looks at the clock. A tenth of a second is
// nothing next to a task's minutes, and keeps a stop prompt.
#define DEADLINE_POLL_SECONDS 0.1

struct ContributeDeadline {
  ThreadControl *thread_control;
  int64_t deadline_ns;
  cpthread_mutex_t mutex;
  bool stop;
  // Written by the deadline thread before it exits, read after the join.
  bool fired;
  thread_control_status_t status_before;
  cpthread_t thread;
};

// Runs beside the task like the heartbeat, polling a stop flag. At the
// deadline it stops the task the way the user's `stop` does -- every
// executor already ends early on that, and nothing else does -- unless the
// user has stopped it already, in which case the stop is the user's.
static void *deadline_worker(void *arg) {
  ContributeDeadline *deadline = (ContributeDeadline *)arg;
  while (true) {
    ctime_nap(DEADLINE_POLL_SECONDS);
    cpthread_mutex_lock(&deadline->mutex);
    const bool stop = deadline->stop;
    cpthread_mutex_unlock(&deadline->mutex);
    if (stop) {
      break;
    }
    if (now_ns() < deadline->deadline_ns) {
      continue;
    }
    const thread_control_status_t status =
        thread_control_get_status(deadline->thread_control);
    // set_status says whether it changed anything: a stop that landed
    // between the read and here leaves it unchanged, and stays the user's.
    if (status != THREAD_CONTROL_STATUS_USER_INTERRUPT &&
        thread_control_set_status(deadline->thread_control,
                                  THREAD_CONTROL_STATUS_USER_INTERRUPT)) {
      deadline->status_before = status;
      deadline->fired = true;
    }
    break;
  }
  return NULL;
}

ContributeDeadline *contribute_deadline_start(ThreadControl *thread_control,
                                              double seconds) {
  if (!thread_control || seconds <= 0) {
    return NULL;
  }
  ContributeDeadline *deadline =
      (ContributeDeadline *)calloc_or_die(1, sizeof(ContributeDeadline));
  deadline->thread_control = thread_control;
  deadline->deadline_ns = now_ns() + (int64_t)(seconds * 1e9);
  cpthread_mutex_init(&deadline->mutex);
  cpthread_create(&deadline->thread, deadline_worker, deadline);
  return deadline;
}

bool contribute_deadline_finish(ContributeDeadline *deadline) {
  if (!deadline) {
    return false;
  }
  cpthread_mutex_lock(&deadline->mutex);
  deadline->stop = true;
  cpthread_mutex_unlock(&deadline->mutex);
  cpthread_join(deadline->thread);
  const bool fired = deadline->fired;
  if (fired) {
    // The stop was the deadline's, not the user's: the run goes on, unless
    // the user asked to stop while it was in place.
    (void)thread_control_restore_unless_stopped(deadline->thread_control,
                                                deadline->status_before);
  }
  free(deadline);
  return fired;
}

// ---------------------------------------------------------------------------
// The claim/submit state machine
// ---------------------------------------------------------------------------

// See ContributeState.deferred.
typedef struct DeferredJob {
  char *job_id;
  int64_t until_ns;
  int wait_seconds;
} DeferredJob;

struct ContributeState {
  ClientState *client_state;
  HttpClient *http_client;
  int threads;
  // Tasks this run has started (claimed and handed to the executor) and
  // completed (submitted and accepted). The first numbers the
  // started/finished lines.
  int started;
  int completed;
  int consecutive_failures;
  char *last_failure;
  bool stop;

  // Jobs this worker has found it cannot run, for any reason -- missing data,
  // a MAGPIE too old, a job type this build does not know. Sent with every
  // claim so the server routes around them.
  //
  // In memory only, and deliberately so: a contributor who stops and restarts
  // has, in the case that matters, just updated their data, which is what the
  // shutdown message told them to do. A client that remembered its limitations
  // across restarts would refuse work it can now do, and the only cure would
  // be a config file the user has to know to edit. Forgetting costs one wasted
  // claim per job on the next run.
  StringList *unsupported_jobs;
  // Every gap found this run, in full detail, for the summary printed when the
  // server says to stop.
  StringList *data_gaps;
  // Gaps already logged, keyed by resolved path and expected digest. A worker
  // missing a common lexicon declines steadily -- that is the designed
  // behaviour -- and a line per decline would turn an ordinary condition into
  // a firehose.
  StringList *logged_gaps;
  // Digests already computed this run, as "key\tdigest" entries. See
  // digest_cache_key: the key includes inode and ctime, not just size and
  // mtime, because a file replaced with same-size bytes inside one mtime tick
  // is exactly the case a cache must not miss.
  StringList *digest_cache;
  // Derived files (wordmaps, rack info tables) whose bytes did not match what
  // the claim pins, as JSON objects ready to go into a decline's "missing"
  // array. Cleared by every decline that sends them.
  StringList *derived_mismatches;
  // Whether one of them is the server's own artifact (the leave KLV) rather
  // than a file built here; see contribute_decline_derived_mismatch.
  bool server_artifact_mismatch;
  // The derived tables the claimed task's players asked for and this worker
  // skipped for lack of space, as a set of 1 << contribute_table_role_t:
  // sent with its result as `derived_skipped`. Cleared with each claim.
  unsigned int derived_skipped;
  // Jobs set aside for a while because the server's leave KLV for them was
  // missing or wrong: sent as unsupported until `until_ns`, then claimable
  // again -- and the KLV fetched afresh, so an admin's repair is noticed.
  // The interval doubles per job to ten minutes and is forgotten when the
  // job's KLV verifies. Only that job waits: the worker goes on with others.
  DeferredJob *deferred;
  int deferred_count;
  int deferred_capacity;
  // Whether the last claim's body named any set-aside job. Decided when the
  // body is built, not afterwards by the clock: a deferral that ended while
  // the claim was retrying (a 429, a deployment's 503s) was still in the body
  // the server answered.
  bool claim_named_deferred;
  // Set once the server has told this worker to stop.
  bool shutdown_requested;
  // Set once any claim has been answered by the server, whatever it said.
  // From then on a claim that cannot reach the server keeps trying instead of
  // ending the run: see claim_task_over_http.
  bool reached_server;

  // Set between a successful claim and its matching submit.
  Heartbeat heartbeat;
  char *claim_token;
  char *claimed_job_id;
  JsonValue *assignment;
  // The claimed task's job name, as printed, its number this run, the
  // seconds it may run, and the clock that stops it then.
  char *job_name;
  int task_number;
  int max_task_seconds;
  ContributeDeadline *deadline;
  // When the task's time started, on the monotonic clock: what its finished,
  // stopped or failed line says it took.
  int64_t task_started_ns;
  // The data paths the claimed task runs with when they are not the
  // contributor's own: a directory of pinned input files fetched from the
  // server, then the contributor's. NULL when the contributor's own data is
  // what the job pins. See contribute_verify_expected_data.
  char *task_data_paths;
  // How a pinned input file is fetched: over HTTP from the server, unless a
  // test has put something in its place.
  contribute_input_fetch_t input_fetch;
  void *input_fetch_context;
};

// ---------------------------------------------------------------------------
// Input data verification
// ---------------------------------------------------------------------------

// The role names birdtest uses, mapped to the file types MAGPIE resolves.
// Resolving through data_filepaths rather than guessing a path is the whole
// point: it checks the file that will actually load, across the whole
// data_paths search list. A contributor with both a download_data.sh install
// and a MAGPIE-DATA clone has two english.csv files, and only the resolver
// knows which one wins.
static bool role_to_filepath_type(const char *role, data_filepath_t *out) {
  if (strings_equal(role, "kwg")) {
    *out = DATA_FILEPATH_TYPE_KWG;
  } else if (strings_equal(role, "klv")) {
    *out = DATA_FILEPATH_TYPE_KLV;
  } else if (strings_equal(role, "winpct")) {
    *out = DATA_FILEPATH_TYPE_WIN_PCT;
  } else if (strings_equal(role, "letterdist")) {
    *out = DATA_FILEPATH_TYPE_LD;
  } else if (strings_equal(role, "layout")) {
    *out = DATA_FILEPATH_TYPE_LAYOUT;
  } else {
    return false;
  }
  return true;
}

// The path is part of the key: the cache holds digests for many files.
char *contribute_digest_cache_key(const char *path) {
  char *identity = get_file_identity(path);
  if (!identity) {
    return NULL;
  }
  char *key = get_formatted_string("%s|%s", path, identity);
  free(identity);
  return key;
}

static const char *cache_lookup(const StringList *cache, const char *key) {
  const int count = string_list_get_count(cache);
  for (int i = 0; i < count; i++) {
    const char *entry = string_list_get_string(cache, i);
    const char *separator = strchr(entry, '\t');
    if (!separator) {
      continue;
    }
    const size_t key_length = (size_t)(separator - entry);
    if (key_length == string_length(key) &&
        strncmp(entry, key, key_length) == 0) {
      return separator + 1;
    }
  }
  return NULL;
}

// The digest of `path`, from the cache when the file is unchanged. Hashing a
// 15 MB lexicon on every claim would be the alternative; the cache is an
// optimisation only, and a miss is always a real hash.
static char *hash_with_cache(ContributeState *state, const char *path,
                             ErrorStack *error_stack) {
  char *key = contribute_digest_cache_key(path);
  if (key) {
    const char *cached = cache_lookup(state->digest_cache, key);
    if (cached) {
      char *digest = string_duplicate(cached);
      free(key);
      return digest;
    }
  }

  char *digest = sha256_hash_file(path, error_stack);
  if (digest && key) {
    char *entry = get_formatted_string("%s\t%s", key, digest);
    string_list_add_string(state->digest_cache, entry);
    free(entry);
  }
  free(key);
  return digest;
}

static bool already_logged(ContributeState *state, const char *key) {
  const int count = string_list_get_count(state->logged_gaps);
  for (int i = 0; i < count; i++) {
    if (strings_equal(string_list_get_string(state->logged_gaps, i), key)) {
      return true;
    }
  }
  string_list_add_string(state->logged_gaps, key);
  return false;
}

// Every assignment states the files its task loads under `expected_data`,
// with the algorithm their digests use. birdtest always sends it, naming
// sha256; an assignment without it, or naming an algorithm this build does
// not know, is one whose input data this build cannot check, and is refused
// rather than run unverified -- results computed from different bytes are
// worse than none, because nothing downstream would notice.
//
// So is an entry of its `files` this build cannot check: one without a role,
// name or digest, or with a role this build does not know. Skipped, it named
// a file the task then loaded unchecked.
void contribute_check_expected_data(const JsonValue *assignment,
                                    ErrorStack *error_stack) {
  const JsonValue *expected = json_object_get(assignment, "expected_data");
  const char *algorithm =
      expected ? json_get_string_or_null(expected, "algorithm") : NULL;
  if (!algorithm || !strings_equal(algorithm, "sha256")) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string(
            "the server sent a task whose input data this build cannot check "
            "(expected_data algorithm: %s; this build knows sha256)",
            algorithm ? algorithm : "none"));
    return;
  }
  const JsonValue *files = json_object_get(expected, "files");
  if (!json_is_array(files)) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     string_duplicate("the server sent a task whose "
                                      "expected_data lists no files"));
    return;
  }
  for (int i = 0; i < json_array_length(files); i++) {
    const JsonValue *file = json_array_get(files, i);
    const char *role = json_get_string_or_null(file, "role");
    data_filepath_t type;
    if (!role || !json_get_string_or_null(file, "name") ||
        !json_get_string_or_null(file, "sha256") ||
        !role_to_filepath_type(role, &type)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
          get_formatted_string(
              "the server sent a task whose input data this build cannot "
              "check (expected_data file %d, role %s)",
              i, role ? role : "none"));
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Pinned input files this contributor does not have
// ---------------------------------------------------------------------------
//
// A job pins bytes. A contributor usually has them from a MAGPIE-DATA
// tarball, and for a long time that was the only place: a file that did not
// match was a decline, with the advice to install the tarball it came from.
// Then a tarball was rebuilt under the name it already had (data-20260925,
// 2026-11-02, with new leaves for every lexicon), the jobs went on pinning
// the files it replaced, and that advice reinstalled the very file the worker
// had just been declined for. So the assignment now says where each file can
// be fetched (`sources`, and its size, `bytes`), and a file that does not
// match is fetched, checked against the digest the job pins, and kept.
//
// Kept apart from the contributor's own data: a fetched CSW24.klv2 written
// over data/lexica/CSW24.klv2 would change what every other MAGPIE command of
// theirs plays with. It goes into a directory of its own inside the data
// directory, laid out as a data directory is, and the task runs with that
// directory ahead of the contributor's in its data paths -- so the file keeps
// its name, the tables built from it are written beside it rather than over
// the contributor's, and everything the job pins that the contributor does
// have is still read from where it is.

#define CONTRIBUTE_INPUT_DIRECTORY "birdtest"

// A source this build can fetch: a path on the server the claim came from.
// The string is the server's and becomes part of a URL, so anything outside
// a plain path and query is refused; a source of any other kind (another
// host, which a later server may list) is one this build skips.
static bool input_source_is_usable(const char *source) {
  if (!source || source[0] != '/' || source[1] == '/' || strstr(source, "..")) {
    return false;
  }
  for (const char *c = source; *c; c++) {
    const bool allowed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                         (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' ||
                         *c == '.' || *c == '/' || *c == '?' || *c == '=' ||
                         *c == '&';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

static bool input_file_is_fetchable(const JsonValue *file) {
  if (!data_filepaths_is_safe_name(json_get_string_or_null(file, "name"))) {
    return false;
  }
  const JsonValue *sources = json_object_get(file, "sources");
  for (int i = 0; i < json_array_length(sources); i++) {
    if (input_source_is_usable(json_array_get_string(sources, i))) {
      return true;
    }
  }
  return false;
}

static char *fetch_input_over_http(void *context, const char *source,
                                   size_t *length, char **why) {
  ContributeState *state = (ContributeState *)context;
  ErrorStack *errors = error_stack_create();
  ChttpResponse response;
  http_client_get(state->http_client, source, &response, errors);
  char *body = NULL;
  if (!error_stack_is_empty(errors)) {
    *why = error_stack_get_string_and_reset(errors);
  } else if (response.status_code != 200) {
    *why = get_formatted_string("the server answered HTTP %ld",
                                response.status_code);
    chttp_response_destroy(&response);
  } else {
    // The response's own buffer, taken rather than copied.
    body = response.body;
    *length = response.body_length;
    response.body = NULL;
    chttp_response_destroy(&response);
  }
  error_stack_destroy(errors);
  return body;
}

void contribute_set_input_fetch(ContributeState *state,
                                contribute_input_fetch_t fetch, void *context) {
  state->input_fetch = fetch;
  state->input_fetch_context = context;
}

const char *contribute_get_task_data_paths(const ContributeState *state) {
  return state ? state->task_data_paths : NULL;
}

char *contribute_input_directory(const char *data_paths,
                                 const char *fetched_files) {
  char *digest = sha256_hash_bytes(fetched_files, strlen(fetched_files));
  char *first = cpath_list_first(data_paths ? data_paths : ".");
  char *directory =
      get_formatted_string("%s/" CONTRIBUTE_INPUT_DIRECTORY "/%.16s",
                           first[0] ? first : ".", digest);
  free(first);
  free(digest);
  return directory;
}

// The directories a file at `path` needs, made. False with errno set if they
// are not there afterwards.
static bool make_directories_for(const char *path) {
  size_t length = strlen(path);
  while (length > 0 && !cpath_is_separator(path[length - 1])) {
    length--;
  }
  char *directory = get_formatted_string("%.*s", (int)length, path);
  const bool made = data_fetch_make_directories(directory);
  free(directory);
  return made;
}

// Writes `body` to `path` in one step, so a task -- this process's or another
// sharing the data directory -- reads the whole file or none of it.
static bool write_input_file(const char *path, const char *body,
                             size_t length) {
  if (!make_directories_for(path)) {
    return false;
  }
  CFileAtomicWrite atomic_write;
  FILE *stream = cfile_atomic_replace_begin(&atomic_write, path);
  if (!stream) {
    return false;
  }
  const bool written = fwrite(body, 1, length, stream) == length;
  return cfile_atomic_write_end(&atomic_write, written);
}

// Makes the file the assignment's `file` entry pins be at `path`, inside the
// directory of fetched input files: there already with the pinned digest, or
// fetched from the first of its sources that sends those bytes. Returns
// whether it is there; if not, *why says what went wrong (the caller frees).
static bool input_file_fetched(ContributeState *state, const JsonValue *file,
                               const char *path, ThreadControl *thread_control,
                               char **why) {
  const char *expected_digest = json_get_string_or_null(file, "sha256");
  ErrorStack *errors = error_stack_create();
  char *kept = hash_with_cache(state, path, errors);
  error_stack_destroy(errors);
  const bool already_there = kept && strings_equal(kept, expected_digest);
  free(kept);
  if (already_there) {
    return true;
  }

  // The claim's time is running from here, as it is for a leave task's KLV
  // fetched mid-task: a lexicon over a slow link, or a wait for the server's
  // rate limit, must not let the claim lapse before the task has begun.
  if (state->claim_token && !state->heartbeat.running) {
    heartbeat_start(&state->heartbeat, state->http_client, state->claim_token);
  }

  const int64_t expected_bytes = json_get_int64_or(file, "bytes", -1);
  const JsonValue *sources = json_object_get(file, "sources");
  bool fetched = false;
  for (int i = 0; i < json_array_length(sources) && !fetched; i++) {
    const char *source = json_array_get_string(sources, i);
    if (!input_source_is_usable(source)) {
      continue;
    }
    free(*why);
    *why = NULL;
    size_t length = 0;
    char *body =
        state->input_fetch(state->input_fetch_context, source, &length, why);
    if (!body) {
      continue;
    }
    // What is kept is what the job pins, whatever sent it: the size first,
    // which costs nothing, then the digest.
    char *digest = NULL;
    if (expected_bytes >= 0 && (int64_t)length != expected_bytes) {
      *why = get_formatted_string("the server sent %zu bytes, and the file "
                                  "is %" PRId64,
                                  length, expected_bytes);
    } else {
      digest = sha256_hash_bytes(body, length);
      if (!strings_equal(digest, expected_digest)) {
        *why = get_formatted_string("the server sent a file with sha256 %.8s",
                                    digest);
      } else if (!write_input_file(path, body, length)) {
        *why = get_formatted_string("could not write %s", path);
      } else {
        fetched = true;
      }
    }
    free(digest);
    free(body);
  }
  if (fetched) {
    const char *display_path = json_get_string_or_null(file, "path");
    thread_control_print_formatted(
        thread_control,
        "fetched %s from the server (sha256 %.8s, which the job requires), "
        "kept as %s\n",
        display_path ? display_path : json_get_string_or_null(file, "name"),
        expected_digest, path);
  } else if (!*why) {
    *why = string_duplicate("the server names nowhere this MAGPIE can fetch "
                            "it from");
  }
  return fetched;
}

// What the check of one expected file against the contributor's own data
// found.
typedef struct ExpectedFileCheck {
  // Where the name resolves in the contributor's data paths; NULL if nowhere.
  char *path;
  // Its digest; NULL if it could not be read.
  char *actual;
  bool matched;
} ExpectedFileCheck;

// Checks every file the claimed task will load against the digest the job
// pins, writing the ones this worker does not have and cannot get into
// `missing` as the JSON array the decline carries. Returns true when the task
// has every file -- and then contribute_get_task_data_paths says where it
// reads them from.
//
// Each file is first looked for in the contributor's own data (`data_paths`).
// If any is missing or has other bytes, and the server says where every such
// file can be fetched, they are fetched into a directory of their own (see
// above) that the task then reads ahead of the contributor's. A file that
// cannot be fetched -- an older server, a file it does not hold, a download
// that fails or is not the pinned bytes -- is a gap, as it always was.
//
// The check runs before the task starts: hashing is milliseconds, and a
// decline should not look like a worker that started and died. The claim has
// already checked that `expected_data` is there, names sha256, and lists only
// files with a role, name and digest, each role one this build knows
// (contribute_check_expected_data).
bool contribute_verify_expected_data(ContributeState *state,
                                     const char *data_paths,
                                     ThreadControl *thread_control,
                                     StringBuilder *missing) {
  free(state->task_data_paths);
  state->task_data_paths = NULL;
  const JsonValue *expected =
      json_object_get(state->assignment, "expected_data");
  const JsonValue *files = json_object_get(expected, "files");
  const int count = json_array_length(files);
  ErrorStack *errors = error_stack_create();
  ExpectedFileCheck *checks = (ExpectedFileCheck *)calloc_or_die(
      count > 0 ? (size_t)count : 1, sizeof(ExpectedFileCheck));

  // The contributor's own data first.
  int mismatched = 0;
  bool all_fetchable = true;
  StringBuilder *fetched_files = string_builder_create();
  for (int i = 0; i < count; i++) {
    const JsonValue *file = json_array_get(files, i);
    const char *role = json_get_string_or_null(file, "role");
    const char *name = json_get_string_or_null(file, "name");
    const char *expected_digest = json_get_string_or_null(file, "sha256");
    // Every entry has its three fields and a known role: the claim refused
    // the assignment otherwise.
    data_filepath_t type = DATA_FILEPATH_TYPE_KWG;
    (void)role_to_filepath_type(role, &type);

    // The name is the server's, and becomes a path: one that could leave the
    // data directory is never resolved, and so never hashed and reported
    // back. It is simply a file this worker does not have.
    ExpectedFileCheck *check = &checks[i];
    if (data_filepaths_is_safe_name(name)) {
      check->path =
          data_filepaths_get_readable_filename(data_paths, name, type, errors);
      if (error_stack_is_empty(errors)) {
        check->actual = hash_with_cache(state, check->path, errors);
      }
      error_stack_reset(errors);
    }
    check->matched =
        check->actual && strings_equal(check->actual, expected_digest);
    if (!check->matched) {
      mismatched++;
      all_fetchable = all_fetchable && input_file_is_fetchable(file);
      string_builder_add_formatted_string(fetched_files, "%s\t%s\t%s\n", role,
                                          name, expected_digest);
    }
  }

  // Then the server, for what the contributor's data does not have. One
  // directory per set of fetched files: two jobs that need the same file from
  // the server share it and the tables built from it, and two that pin
  // different bytes under one name never write over each other's.
  char *fetched_list = string_builder_dump_and_destroy(fetched_files, NULL);
  char *directory = mismatched > 0 && all_fetchable
                        ? contribute_input_directory(data_paths, fetched_list)
                        : NULL;
  free(fetched_list);

  bool all_matched = true;
  bool first_missing = true;
  for (int i = 0; i < count; i++) {
    ExpectedFileCheck *check = &checks[i];
    if (check->matched) {
      continue;
    }
    const JsonValue *file = json_array_get(files, i);
    const char *role = json_get_string_or_null(file, "role");
    const char *name = json_get_string_or_null(file, "name");
    const char *expected_digest = json_get_string_or_null(file, "sha256");
    const char *tarball_date = json_get_string_or_null(file, "tarball_date");
    const char *display_path = json_get_string_or_null(file, "path");
    const bool fetchable = input_file_is_fetchable(file);
    // With a file the server cannot send among them, the task is declined
    // whatever else is fetched, so nothing is; and the ones that could have
    // been are not what this worker is missing.
    if (fetchable && !directory) {
      continue;
    }
    char *why_not_fetched = NULL;
    if (fetchable) {
      data_filepath_t type = DATA_FILEPATH_TYPE_KWG;
      (void)role_to_filepath_type(role, &type);
      char *fetched_path = get_filepath(directory, name, type);
      const bool have = input_file_fetched(state, file, fetched_path,
                                           thread_control, &why_not_fetched);
      free(fetched_path);
      if (have) {
        free(why_not_fetched);
        continue;
      }
    }

    all_matched = false;
    if (!first_missing) {
      string_builder_add_string(missing, ",");
    }
    first_missing = false;
    char *entry = contribute_missing_file_json(role, name, expected_digest,
                                               check->actual);
    string_builder_add_string(missing, entry);
    free(entry);

    // Keyed by resolved path and expected digest: the same gap logs once, and
    // logs again only when the file or the expectation changes. The resolved
    // absolute path is printed because "your english.csv does not match" is
    // unactionable when the contributor has two of them.
    char *gap =
        check->actual
            ? get_formatted_string(
                  "  %-24s has sha256 %.8s, jobs require %.8s (%s)",
                  display_path ? display_path : name, check->actual,
                  expected_digest, check->path ? check->path : "unresolved")
            : get_formatted_string("  %-24s not found in any data path (%s)",
                                   display_path ? display_path : name,
                                   data_paths);
    char *log_key = get_formatted_string(
        "%s|%s", check->path ? check->path : name, expected_digest);
    if (!already_logged(state, log_key)) {
      thread_control_print_formatted_err(thread_control, "%s\n", gap);
      if (why_not_fetched) {
        // The tarball is not named: the server offered the file itself, so
        // the tarball it was first seen in may no longer carry it.
        thread_control_print_formatted_err(
            thread_control, "  it could not be fetched from the server: %s\n",
            why_not_fetched);
      } else if (tarball_date) {
        thread_control_print_formatted_err(
            thread_control,
            "  this comes from MAGPIE-DATA data-%s or later; run "
            "./download_data.sh to update\n",
            tarball_date);
      }
    }
    string_list_add_string(state->data_gaps, gap);
    free(log_key);
    free(gap);
    free(why_not_fetched);
  }

  if (all_matched && directory) {
    // Where the tables a task builds are written, whichever file was fetched:
    // the first data path is the one builds go to.
    char *lexica_file =
        get_filepath(directory, "lexicon", DATA_FILEPATH_TYPE_KWG);
    // Without it a build fails with its own message, and the task with it.
    (void)make_directories_for(lexica_file);
    free(lexica_file);
    state->task_data_paths = cpath_list_prepend(directory, data_paths);
  }

  for (int i = 0; i < count; i++) {
    free(checks[i].path);
    free(checks[i].actual);
  }
  free(checks);
  free(directory);
  error_stack_destroy(errors);
  return all_matched;
}

// ---------------------------------------------------------------------------
// Derived files
// ---------------------------------------------------------------------------

char *contribute_hash_file(ContributeState *state, const char *path,
                           ErrorStack *error_stack) {
  return hash_with_cache(state, path, error_stack);
}

int contribute_lock_build(const char *output_path, const char *what,
                          ThreadControl *thread_control,
                          ErrorStack *error_stack) {
  char *lock_path = get_formatted_string("%s.lock", output_path);
  const int lock_fd = cfile_lock_open(lock_path);
  free(lock_path);
  if (lock_fd < 0) {
    return -1;
  }
  bool said_waiting = false;
  bool busy = false;
  while (!cfile_lock_try(lock_fd, &busy)) {
    if (!busy) {
      // A filesystem without locks: build unserialised, as before.
      cfile_lock_close(lock_fd);
      return -1;
    }
    if (!said_waiting && thread_control) {
      thread_control_print_formatted(
          thread_control,
          "waiting for another MAGPIE process to finish building %s\n", what);
      said_waiting = true;
    }
    if (contribute_interrupted(thread_control)) {
      cfile_lock_close(lock_fd);
      error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_INTERRUPTED,
                       get_formatted_string(
                           "stopped while waiting for %s to be built", what));
      return -1;
    }
    ctime_nap(1.0);
  }
  return lock_fd;
}

void contribute_unlock_build(int lock_fd) {
  // Closing releases the lock.
  cfile_lock_close(lock_fd);
}

bool contribute_find_derived(const ContributeState *state, const char *role,
                             const char *name, ContributeDerived *out) {
  if (!state || !state->assignment) {
    return false;
  }
  const JsonValue *expected =
      json_object_get(state->assignment, "expected_data");
  if (!expected) {
    return false;
  }
  const JsonValue *derived = json_object_get(expected, "derived");
  const int count = json_array_length(derived);
  for (int i = 0; i < count; i++) {
    const JsonValue *entry = json_array_get(derived, i);
    const char *entry_role = json_get_string_or_null(entry, "role");
    const char *entry_name = json_get_string_or_null(entry, "name");
    const char *sha256 = json_get_string_or_null(entry, "sha256");
    if (!entry_role || !entry_name || !sha256 ||
        !strings_equal(entry_role, role) || !strings_equal(entry_name, name)) {
      continue;
    }
    out->role = entry_role;
    out->name = entry_name;
    out->sha256 = sha256;
    out->builder = json_get_string_or_null(entry, "builder");
    out->build_target = json_get_string_or_null(entry, "build_target");
    // A size that is not a whole number of bytes is none: the build is tried,
    // and finds out.
    out->bytes = -1;
    double bytes = 0;
    if (json_number(json_object_get(entry, "bytes"), &bytes) && bytes >= 0 &&
        bytes <= (double)INT64_MAX / 2 && bytes == (double)(int64_t)bytes) {
      out->bytes = (int64_t)bytes;
    }
    return true;
  }
  return false;
}

void contribute_record_derived_skipped(ContributeState *state,
                                       contribute_table_role_t role) {
  if (state && role < NUMBER_OF_CONTRIBUTE_TABLE_ROLES) {
    state->derived_skipped |= 1U << role;
  }
}

unsigned int contribute_get_derived_skipped(const ContributeState *state) {
  return state ? state->derived_skipped : 0;
}

int64_t contribute_get_disk_reserve_bytes(const ContributeState *state) {
  if (!state) {
    return (int64_t)CONTRIBUTE_DEFAULT_DISK_RESERVE_GB *
           CONTRIBUTE_BYTES_PER_GB;
  }
  return state->client_state->disk_reserve_bytes;
}

int64_t contribute_get_memory_reserve_bytes(const ContributeState *state) {
  if (!state) {
    return (int64_t)CONTRIBUTE_DEFAULT_MEMORY_RESERVE_GB *
           CONTRIBUTE_BYTES_PER_GB;
  }
  return state->client_state->memory_reserve_bytes;
}

bool contribute_first_mention(ContributeState *state, const char *key) {
  return !state || !already_logged(state, key);
}

void contribute_state_set_assignment(ContributeState *state,
                                     JsonValue *assignment) {
  json_destroy(state->assignment);
  state->assignment = assignment;
  state->derived_skipped = 0;
}

char *contribute_missing_file_json(const char *role, const char *name,
                                   const char *expected, const char *actual) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "role", role, &first);
  json_write_string_field(sb, "name", name, &first);
  json_write_string_field(sb, "expected", expected, &first);
  if (actual) {
    json_write_string_field(sb, "actual", actual, &first);
  }
  json_write_object_end(sb);
  return string_builder_dump_and_destroy(sb, NULL);
}

void contribute_record_derived_mismatch(ContributeState *state,
                                        const char *role, const char *name,
                                        const char *expected,
                                        const char *actual) {
  char *entry = contribute_missing_file_json(role, name, expected, actual);
  string_list_add_string(state->derived_mismatches, entry);
  free(entry);

  // The leave KLV is fetched from the server, not built here: a mismatch is
  // the server's to fix, and nothing on this disk is out of date.
  if (strings_equal(role, "klv")) {
    state->server_artifact_mismatch = true;
    return;
  }

  // Kept for the shutdown summary alongside the input-data gaps, in the same
  // shape, because from a contributor's point of view they are the same
  // problem: a file on this disk is not the file the job means.
  char *gap =
      get_formatted_string("  %-24s built as sha256 %.8s, jobs require %.8s",
                           name, actual ? actual : "(unbuilt)", expected);
  string_list_add_string(state->data_gaps, gap);
  free(gap);
}

// A worker with neither an API key nor a UUID sends no identity at all -- the
// server mints one and hands it back the first time it actually assigns a
// task. This persists that assignment for the rest of the run and to the
// settings file for every run after.
//
// Only a UUID in canonical form is taken: anything else would go into a
// request header and a line of the settings file as it came, and a newline in
// it added settings of the server's choosing that every later run obeyed. One
// that is not is ignored, and the next claim asks again. One the settings file
// will not take is used for this run and said so, since a run that cannot save
// it becomes a new anonymous worker every time it starts.
static void adopt_server_assigned_uuid(ContributeState *state,
                                       ThreadControl *thread_control,
                                       const JsonValue *assignment) {
  ClientState *client_state = state->client_state;
  if (client_state->api_key || client_state->worker_uuid) {
    return;
  }
  const char *worker_uuid = json_get_string_or_null(assignment, "worker_uuid");
  if (!worker_uuid) {
    return;
  }
  // Nothing a contributor could do about either of these: the next claim asks
  // again, and a settings file created to hold the identity is what the run
  // is meant to do.
  if (!client_state_is_worker_uuid(worker_uuid)) {
    return;
  }
  if (client_state_set_worker_uuid(client_state, worker_uuid)) {
    client_state->settings_file_found = true;
  } else {
    thread_control_print_formatted_err(
        thread_control,
        "could not save this worker's identity to %s; add the line\n"
        "  uuid %s\n"
        "to it yourself (replacing any part of it left at the end), or every "
        "run will be a new anonymous worker\n",
        client_state->settings_path, worker_uuid);
  }
  http_client_set_worker_uuid(state->http_client, worker_uuid);
}

static bool deferral_active(const DeferredJob *deferred) {
  return deferred->until_ns > now_ns();
}

static DeferredJob *find_deferral(ContributeState *state, const char *job_id) {
  for (int i = 0; i < state->deferred_count; i++) {
    if (strings_equal(state->deferred[i].job_id, job_id)) {
      return &state->deferred[i];
    }
  }
  return NULL;
}

bool contribute_shutdown_waits_for_deferral(const JsonValue *shutdown,
                                            bool claim_named_deferred) {
  // Only a shutdown about data: one about this build's version
  // (`magpie_too_old`) or its board and rack (`unsupported_build`) has
  // nothing to do with the jobs set aside, and is obeyed at once.
  const char *reason = json_get_string_or_null(shutdown, "reason");
  return claim_named_deferred && reason &&
         (strings_equal(reason, "data_out_of_date") ||
          strings_equal(reason, "both"));
}

// Sets `job_id` aside, for twice as long as last time (from the idle interval
// up to CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS). Returns the interval.
int contribute_defer_job(ContributeState *state, const char *job_id) {
  const int idle = state->client_state->idle_wait_seconds;
  DeferredJob *deferred = find_deferral(state, job_id);
  if (!deferred) {
    if (state->deferred_count == state->deferred_capacity) {
      state->deferred_capacity =
          state->deferred_capacity ? state->deferred_capacity * 2 : 4;
      state->deferred = (DeferredJob *)realloc_or_die(
          state->deferred,
          (size_t)state->deferred_capacity * sizeof(DeferredJob));
    }
    deferred = &state->deferred[state->deferred_count++];
    deferred->job_id = string_duplicate(job_id);
    deferred->wait_seconds = 0;
  }
  // The first wait is the idle wait (at least a second), each after it
  // twice the last.
  int wait = deferred->wait_seconds * 2;
  if (deferred->wait_seconds <= 0) {
    wait = idle > 0 ? idle : 1;
  }
  if (wait > CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS) {
    wait = CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS;
  }
  deferred->wait_seconds = wait;
  deferred->until_ns = now_ns() + (int64_t)wait * 1000000000LL;
  return wait;
}

void contribute_artifact_verified(ContributeState *state) {
  if (!state->claimed_job_id) {
    return;
  }
  for (int i = 0; i < state->deferred_count; i++) {
    if (strings_equal(state->deferred[i].job_id, state->claimed_job_id)) {
      free(state->deferred[i].job_id);
      state->deferred[i] = state->deferred[--state->deferred_count];
      return;
    }
  }
}

// Seconds until the first active deferral ends, or 0 if none is active.
static int seconds_until_a_deferral_ends(const ContributeState *state) {
  int64_t soonest = 0;
  for (int i = 0; i < state->deferred_count; i++) {
    const int64_t left = state->deferred[i].until_ns - now_ns();
    if (left > 0 && (soonest == 0 || left < soonest)) {
      soonest = left;
    }
  }
  return soonest == 0 ? 0 : (int)(soonest / 1000000000LL) + 1;
}

static bool interrupted_check(void *thread_control) {
  return contribute_interrupted((ThreadControl *)thread_control);
}

ContributeState *contribute_state_create(const char *settings_path,
                                         ThreadControl *thread_control,
                                         ErrorStack *error_stack) {
  ClientState *client_state = client_state_load(settings_path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  // Zeroed: every field not set below starts false, zero or NULL. Fields
  // added without a line here were read uninitialized -- and freed at the
  // end of every run.
  ContributeState *state =
      (ContributeState *)calloc_or_die(1, sizeof(ContributeState));
  state->client_state = client_state;
  state->threads = client_state->threads;
  if (state->threads <= 0) {
    // Leave the machine usable. Contributing is a background activity someone
    // opts into on their daily driver, and one that makes the machine
    // unresponsive is one they turn off.
    const int cores = get_num_cores();
    state->threads = cores > 1 ? cores - 1 : 1;
  }
  // See CONTRIBUTE_MAX_THREADS. contribute.txt's value never passes through
  // -threads, and the default (cores - 1) reaches the cap on a large enough
  // machine.
  if (state->threads > CONTRIBUTE_MAX_THREADS) {
    state->threads = CONTRIBUTE_MAX_THREADS;
  }
  state->http_client =
      http_client_create(client_state->server_url, client_state->api_key,
                         client_state->worker_uuid);
  state->completed = 0;
  state->consecutive_failures = 0;
  state->last_failure = NULL;
  state->stop = false;
  state->unsupported_jobs = string_list_create();
  state->data_gaps = string_list_create();
  state->logged_gaps = string_list_create();
  state->digest_cache = string_list_create();
  state->derived_mismatches = string_list_create();
  state->derived_skipped = 0;
  state->shutdown_requested = false;
  state->reached_server = false;
  // Nothing is said while a request waits for the server, however long: a
  // claim waits out an outage, and only a request that gives up says so, by
  // ending the run with its error.
  http_client_set_abort_check(state->http_client, interrupted_check,
                              thread_control);
  state->input_fetch = fetch_input_over_http;
  state->input_fetch_context = state;
  memset(&state->heartbeat, 0, sizeof(state->heartbeat));
  state->claim_token = NULL;
  state->claimed_job_id = NULL;
  state->assignment = NULL;

  if (!client_state->api_key && client_state->commented_key_line) {
    thread_control_print_formatted_err(
        thread_control,
        "%s line %d is a comment holding an apikey; if it is meant as the "
        "setting, put it on a line of its own\n",
        client_state->settings_path, client_state->commented_key_line);
  }
  return state;
}

bool contribute_interrupted(ThreadControl *thread_control) {
  return thread_control && thread_control_get_status(thread_control) ==
                               THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// Sleeps `seconds`, a second at a time, returning early on a stop request:
// a REPL `stop` during a wait of minutes otherwise went unanswered.
static void nap_unless_interrupted(ThreadControl *thread_control, int seconds) {
  for (int i = 0; i < seconds && !contribute_interrupted(thread_control); i++) {
    ctime_nap(1.0);
  }
}

// Remembers a job this worker cannot run, so the server stops offering it.
// Tracking this is the whole point of the decline path: a client that declines
// and forgets would claim the same task again immediately.
static void remember_unsupported(ContributeState *state, const char *job_id) {
  if (!job_id) {
    return;
  }
  const int count = string_list_get_count(state->unsupported_jobs);
  for (int i = 0; i < count; i++) {
    if (strings_equal(string_list_get_string(state->unsupported_jobs, i),
                      job_id)) {
      return;
    }
  }
  string_list_add_string(state->unsupported_jobs, job_id);
}

char *contribute_decline_body(const char *claim_token, const char *reason,
                              const char *missing_json) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "claim_token", claim_token, &first);
  json_write_string_field(sb, "reason", reason, &first);
  json_write_array_start(sb, "missing", &first);
  if (missing_json) {
    string_builder_add_string(sb, missing_json);
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  return string_builder_dump_and_destroy(sb, NULL);
}

// POST /api/worker/decline: hand the claim straight back rather than letting
// it lapse on the heartbeat timeout, and tell the server which files did not
// match so an admin can see what the fleet is missing.
static void decline_over_http(ContributeState *state, const char *reason,
                              const char *missing_json,
                              ErrorStack *error_stack) {
  char *body =
      contribute_decline_body(state->claim_token, reason, missing_json);

  ChttpResponse response;
  http_client_post_json(state->http_client, "/api/worker/decline", body,
                        &response, error_stack);
  free(body);
  if (error_stack_is_empty(error_stack)) {
    chttp_response_destroy(&response);
  }
}

// Everything a claim owns, released whether the task ran or was declined.
static void release_claim(ContributeState *state) {
  heartbeat_stop(&state->heartbeat);
  // A deadline still running is one nothing asked about: the task ended
  // some other way first.
  (void)contribute_deadline_finish(state->deadline);
  state->deadline = NULL;
  free(state->claim_token);
  state->claim_token = NULL;
  free(state->claimed_job_id);
  state->claimed_job_id = NULL;
  free(state->job_name);
  state->job_name = NULL;
  json_destroy(state->assignment);
  state->assignment = NULL;
  state->derived_skipped = 0;
  free(state->task_data_paths);
  state->task_data_paths = NULL;
}

// A job name from the server, as printed: control characters (a terminal
// escape, a newline that would forge a line of output) become '?', and a name
// past the server's own limit is cut short.
static char *printable_job_name(const char *name) {
  StringBuilder *sb = string_builder_create();
  for (int char_idx = 0; char_idx < MAX_PRINTED_JOB_NAME && name[char_idx];
       char_idx++) {
    const unsigned char byte = (unsigned char)name[char_idx];
    if (byte < 0x20 || byte == 0x7f) {
      string_builder_add_char(sb, '?');
    } else {
      string_builder_add_char(sb, name[char_idx]);
    }
  }
  return string_builder_dump_and_destroy(sb, NULL);
}

void contribute_read_job_name_and_limit(const JsonValue *assignment,
                                        char **job_name, int *max_task_seconds,
                                        ErrorStack *error_stack) {
  *job_name = NULL;
  *max_task_seconds = 0;
  const char *name =
      json_get_string(assignment, CONTRIBUTE_KEY_JOB_NAME, error_stack);
  const int64_t seconds =
      json_get_int(assignment, CONTRIBUTE_KEY_MAX_TASK_SECONDS, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (seconds < 1 || seconds > INT32_MAX) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     get_formatted_string(
                         "the server sent a task with an unusable %s: %lld",
                         CONTRIBUTE_KEY_MAX_TASK_SECONDS, (long long)seconds));
    return;
  }
  *job_name = printable_job_name(name);
  *max_task_seconds = (int)seconds;
}

char *contribute_elapsed_text(int64_t seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  const int64_t units[] = {86400, 3600, 60, 1};
  const char *names[] = {"d", "h", "m", "s"};
  StringBuilder *sb = string_builder_create();
  for (int i = 0; i < 4; i++) {
    const int64_t count = seconds / units[i];
    seconds %= units[i];
    if (count > 0) {
      string_builder_add_formatted_string(sb, "%s%" PRId64 "%s",
                                          string_builder_length(sb) ? " " : "",
                                          count, names[i]);
    }
  }
  if (string_builder_length(sb) == 0) {
    string_builder_add_string(sb, "0s");
  }
  return string_builder_dump_and_destroy(sb, NULL);
}

// The claimed task's time so far, as contribute_elapsed_text prints it.
static char *task_elapsed_text(const ContributeState *state) {
  return contribute_elapsed_text((now_ns() - state->task_started_ns) /
                                 1000000000);
}

// "[14:03:07] started task #3: <job name><suffix>", in local time.
static void print_task_line(ThreadControl *thread_control, const char *what,
                            int task_number, const char *job_name,
                            const char *suffix) {
  const time_t now = time(NULL);
  struct tm local;
  char clock[16] = "??:??:??";
  if (cfile_localtime(now, &local)) {
    (void)strftime(clock, sizeof(clock), "%H:%M:%S", &local);
  }
  thread_control_print_formatted(thread_control, "[%s] %s task #%d: %s%s\n",
                                 clock, what, task_number,
                                 job_name ? job_name : "", suffix);
}

// Prints what this worker is missing, in full, once -- the client knows it
// file by file, and the server's message cannot.
static void print_shutdown(const ContributeState *state,
                           ThreadControl *thread_control,
                           const JsonValue *shutdown) {
  thread_control_print_formatted_err(
      thread_control, "\nCannot contribute to any available job.\n");
  const int gaps = string_list_get_count(state->data_gaps);
  if (gaps > 0) {
    thread_control_print_formatted_err(thread_control,
                                       "\nMissing or outdated input data:\n");
    for (int i = 0; i < gaps; i++) {
      thread_control_print_formatted_err(
          thread_control, "%s\n", string_list_get_string(state->data_gaps, i));
    }
  }
  const char *message = json_get_string_or_null(shutdown, "message");
  if (message) {
    thread_control_print_formatted_err(thread_control, "\n%s\n", message);
  }
  const char *reason = json_get_string_or_null(shutdown, "reason");
  if (reason && strings_equal(reason, "unsupported_build")) {
    // Not a version or data problem, so neither of the fixes below: this
    // binary was compiled for another board or rack, and only a rebuild with
    // the defaults changes that.
    thread_control_print_formatted_err(
        thread_control,
        "This MAGPIE was built with BOARD_DIM=%d and RACK_SIZE=%d. Rebuild it "
        "without setting either (make magpie with no BOARD_DIM= or "
        "RACK_SIZE=), then start contribute again.\n",
        BOARD_DIM, RACK_SIZE);
  }
  const char *required_version =
      json_get_string_or_null(shutdown, "required_magpie_version");
  const char *download_url = json_get_string_or_null(shutdown, "download_url");
  if (required_version) {
    thread_control_print_formatted_err(
        thread_control, "Update MAGPIE to %s or newer%s%s.\n", required_version,
        download_url ? ": " : "", download_url ? download_url : "");
  }
  const JsonValue *dates = json_object_get(shutdown, "required_tarball_dates");
  const int date_count = json_array_length(dates);
  for (int i = 0; i < date_count; i++) {
    const char *date = json_array_get_string(dates, i);
    if (date) {
      thread_control_print_formatted_err(
          thread_control,
          "Run ./download_data.sh from your MAGPIE directory to install "
          "MAGPIE-DATA data-%s or later, then start contribute again.\n",
          date);
    }
  }
}

// The body is required. Every field is load-bearing: the version drives the
// per-job floor filter; the board dimension and rack size are this build's
// compile-time BOARD_DIM and RACK_SIZE, which no version or digest reveals
// and which the server refuses unless they are the ones its jobs play with
// (a build with another rack size would score every bingo differently and
// pass every other check); and the unsupported set is what keeps the server
// from offering work this worker has already found it cannot do -- for good,
// or (the set-aside jobs, after the others) for now.
char *contribute_claim_body(ContributeState *state,
                            const char *this_magpie_version) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "magpie_version", this_magpie_version, &first);
  json_write_int_field(sb, "board_dim", BOARD_DIM, &first);
  json_write_int_field(sb, "rack_size", RACK_SIZE, &first);
  json_write_array_start(sb, "unsupported_jobs", &first);
  const int unsupported_count = string_list_get_count(state->unsupported_jobs);
  bool first_job = true;
  for (int i = 0; i < unsupported_count; i++) {
    if (!first_job) {
      string_builder_add_string(sb, ",");
    }
    first_job = false;
    json_write_quoted(sb, string_list_get_string(state->unsupported_jobs, i));
  }
  state->claim_named_deferred = false;
  for (int i = 0; i < state->deferred_count; i++) {
    if (!deferral_active(&state->deferred[i])) {
      continue;
    }
    if (!first_job) {
      string_builder_add_string(sb, ",");
    }
    first_job = false;
    json_write_quoted(sb, state->deferred[i].job_id);
    state->claim_named_deferred = true;
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  return string_builder_dump_and_destroy(sb, NULL);
}

static contribute_claim_outcome_t
claim_task_over_http(ContributeState *state, const char *this_magpie_version,
                     ErrorStack *error_stack) {
  ChttpResponse response;
  // state is never NULL here: contribute_claim_task only reaches this call
  // with *state_ptr set, either because it was already non-NULL or because
  // contribute_state_create just returned non-NULL for it, and
  // contribute_state_create's only failure path (client_state_load) returns
  // NULL exactly when error_stack is non-empty, which contribute_claim_task
  // already checks and returns on before this point.
  char *claim_body = contribute_claim_body(state, this_magpie_version);

  // A claim that cannot reach the server keeps asking, once this run has been
  // answered by that server at all. An outage has no length a client can know
  // -- a deployment is a minute, a restore from backup is most of an hour --
  // and the two ways of being wrong are not alike: a claim a minute against a
  // server that is away costs nothing, while a run that gave up is a
  // contributor's machine lost until its owner happens to look. Nothing is
  // held while it waits: no claim, no heartbeat, no result.
  //
  // A run that has *never* been answered keeps the finite budget, so a
  // mistyped `server` line still ends with an error rather than a client
  // polling nothing for ever. So does every other request: a submission's
  // claim lapses on the server whatever the client does.
  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  if (state->reached_server) {
    http_client_post_json_persistent(state->http_client, "/api/worker/task",
                                     claim_body, &response, error_stack);
  } else {
    http_client_post_json(state->http_client, "/api/worker/task", claim_body,
                          &response, error_stack);
  }
  free(claim_body);
  if (!error_stack_is_empty(error_stack)) {
    return CONTRIBUTE_CLAIM_FAILED;
  }
  state->reached_server = true;

  // 204 is the normal state of a quiet server, not a failure.
  if (response.status_code == 204) {
    chttp_response_destroy(&response);
    return CONTRIBUTE_CLAIM_NO_WORK;
  }
  if (response.status_code != 200) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string("claiming a task failed with HTTP %ld: %.200s",
                             response.status_code,
                             response.body ? response.body : ""));
    chttp_response_destroy(&response);
    return CONTRIBUTE_CLAIM_FAILED;
  }

  state->assignment = json_parse(response.body, error_stack);
  state->derived_skipped = 0;
  chttp_response_destroy(&response);
  return error_stack_is_empty(error_stack) ? CONTRIBUTE_CLAIM_GOT_TASK
                                           : CONTRIBUTE_CLAIM_FAILED;
}

contribute_claim_outcome_t
contribute_claim_task(ContributeState **state_ptr, const char *settings_path,
                      const char *this_magpie_version, const char *data_paths,
                      ThreadControl *thread_control, const char **out_job_type,
                      const JsonValue **out_task_request,
                      ErrorStack *error_stack) {
  if (*state_ptr == NULL) {
    *state_ptr =
        contribute_state_create(settings_path, thread_control, error_stack);
    if (!error_stack_is_empty(error_stack) || *state_ptr == NULL) {
      return CONTRIBUTE_CLAIM_FAILED;
    }
  }
  ContributeState *state = *state_ptr;

  const contribute_claim_outcome_t outcome =
      claim_task_over_http(state, this_magpie_version, error_stack);
  if (outcome == CONTRIBUTE_CLAIM_FAILED) {
    return outcome;
  }
  if (outcome == CONTRIBUTE_CLAIM_NO_WORK) {
    nap_unless_interrupted(thread_control,
                           state->client_state->idle_wait_seconds);
    return outcome;
  }

  // "Nothing you can do until something on your end changes" -- the opposite
  // of a 204, which is a quiet server. Exit cleanly, having said what is
  // wrong and what to do about it.
  const JsonValue *shutdown = json_object_get(state->assignment, "shutdown");
  // Nothing claimable may only be because of the jobs set aside for a while
  // (a server KLV that was missing or wrong): the server cannot tell those
  // from jobs this worker cannot run at all. Wait for the first to come back
  // rather than end the run over a condition an admin is expected to fix.
  if (shutdown && contribute_shutdown_waits_for_deferral(
                      shutdown, state->claim_named_deferred)) {
    json_destroy(state->assignment);
    state->assignment = NULL;
    nap_unless_interrupted(thread_control,
                           seconds_until_a_deferral_ends(state));
    return CONTRIBUTE_CLAIM_NO_WORK;
  }
  if (shutdown) {
    print_shutdown(state, thread_control, shutdown);
    state->shutdown_requested = true;
    state->stop = true;
    json_destroy(state->assignment);
    state->assignment = NULL;
    return CONTRIBUTE_CLAIM_SHUTDOWN;
  }

  adopt_server_assigned_uuid(state, thread_control, state->assignment);

  const char *claim_token =
      json_get_string(state->assignment, "claim_token", error_stack);
  const char *job_id =
      json_get_string(state->assignment, "job_id", error_stack);
  const JsonValue *request = json_object_get(state->assignment, "task_request");
  const char *job_type =
      request ? json_get_string(request, "job_type", error_stack) : "";
  char *job_name = NULL;
  int max_task_seconds = 0;
  if (error_stack_is_empty(error_stack)) {
    contribute_read_job_name_and_limit(state->assignment, &job_name,
                                       &max_task_seconds, error_stack);
  }
  if (error_stack_is_empty(error_stack)) {
    contribute_check_expected_data(state->assignment, error_stack);
  }
  if (!error_stack_is_empty(error_stack)) {
    free(job_name);
    json_destroy(state->assignment);
    state->assignment = NULL;
    return CONTRIBUTE_CLAIM_FAILED;
  }

  state->claim_token = string_duplicate(claim_token);
  state->claimed_job_id = string_duplicate(job_id);
  state->job_name = job_name;
  state->max_task_seconds = max_task_seconds;

  // The server filters on version before it dispatches, so reaching this with
  // a job above this build is a server bug or a race with a floor that was
  // just raised. Either way it is one more job this worker cannot do, not a
  // reason to end the session: other jobs may be well within reach.
  const char *min_version =
      json_get_string_or_null(state->assignment, "min_magpie_version");
  if (min_version &&
      contribute_compare_versions(this_magpie_version, min_version) < 0) {
    thread_control_print_formatted_err(
        thread_control,
        "declining a job that requires MAGPIE %s; this build is %s\n",
        min_version, this_magpie_version);
    decline_over_http(state, "magpie_version", NULL, error_stack);
    remember_unsupported(state, state->claimed_job_id);
    release_claim(state);
    return error_stack_is_empty(error_stack) ? CONTRIBUTE_CLAIM_DECLINED
                                             : CONTRIBUTE_CLAIM_FAILED;
  }

  // Verified before the heartbeat starts: hashing is milliseconds, and a
  // decline should not look like a worker that started and died.
  StringBuilder *missing = string_builder_create();
  const bool data_ok = contribute_verify_expected_data(state, data_paths,
                                                       thread_control, missing);
  char *missing_json = string_builder_dump_and_destroy(missing, NULL);
  if (!data_ok) {
    // Started if a file was being fetched; the claim is about to be handed
    // back, and a heartbeat for it after that is refused.
    heartbeat_stop(&state->heartbeat);
    decline_over_http(state, "missing_data", missing_json, error_stack);
    free(missing_json);
    remember_unsupported(state, state->claimed_job_id);
    release_claim(state);
    return error_stack_is_empty(error_stack) ? CONTRIBUTE_CLAIM_DECLINED
                                             : CONTRIBUTE_CLAIM_FAILED;
  }
  free(missing_json);

  // Running already if a pinned file was fetched (input_file_fetched).
  if (!state->heartbeat.running) {
    heartbeat_start(&state->heartbeat, state->http_client, state->claim_token);
  }
  state->started++;
  state->task_number = state->started;
  print_task_line(thread_control, "started", state->task_number,
                  state->job_name, "");
  // From here, the task's time is running: the derived files it builds count
  // against it as well as the run itself.
  state->task_started_ns = now_ns();
  state->deadline =
      contribute_deadline_start(thread_control, state->max_task_seconds);

  *out_job_type = job_type;
  *out_task_request = request;
  return CONTRIBUTE_CLAIM_GOT_TASK;
}

bool contribute_task_hit_time_limit(ContributeState *state) {
  if (!state) {
    return false;
  }
  const bool fired = contribute_deadline_finish(state->deadline);
  state->deadline = NULL;
  return fired;
}

void contribute_hand_back_timed_out_task(ContributeState *state,
                                         ThreadControl *thread_control) {
  if (!state || !state->claim_token) {
    return;
  }
  char *elapsed = task_elapsed_text(state);
  char *counts = get_formatted_string(
      " at its %d-second limit after %s, handed back (%d started, %d "
      "completed)",
      state->max_task_seconds, elapsed, state->started, state->completed);
  free(elapsed);
  print_task_line(thread_control, "stopped", state->task_number,
                  state->job_name, counts);
  free(counts);
  // Not remembered as a job this worker cannot run, and its errors dropped,
  // as a failed task's are (decline_failed_task): whether a job's tasks fit
  // the limit is the server's to judge across its workers -- it sets aside a
  // job whose tasks keep running out of time -- and an undelivered decline
  // costs only the time until the server lapses the claim.
  ErrorStack *decline_errors = error_stack_create();
  decline_over_http(state, CONTRIBUTE_DECLINE_TIME_LIMIT, NULL, decline_errors);
  error_stack_destroy(decline_errors);
  release_claim(state);
}

void contribute_decline_task(ContributeState *state,
                             ThreadControl *thread_control, const char *reason,
                             ErrorStack *error_stack) {
  (void)thread_control;
  if (!state || !state->claim_token) {
    return;
  }
  decline_over_http(state, reason, NULL, error_stack);
  remember_unsupported(state, state->claimed_job_id);
  release_claim(state);
}

void contribute_decline_derived_mismatch(ContributeState *state,
                                         ThreadControl *thread_control,
                                         const char *why,
                                         ErrorStack *error_stack) {
  if (!state || !state->claim_token) {
    return;
  }
  StringBuilder *sb = string_builder_create();
  const int count = string_list_get_count(state->derived_mismatches);
  for (int i = 0; i < count; i++) {
    if (i > 0) {
      string_builder_add_string(sb, ",");
    }
    string_builder_add_string(
        sb, string_list_get_string(state->derived_mismatches, i));
  }
  char *missing_json = string_builder_dump_and_destroy(sb, NULL);
  const bool server_artifact = state->server_artifact_mismatch;
  // Said only for a file built here, which this worker's MAGPIE has to
  // change to match (and which goes on the shutdown's list besides). The
  // server's leave KLV is the server's to put right, and the worker simply
  // comes back to the job later.
  if (server_artifact) {
    (void)contribute_defer_job(state, state->claimed_job_id);
  } else {
    if (why) {
      thread_control_print_formatted_err(thread_control, "%s\n", why);
    }
    thread_control_print_formatted_err(
        thread_control,
        "declining this task: a file built here does not match what the job "
        "pins\n");
  }
  decline_over_http(state, "derived_mismatch", missing_json, error_stack);
  free(missing_json);
  string_list_destroy(state->derived_mismatches);
  state->derived_mismatches = string_list_create();
  state->server_artifact_mismatch = false;
  // A file built here stays wrong for the run, so the job is set aside for
  // good. The server's KLV does not: an admin's "Check artifacts" or a
  // restored object version puts it right, so the job is set aside only for
  // a while (above) and its KLV fetched afresh after. Setting it aside for
  // good ended the run of every worker for whom it was the only active job,
  // with the server's "every active job needs input data you do not have"
  // sending the contributor to the wrong fix; and napping the whole worker,
  // as it next did, idled it for every other job too.
  if (!server_artifact) {
    remember_unsupported(state, state->claimed_job_id);
  }
  release_claim(state);
}

char *contribute_result_body(const char *claim_token, const char *result_json,
                             uint64_t movegens, unsigned int derived_skipped) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "claim_token", claim_token, &first);
  json_write_raw_key(sb, "result", &first);
  string_builder_add_string(sb, result_json);
  // Beside the result, not in it: it is what this machine spent on the task,
  // credited to the contributor, not part of what the task computed.
  json_write_raw_key(sb, "movegens", &first);
  string_builder_add_formatted_string(sb, "%" PRIu64, movegens);
  // Beside it too, and always there, empty when nothing was skipped: the
  // tables the task's players asked for that this machine had no room for,
  // which the task ran without -- slower, with the same moves. Sorted, so
  // the same skips always read the same.
  json_write_array_start(sb, "derived_skipped", &first);
  static const contribute_table_role_t sorted_roles[] = {
      CONTRIBUTE_TABLE_RIT, CONTRIBUTE_TABLE_WIT, CONTRIBUTE_TABLE_WMP};
  bool first_role = true;
  for (size_t role_idx = 0;
       role_idx < sizeof(sorted_roles) / sizeof(sorted_roles[0]); role_idx++) {
    if (derived_skipped & (1U << sorted_roles[role_idx])) {
      if (!first_role) {
        string_builder_add_string(sb, ",");
      }
      first_role = false;
      json_write_quoted(sb, contribute_table_role_name(sorted_roles[role_idx]));
    }
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  return string_builder_dump_and_destroy(sb, NULL);
}

typedef enum {
  CONTRIBUTE_SUBMIT_ACCEPTED,
  // 200 with {"accepted": false}: the claim had lapsed and been reassigned,
  // or this result was already accepted. Nothing to fix and nothing to count.
  CONTRIBUTE_SUBMIT_NOT_ACCEPTED,
  // A 4xx other than 429 (which the HTTP client retries): the server refused
  // this result. That is a property of this task -- or of a disagreement
  // between this build and the server -- not of the connection, so it counts
  // as one task failure rather than ending the run.
  CONTRIBUTE_SUBMIT_REJECTED,
} contribute_submit_outcome_t;

// On CONTRIBUTE_SUBMIT_REJECTED, *rejection is set to a message the caller
// frees. A transport failure or a 5xx that outlasted the client's retries
// goes on error_stack and ends the run, as before.
static contribute_submit_outcome_t
submit_result_over_http(HttpClient *client, const char *claim_token,
                        const char *result_json, uint64_t movegens,
                        unsigned int derived_skipped, char **rejection,
                        ErrorStack *error_stack) {
  char *body = contribute_result_body(claim_token, result_json, movegens,
                                      derived_skipped);

  ChttpResponse response;
  http_client_post_json(client, "/api/worker/result", body, &response,
                        error_stack);
  free(body);
  if (!error_stack_is_empty(error_stack)) {
    return CONTRIBUTE_SUBMIT_REJECTED;
  }

  contribute_submit_outcome_t outcome = CONTRIBUTE_SUBMIT_ACCEPTED;
  if (response.status_code == 200) {
    ErrorStack *parse_errors = error_stack_create();
    const JsonValue *ack =
        response.body ? json_parse(response.body, parse_errors) : NULL;
    if (ack && error_stack_is_empty(parse_errors) &&
        !json_get_bool_or(ack, "accepted", true)) {
      outcome = CONTRIBUTE_SUBMIT_NOT_ACCEPTED;
    }
    json_destroy(ack);
    error_stack_destroy(parse_errors);
  } else if (response.status_code >= 400 && response.status_code < 500) {
    *rejection = get_formatted_string(
        "the server rejected the result with HTTP %ld: %.200s",
        response.status_code, response.body ? response.body : "");
    outcome = CONTRIBUTE_SUBMIT_REJECTED;
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string("submitting a result failed with HTTP %ld: %.200s",
                             response.status_code,
                             response.body ? response.body : ""));
  }
  chttp_response_destroy(&response);
  return outcome;
}

// Hands back a claim this worker could not produce an accepted result for,
// rather than leaving it to lapse. Stopping the heartbeat alone held the
// task's slot for the server's whole heartbeat timeout -- five minutes by
// default -- before anyone else could have it. The job is deliberately not
// remembered as unsupported: a failure is a property of this attempt, and the
// consecutive-failure guard is what stops a worker that fails every time. A
// decline that cannot be delivered only costs the timeout it was meant to
// save, so its errors are dropped rather than ending the run.
static void decline_failed_task(ContributeState *state) {
  ErrorStack *decline_errors = error_stack_create();
  decline_over_http(state, "task_failed", NULL, decline_errors);
  error_stack_destroy(decline_errors);
}

void contribute_submit_result(ContributeState *state,
                              ThreadControl *thread_control,
                              const char *result_json, uint64_t movegens,
                              const char *error_message,
                              ErrorStack *error_stack) {
  // The heartbeat is kept going through the submission below, not stopped
  // here as it used to be. A claim is only as alive as its last heartbeat, and
  // a submission is not instant: a batch with captured positions is tens of
  // megabytes on a contributor's uplink, and a server that is restarting is
  // retried for a quarter of an hour (http_client_backoff_seconds). With the
  // heartbeat already stopped, the claim could lapse while its own result was
  // on the way, be handed to another worker, and the finished task be answered
  // "not accepted". A heartbeat for a claim that has just completed is a no-op
  // on the server. An executor that produced no result has nothing to keep
  // alive, so that case stops at once.
  if (!result_json) {
    heartbeat_stop(&state->heartbeat);
  }
  // The caller has asked contribute_task_hit_time_limit already; a deadline
  // left running could only stop the submission itself.
  (void)contribute_deadline_finish(state->deadline);
  state->deadline = NULL;
  // An executor that failed produced no result to submit.
  bool hand_back = error_message && !result_json;

  if (error_message) {
    char *elapsed = task_elapsed_text(state);
    thread_control_print_formatted_err(
        thread_control, "task failed after %s: %s\n", elapsed, error_message);
    free(elapsed);
    state->consecutive_failures++;
    free(state->last_failure);
    state->last_failure = string_duplicate(error_message);
  }

  if (result_json) {
    char *rejection = NULL;
    const contribute_submit_outcome_t outcome = submit_result_over_http(
        state->http_client, state->claim_token, result_json, movegens,
        state->derived_skipped, &rejection, error_stack);
    heartbeat_stop(&state->heartbeat);
    if (error_stack_is_empty(error_stack)) {
      if (outcome == CONTRIBUTE_SUBMIT_ACCEPTED) {
        state->completed++;
      }
      // Whatever the server made of it, the task ran to its end; why a
      // result did not count follows on the error stream.
      char *elapsed = task_elapsed_text(state);
      char *counts =
          get_formatted_string(" in %s (%d started, %d completed)", elapsed,
                               state->started, state->completed);
      free(elapsed);
      print_task_line(thread_control, "finished", state->task_number,
                      state->job_name, counts);
      free(counts);
      switch (outcome) {
      case CONTRIBUTE_SUBMIT_ACCEPTED:
        state->consecutive_failures = 0;
        break;
      case CONTRIBUTE_SUBMIT_NOT_ACCEPTED:
        thread_control_print_formatted_err(
            thread_control, "result not accepted: the claim had already "
                            "lapsed or been submitted\n");
        break;
      case CONTRIBUTE_SUBMIT_REJECTED:
        thread_control_print_formatted_err(thread_control, "%s\n", rejection);
        state->consecutive_failures++;
        free(state->last_failure);
        state->last_failure = rejection;
        rejection = NULL;
        // A refused result leaves the claim open on the server, holding the
        // slot exactly as a failed executor would.
        hand_back = true;
        break;
      }
    }
    free(rejection);
  }

  if (hand_back) {
    decline_failed_task(state);
  }

  free(state->claim_token);
  state->claim_token = NULL;
  free(state->claimed_job_id);
  state->claimed_job_id = NULL;
  free(state->job_name);
  state->job_name = NULL;
  json_destroy(state->assignment);
  state->assignment = NULL;

  if (!error_stack_is_empty(error_stack)) {
    state->stop = true;
  }
  if (state->consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string(
            "gave up after %d consecutive task failures. Last failure: %s",
            state->consecutive_failures,
            state->last_failure ? state->last_failure : "unknown"));
    state->stop = true;
  }
  if (state->client_state->max_tasks != 0 &&
      state->completed >= state->client_state->max_tasks) {
    state->stop = true;
  }
}

bool contribute_should_stop(const ContributeState *state) {
  return state != NULL && state->stop;
}

int contribute_get_threads(const ContributeState *state) {
  return state->threads;
}

// Server-minted artifact keys look like leaves/<uuid>/generation-<n>.klv2.
// The key is untrusted and goes into a URL, so anything outside that alphabet,
// an absolute path or a parent-directory segment is refused rather than sent.
static bool contribute_is_safe_artifact_key(const char *key) {
  if (!key || *key == '\0' || *key == '/' || strstr(key, "..")) {
    return false;
  }
  for (const char *c = key; *c; c++) {
    const bool allowed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                         (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' ||
                         *c == '.' || *c == '/';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

void contribute_fetch_artifact(ContributeState *state, const char *key,
                               ChttpResponse *response, bool *not_found,
                               ErrorStack *error_stack) {
  *not_found = false;
  if (!contribute_is_safe_artifact_key(key)) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     get_formatted_string("server sent an unusable artifact "
                                          "key: '%.200s'",
                                          key ? key : "(absent)"));
    return;
  }
  char *path = get_formatted_string("/api/worker/artifact?key=%s", key);
  http_client_get(state->http_client, path, response, error_stack);
  free(path);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (response->status_code != 200) {
    // A 404 is the server saying the object is gone -- RUNBOOK §3's "missing
    // object", until an admin rebuilds it -- which the caller declines like
    // a KLV that fails its hash. Anything else is an error (a 5xx has
    // already been retried).
    *not_found = response->status_code == 404;
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string("fetching artifact '%s' failed with HTTP %ld", key,
                             response->status_code));
    chttp_response_destroy(response);
  }
}

void contribute_state_destroy(ContributeState *state) {
  if (!state) {
    return;
  }
  free(state->last_failure);
  free(state->task_data_paths);
  for (int i = 0; i < state->deferred_count; i++) {
    free(state->deferred[i].job_id);
  }
  free(state->deferred);
  (void)contribute_deadline_finish(state->deadline);
  free(state->claim_token);
  free(state->claimed_job_id);
  free(state->job_name);
  string_list_destroy(state->unsupported_jobs);
  string_list_destroy(state->data_gaps);
  string_list_destroy(state->logged_gaps);
  string_list_destroy(state->digest_cache);
  string_list_destroy(state->derived_mismatches);
  json_destroy(state->assignment);
  http_client_destroy(state->http_client);
  client_state_destroy(state->client_state);
  free(state);
}

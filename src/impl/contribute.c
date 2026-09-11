#include "contribute.h"

#include "../compat/chttp.h"
#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/cpthread_defs.h"
#include "../ent/client_state.h"
#include "../ent/data_filepaths.h"
#include "../ent/thread_control.h"
#include "../util/hash.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
  HEARTBEAT_INTERVAL_SECONDS = 30,
  // A task that fails does not count toward maxtasks, so without this a job
  // this build cannot execute would spin forever, claiming and abandoning.
  MAX_CONSECUTIVE_FAILURES = 5,
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
    // one as a lapsed claim and reassigns the task, which is the design.
    ChttpResponse response;
    http_client_post_json(heartbeat->client, "/api/worker/heartbeat", body,
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
// The claim/submit state machine
// ---------------------------------------------------------------------------

struct ContributeState {
  ClientState *client_state;
  HttpClient *http_client;
  int threads;
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
  // Set once the server has told this worker to stop.
  bool shutdown_requested;

  // Set between a successful claim and its matching submit.
  Heartbeat heartbeat;
  char *claim_token;
  char *claimed_job_id;
  JsonValue *assignment;
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

// Identity of a file's *contents*, as far as the filesystem can report it.
//
// (path, size, mtime) alone collides: a file replaced with different bytes of
// the same size inside one mtime tick keys identically to the old one, and
// archive extraction routinely sets mtimes rather than letting them fall to
// now. The inode and the ctime close that -- ctime moves on any change to the
// inode and cannot be set backwards by a program -- but only at the
// resolution they are read at: whole seconds is not enough, because the whole
// problem is two writes inside one tick. So the key carries nanoseconds where
// the filesystem records them. A cached digest must never be the reason a bad
// file passes.
#if defined(__APPLE__)
#define STAT_MTIM(info) ((info).st_mtimespec)
#define STAT_CTIM(info) ((info).st_ctimespec)
#else
#define STAT_MTIM(info) ((info).st_mtim)
#define STAT_CTIM(info) ((info).st_ctim)
#endif

char *contribute_digest_cache_key(const char *path) {
  struct stat info;
  if (stat(path, &info) != 0) {
    return NULL;
  }
  return get_formatted_string(
      "%s|%lld|%lld.%09ld|%llu|%lld.%09ld", path, (long long)info.st_size,
      (long long)STAT_MTIM(info).tv_sec, (long)STAT_MTIM(info).tv_nsec,
      (unsigned long long)info.st_ino, (long long)STAT_CTIM(info).tv_sec,
      (long)STAT_CTIM(info).tv_nsec);
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

// Checks every file the claimed task will load against the digest the job
// pins, writing the mismatches into `missing` as the JSON array the decline
// carries. Returns true when everything matches.
//
// The check runs before the heartbeat starts: hashing is milliseconds, and a
// decline should not look like a worker that started and died.
static bool expected_data_matches(ContributeState *state, const char *data_paths,
                                  ThreadControl *thread_control,
                                  StringBuilder *missing) {
  const JsonValue *expected =
      json_object_get(state->assignment, "expected_data");
  if (!expected) {
    // A job that pins nothing. Nothing to verify, nothing to decline.
    return true;
  }

  const char *algorithm = json_get_string_or_null(expected, "algorithm");
  if (algorithm && !strings_equal(algorithm, "sha256")) {
    // Run unverified rather than refusing: an algorithm this build does not
    // know is a server that moved on, and refusing work over it would turn an
    // algorithm change into a fleet-wide outage. min_magpie_version is the
    // correct lever for that.
    if (!already_logged(state, algorithm)) {
      thread_control_print_formatted(
          thread_control,
          "server verifies input data with '%s', which this MAGPIE does not "
          "know; contributing unverified\n",
          algorithm);
    }
    return true;
  }

  const JsonValue *files = json_object_get(expected, "files");
  const int count = json_array_length(files);
  bool all_matched = true;
  bool first_missing = true;
  ErrorStack *errors = error_stack_create();

  for (int i = 0; i < count; i++) {
    const JsonValue *file = json_array_get(files, i);
    const char *role = json_get_string_or_null(file, "role");
    const char *name = json_get_string_or_null(file, "name");
    const char *expected_digest = json_get_string_or_null(file, "sha256");
    const char *tarball_date = json_get_string_or_null(file, "tarball_date");
    const char *display_path = json_get_string_or_null(file, "path");
    data_filepath_t type;
    if (!role || !name || !expected_digest || !role_to_filepath_type(role, &type)) {
      continue;
    }

    char *path =
        data_filepaths_get_readable_filename(data_paths, name, type, errors);
    char *actual = NULL;
    if (error_stack_is_empty(errors)) {
      actual = hash_with_cache(state, path, errors);
    }
    error_stack_reset(errors);

    if (actual && strings_equal(actual, expected_digest)) {
      free(actual);
      free(path);
      continue;
    }

    all_matched = false;
    if (!first_missing) {
      string_builder_add_string(missing, ",");
    }
    first_missing = false;
    json_write_object_start(missing);
    bool field_first = true;
    json_write_string_field(missing, "role", role, &field_first);
    json_write_string_field(missing, "name", name, &field_first);
    json_write_string_field(missing, "expected", expected_digest, &field_first);
    if (actual) {
      json_write_string_field(missing, "actual", actual, &field_first);
    }
    json_write_object_end(missing);

    // Keyed by resolved path and expected digest: the same gap logs once, and
    // logs again only when the file or the expectation changes. The resolved
    // absolute path is printed because "your english.csv does not match" is
    // unactionable when the contributor has two of them.
    char *gap = actual ? get_formatted_string(
                             "  %-24s has sha256 %.8s, jobs require %.8s (%s)",
                             display_path ? display_path : name, actual,
                             expected_digest, path ? path : "unresolved")
                       : get_formatted_string(
                             "  %-24s not found in any data path (%s)",
                             display_path ? display_path : name, data_paths);
    char *log_key = get_formatted_string("%s|%s", path ? path : name,
                                         expected_digest);
    if (!already_logged(state, log_key)) {
      thread_control_print_formatted(thread_control, "%s\n", gap);
      if (tarball_date) {
        thread_control_print_formatted(
            thread_control,
            "  this comes from MAGPIE-DATA data-%s or later; run "
            "./download_data.sh to update\n",
            tarball_date);
      }
    }
    string_list_add_string(state->data_gaps, gap);
    free(log_key);
    free(gap);
    free(actual);
    free(path);
  }

  error_stack_destroy(errors);
  return all_matched;
}

// A worker with neither an API key nor a UUID sends no identity at all -- the
// server mints one and hands it back the first time it actually assigns a
// task. This persists that assignment for the rest of the run and to the
// settings file for every run after.
static void adopt_server_assigned_uuid(ContributeState *state,
                                       const JsonValue *assignment) {
  ClientState *client_state = state->client_state;
  if (client_state->api_key || client_state->worker_uuid) {
    return;
  }
  const char *worker_uuid = json_get_string_or_null(assignment, "worker_uuid");
  if (!worker_uuid) {
    return;
  }
  client_state_set_worker_uuid(client_state, worker_uuid);
  http_client_set_worker_uuid(state->http_client, worker_uuid);
}

static ContributeState *contribute_state_create(const char *settings_path,
                                                ThreadControl *thread_control,
                                                ErrorStack *error_stack) {
  ClientState *client_state = client_state_load(settings_path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  ContributeState *state =
      (ContributeState *)malloc_or_die(sizeof(ContributeState));
  state->client_state = client_state;
  state->threads = client_state->threads;
  if (state->threads <= 0) {
    // Leave the machine usable. Contributing is a background activity someone
    // opts into on their daily driver, and one that makes the machine
    // unresponsive is one they turn off.
    const int cores = get_num_cores();
    state->threads = cores > 1 ? cores - 1 : 1;
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
  state->shutdown_requested = false;
  memset(&state->heartbeat, 0, sizeof(state->heartbeat));
  state->claim_token = NULL;
  state->claimed_job_id = NULL;
  state->assignment = NULL;

  const char *identity_description = "a new anonymous worker";
  if (client_state->api_key) {
    identity_description = "an authenticated worker";
  } else if (client_state->worker_uuid) {
    identity_description = client_state->worker_uuid;
  }
  thread_control_print_formatted(
      thread_control, "contributing to %s as %s (%d threads)\n",
      client_state->server_url, identity_description, state->threads);
  return state;
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

// POST /api/worker/decline: hand the claim straight back rather than letting
// it lapse on the heartbeat timeout, and tell the server which files did not
// match so an admin can see what the fleet is missing.
static void decline_over_http(ContributeState *state, const char *reason,
                              const char *missing_json,
                              ErrorStack *error_stack) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "claim_token", state->claim_token, &first);
  json_write_string_field(sb, "reason", reason, &first);
  json_write_array_start(sb, "missing", &first);
  if (missing_json) {
    string_builder_add_string(sb, missing_json);
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  char *body = string_builder_dump_and_destroy(sb, NULL);

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
  free(state->claim_token);
  state->claim_token = NULL;
  free(state->claimed_job_id);
  state->claimed_job_id = NULL;
  json_destroy(state->assignment);
  state->assignment = NULL;
}

// Prints what this worker is missing, in full, once -- the client knows it
// file by file, and the server's message cannot.
static void print_shutdown(ContributeState *state,
                           ThreadControl *thread_control,
                           const JsonValue *shutdown) {
  thread_control_print_formatted(thread_control,
                                 "\nCannot contribute to any available job.\n");
  const int gaps = string_list_get_count(state->data_gaps);
  if (gaps > 0) {
    thread_control_print_formatted(thread_control,
                                   "\nMissing or outdated input data:\n");
    for (int i = 0; i < gaps; i++) {
      thread_control_print_formatted(
          thread_control, "%s\n", string_list_get_string(state->data_gaps, i));
    }
  }
  const char *message = json_get_string_or_null(shutdown, "message");
  if (message) {
    thread_control_print_formatted(thread_control, "\n%s\n", message);
  }
  const char *required_version =
      json_get_string_or_null(shutdown, "required_magpie_version");
  const char *download_url = json_get_string_or_null(shutdown, "download_url");
  if (required_version) {
    thread_control_print_formatted(
        thread_control, "Update MAGPIE to %s or newer%s%s.\n", required_version,
        download_url ? ": " : "", download_url ? download_url : "");
  }
  const JsonValue *dates = json_object_get(shutdown, "required_tarball_dates");
  const int date_count = json_array_length(dates);
  for (int i = 0; i < date_count; i++) {
    const char *date = json_array_get_string(dates, i);
    if (date) {
      thread_control_print_formatted(
          thread_control,
          "Run ./download_data.sh from your MAGPIE directory to install "
          "MAGPIE-DATA data-%s or later, then start contribute again.\n",
          date);
    }
  }
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
  // The body is required. Both fields are load-bearing: the version drives
  // the per-job floor filter, and the unsupported set is what keeps the server
  // from offering work this worker has already found it cannot do.
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "magpie_version", this_magpie_version, &first);
  json_write_array_start(sb, "unsupported_jobs", &first);
  const int unsupported_count = string_list_get_count(state->unsupported_jobs);
  for (int i = 0; i < unsupported_count; i++) {
    if (i > 0) {
      string_builder_add_string(sb, ",");
    }
    json_write_quoted(sb, string_list_get_string(state->unsupported_jobs, i));
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  char *claim_body = string_builder_dump_and_destroy(sb, NULL);

  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  http_client_post_json(state->http_client, "/api/worker/task", claim_body,
                        &response, error_stack);
  free(claim_body);
  if (!error_stack_is_empty(error_stack)) {
    return CONTRIBUTE_CLAIM_FAILED;
  }

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
    if (!error_stack_is_empty(error_stack)) {
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
    ctime_nap(state->client_state->idle_wait_seconds);
    return outcome;
  }

  // "Nothing you can do until something on your end changes" -- the opposite
  // of a 204, which is a quiet server. Exit cleanly, having said what is
  // wrong and what to do about it.
  const JsonValue *shutdown = json_object_get(state->assignment, "shutdown");
  if (shutdown) {
    print_shutdown(state, thread_control, shutdown);
    state->shutdown_requested = true;
    state->stop = true;
    json_destroy(state->assignment);
    state->assignment = NULL;
    return CONTRIBUTE_CLAIM_SHUTDOWN;
  }

  adopt_server_assigned_uuid(state, state->assignment);

  const char *claim_token =
      json_get_string(state->assignment, "claim_token", error_stack);
  const JsonValue *request = json_object_get(state->assignment, "task_request");
  const char *job_type =
      request ? json_get_string(request, "job_type", error_stack) : "";
  if (!error_stack_is_empty(error_stack)) {
    json_destroy(state->assignment);
    state->assignment = NULL;
    return CONTRIBUTE_CLAIM_FAILED;
  }

  const char *job_id = json_get_string_or_null(state->assignment, "job_id");
  state->claim_token = string_duplicate(claim_token);
  state->claimed_job_id = job_id ? string_duplicate(job_id) : NULL;

  // The server filters on version before it dispatches, so reaching this with
  // a job above this build is a server bug or a race with a floor that was
  // just raised. Either way it is one more job this worker cannot do, not a
  // reason to end the session: other jobs may be well within reach.
  const char *min_version =
      json_get_string_or_null(state->assignment, "min_magpie_version");
  if (min_version &&
      contribute_compare_versions(this_magpie_version, min_version) < 0) {
    thread_control_print_formatted(
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
  const bool data_ok =
      expected_data_matches(state, data_paths, thread_control, missing);
  char *missing_json = string_builder_dump_and_destroy(missing, NULL);
  if (!data_ok) {
    decline_over_http(state, "missing_data", missing_json, error_stack);
    free(missing_json);
    remember_unsupported(state, state->claimed_job_id);
    release_claim(state);
    return error_stack_is_empty(error_stack) ? CONTRIBUTE_CLAIM_DECLINED
                                             : CONTRIBUTE_CLAIM_FAILED;
  }
  free(missing_json);

  heartbeat_start(&state->heartbeat, state->http_client, state->claim_token);

  *out_job_type = job_type;
  *out_task_request = request;
  return CONTRIBUTE_CLAIM_GOT_TASK;
}

void contribute_decline_task(ContributeState *state,
                             ThreadControl *thread_control, const char *reason,
                             ErrorStack *error_stack) {
  if (!state || !state->claim_token) {
    return;
  }
  thread_control_print_formatted(thread_control, "declining this task: %s\n",
                                 reason);
  decline_over_http(state, reason, NULL, error_stack);
  remember_unsupported(state, state->claimed_job_id);
  release_claim(state);
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
                        const char *result_json, char **rejection,
                        ErrorStack *error_stack) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "claim_token", claim_token, &first);
  json_write_raw_key(sb, "result", &first);
  string_builder_add_string(sb, result_json);
  json_write_object_end(sb);
  char *body = string_builder_dump_and_destroy(sb, NULL);

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
    JsonValue *ack =
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

void contribute_submit_result(ContributeState *state,
                              ThreadControl *thread_control,
                              const char *result_json,
                              const char *error_message, bool fatal,
                              ErrorStack *error_stack) {
  heartbeat_stop(&state->heartbeat);

  if (error_message) {
    thread_control_print_formatted(thread_control, "task failed: %s\n",
                                   error_message);
    state->consecutive_failures++;
    free(state->last_failure);
    state->last_failure = string_duplicate(error_message);
    if (fatal) {
      error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_UNKNOWN_JOB_TYPE,
                       string_duplicate(error_message));
      state->stop = true;
    }
  }

  if (result_json) {
    char *rejection = NULL;
    const contribute_submit_outcome_t outcome =
        submit_result_over_http(state->http_client, state->claim_token,
                                result_json, &rejection, error_stack);
    if (error_stack_is_empty(error_stack)) {
      switch (outcome) {
      case CONTRIBUTE_SUBMIT_ACCEPTED:
        state->completed++;
        state->consecutive_failures = 0;
        thread_control_print_formatted(thread_control, "completed %d task(s)\n",
                                       state->completed);
        break;
      case CONTRIBUTE_SUBMIT_NOT_ACCEPTED:
        thread_control_print_formatted(
            thread_control, "result not accepted: the claim had already "
                            "lapsed or been submitted\n");
        break;
      case CONTRIBUTE_SUBMIT_REJECTED:
        thread_control_print_formatted(thread_control, "%s\n", rejection);
        state->consecutive_failures++;
        free(state->last_failure);
        state->last_failure = rejection;
        rejection = NULL;
        break;
      }
    }
    free(rejection);
  }

  free(state->claim_token);
  state->claim_token = NULL;
  free(state->claimed_job_id);
  state->claimed_job_id = NULL;
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
                               ChttpResponse *response,
                               ErrorStack *error_stack) {
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
  free(state->claim_token);
  free(state->claimed_job_id);
  string_list_destroy(state->unsupported_jobs);
  string_list_destroy(state->data_gaps);
  string_list_destroy(state->logged_gaps);
  string_list_destroy(state->digest_cache);
  json_destroy(state->assignment);
  http_client_destroy(state->http_client);
  client_state_destroy(state->client_state);
  free(state);
}

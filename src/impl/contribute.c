#include "contribute.h"

#include "../compat/chttp.h"
#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/cpthread_defs.h"
#include "../ent/client_state.h"
#include "../ent/thread_control.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include <stdlib.h>
#include <string.h>

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

  // Set between a successful claim and its matching submit.
  Heartbeat heartbeat;
  char *claim_token;
  JsonValue *assignment;
};

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
  memset(&state->heartbeat, 0, sizeof(state->heartbeat));
  state->claim_token = NULL;
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

static contribute_claim_outcome_t
claim_task_over_http(ContributeState *state, ErrorStack *error_stack) {
  ChttpResponse response;
  // state is never NULL here: contribute_claim_task only reaches this call
  // with *state_ptr set, either because it was already non-NULL or because
  // contribute_state_create just returned non-NULL for it, and
  // contribute_state_create's only failure path (client_state_load) returns
  // NULL exactly when error_stack is non-empty, which contribute_claim_task
  // already checks and returns on before this point.
  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  http_client_post_json(state->http_client, "/api/worker/task", "", &response,
                        error_stack);
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
                      const char *this_magpie_version,
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
      claim_task_over_http(state, error_stack);
  if (outcome == CONTRIBUTE_CLAIM_FAILED) {
    return outcome;
  }
  if (outcome == CONTRIBUTE_CLAIM_NO_WORK) {
    ctime_nap(state->client_state->idle_wait_seconds);
    return outcome;
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

  // The server states a minimum and the client reports clearly when it falls
  // short. Stopping rather than skipping means a GUI shows one actionable
  // message instead of a scrolling error; the claim lapses via the heartbeat
  // timeout and another worker picks it up.
  const char *min_version =
      json_get_string_or_null(state->assignment, "min_magpie_version");
  if (min_version &&
      contribute_compare_versions(this_magpie_version, min_version) < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_MAGPIE_TOO_OLD,
        get_formatted_string(
            "this job requires MAGPIE %s but this build is %s. Update MAGPIE "
            "to continue contributing.",
            min_version, this_magpie_version));
    json_destroy(state->assignment);
    state->assignment = NULL;
    return CONTRIBUTE_CLAIM_FAILED;
  }

  state->claim_token = string_duplicate(claim_token);
  heartbeat_start(&state->heartbeat, state->http_client, state->claim_token);

  *out_job_type = job_type;
  *out_task_request = request;
  return CONTRIBUTE_CLAIM_GOT_TASK;
}

static void submit_result_over_http(HttpClient *client, const char *claim_token,
                                    const char *result_json,
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
    return;
  }

  if (response.status_code != 200) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string("submitting a result failed with HTTP %ld: %.200s",
                             response.status_code,
                             response.body ? response.body : ""));
  }
  chttp_response_destroy(&response);
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
    submit_result_over_http(state->http_client, state->claim_token, result_json,
                            error_stack);
    state->completed++;
    state->consecutive_failures = 0;
    thread_control_print_formatted(thread_control, "completed %d task(s)\n",
                                   state->completed);
  }

  free(state->claim_token);
  state->claim_token = NULL;
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

void contribute_fetch_artifact(ContributeState *state, const char *key,
                               ChttpResponse *response,
                               ErrorStack *error_stack) {
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
  json_destroy(state->assignment);
  http_client_destroy(state->http_client);
  client_state_destroy(state->client_state);
  free(state);
}

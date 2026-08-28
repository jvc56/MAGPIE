#include "contribute.h"

#include "../compat/chttp.h"
#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/client_state.h"
#include "../ent/data_filepaths.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../str/move_string.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  HEARTBEAT_INTERVAL_SECONDS = 30,
  // A task that fails does not count toward maxtasks, so without this a job
  // this build cannot execute would spin forever, claiming and abandoning.
  MAX_CONSECUTIVE_FAILURES = 5,
  MAX_FORCED_RACKS = 100000,
  MAX_BATCH_GAMES = 1000000,
};

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------

// Compares dotted numeric versions. Returns <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" compare equal.
int contribute_compare_versions(const char *left, const char *right) {
  while (*left != '\0' || *right != '\0') {
    char *left_end = NULL;
    char *right_end = NULL;
    const long left_part = strtol(left, &left_end, 10);
    const long right_part = strtol(right, &right_end, 10);
    if (left_part != right_part) {
      return left_part < right_part ? -1 : 1;
    }
    left = left_end;
    right = right_end;
    if (*left == '.') {
      left++;
    }
    if (*right == '.') {
      right++;
    }
  }
  return 0;
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
  while (true) {
    // Poll the stop flag on a short interval so stopping is responsive even
    // though the heartbeat itself is infrequent.
    for (int i = 0; i < HEARTBEAT_INTERVAL_SECONDS; i++) {
      ctime_nap(1.0);
      cpthread_mutex_lock(&heartbeat->mutex);
      const bool stop = heartbeat->stop;
      cpthread_mutex_unlock(&heartbeat->mutex);
      if (stop) {
        return NULL;
      }
    }

    StringBuilder *sb = string_builder_create();
    bool first = true;
    json_write_object_start(sb);
    json_write_string_field(sb, "claim_token", heartbeat->claim_token, &first);
    json_write_object_end(sb);
    char *body = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);

    // A failed heartbeat is not actionable here: the server treats a missed
    // one as a lapsed claim and reassigns the task, which is the design.
    ErrorStack *errors = error_stack_create();
    ChttpResponse response;
    http_client_post_json(heartbeat->client, "/api/worker/heartbeat", body,
                          &response, errors);
    if (error_stack_is_empty(errors)) {
      chttp_response_destroy(&response);
    }
    error_stack_reset(errors);
    error_stack_destroy(errors);
    free(body);
  }
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
// Player settings
// ---------------------------------------------------------------------------

// Appends the per-player arguments for one player of a task request. `slot` is
// 1 or 2. Simulation settings are absent together for a static player and are
// omitted rather than passed as zero.
static void append_player_args(StringBuilder *sb, const JsonValue *player,
                               int slot) {
  const char *recorder = json_get_string_or_null(player, "recorder_type");
  if (recorder) {
    string_builder_add_formatted_string(sb, " -r%d %s", slot, recorder);
  }
  const char *sort = json_get_string_or_null(player, "sort_strategy");
  if (sort) {
    string_builder_add_formatted_string(sb, " -s%d %s", slot, sort);
  }
  const char *leaves = json_get_string_or_null(player, "leaves");
  if (leaves) {
    string_builder_add_formatted_string(sb, " -k%d %s", slot, leaves);
  }

  const JsonValue *iterations = json_object_get(player, "max_iterations");
  if (iterations && !json_is_null(iterations)) {
    string_builder_add_formatted_string(
        sb, " -i%d %d", slot, json_get_int_or(player, "max_iterations", 0));
  }
  const JsonValue *plies = json_object_get(player, "plies");
  if (plies && !json_is_null(plies)) {
    string_builder_add_formatted_string(sb, " -pl%d %d", slot,
                                        json_get_int_or(player, "plies", 0));
  }
  const JsonValue *top_plays = json_object_get(player, "top_plays");
  if (top_plays && !json_is_null(top_plays)) {
    string_builder_add_formatted_string(
        sb, " -np%d %d", slot, json_get_int_or(player, "top_plays", 0));
  }
  const JsonValue *stopping = json_object_get(player, "stopping_pct");
  if (stopping && !json_is_null(stopping)) {
    string_builder_add_formatted_string(
        sb, " -sc%d %.6f", slot,
        json_get_double_or(player, "stopping_pct", 0.0));
  }
  const JsonValue *inference = json_object_get(player, "use_inference");
  if (inference && !json_is_null(inference)) {
    string_builder_add_formatted_string(
        sb, " -si%d %s", slot,
        json_get_bool_or(player, "use_inference", false) ? "true" : "false");
  }
  const JsonValue *time_limit = json_object_get(player, "time_limit_secs");
  if (time_limit && !json_is_null(time_limit)) {
    string_builder_add_formatted_string(
        sb, " -tl%d %.6f", slot,
        json_get_double_or(player, "time_limit_secs", 0.0));
  }
}

// ---------------------------------------------------------------------------
// Input validation
// ---------------------------------------------------------------------------

// Every field of a task request is untrusted: it becomes file paths and
// allocation sizes. Lexicon and variant reach data_filepaths, so they must not
// be able to escape the data directory.
static bool is_safe_data_name(const char *name) {
  if (!name || *name == '\0') {
    return false;
  }
  for (const char *c = name; *c; c++) {
    const bool allowed = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                         (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

static bool validate_common(const JsonValue *request, const char **lexicon,
                            const char **variant, ErrorStack *error_stack) {
  *lexicon = json_get_string(request, "lexicon", error_stack);
  *variant = json_get_string(request, "variant", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  if (!is_safe_data_name(*lexicon) || !is_safe_data_name(*variant)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string(
            "server sent an unusable lexicon or variant name: '%s' / '%s'",
            *lexicon, *variant));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Wordmap provisioning
// ---------------------------------------------------------------------------

// Wordmaps make game play dramatically faster, so a contributing client always
// uses one. They are never transmitted -- roughly ten times the size of
// everything else MAGPIE ships -- so the client derives them from the .kwg it
// already has. The whole chain costs about a second per lexicon, once.
static void ensure_wordmap(const ContributeConfigApi *config_api,
                           const char *lexicon, ErrorStack *error_stack) {
  const char *data_paths = config_api->get_data_paths(config_api->config);

  char *wmp_path = data_filepaths_get_readable_filename(
      data_paths, lexicon, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  if (error_stack_is_empty(error_stack)) {
    free(wmp_path);
    return;
  }
  // Absent, which is the normal first-run case rather than a failure.
  error_stack_reset(error_stack);

  char *txt_path = data_filepaths_get_readable_filename(
      data_paths, lexicon, DATA_FILEPATH_TYPE_LEXICON, error_stack);
  if (error_stack_is_empty(error_stack)) {
    free(txt_path);
  } else {
    error_stack_reset(error_stack);
    char *command = get_formatted_string("convert dawg2text %s", lexicon);
    config_api->load_command(config_api->config, command, error_stack);
    free(command);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    config_api->execute_command(config_api->config, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  char *command = get_formatted_string("convert text2wordmap %s", lexicon);
  config_api->load_command(config_api->config, command, error_stack);
  free(command);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  config_api->execute_command(config_api->config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // `convert` reports failures on the error stack but can still leave no file
  // behind, so the output's existence is the real check.
  wmp_path = data_filepaths_get_readable_filename(
      data_paths, lexicon, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_reset(error_stack);
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_DATA_NOT_WRITABLE,
        get_formatted_string(
            "could not build a wordmap for %s. The data directory must be "
            "writable; contributing requires a wordmap.",
            lexicon));
    return;
  }
  free(wmp_path);
}

// ---------------------------------------------------------------------------
// Task executors
// ---------------------------------------------------------------------------

// Each executor drives MAGPIE through the same in-process command API the
// tests use, then reads results out of the result structs. Nothing is
// serialized to stdout and parsed back.

static void write_game_summary(StringBuilder *sb, const char *key,
                               const AutoplayGameSummary *summary,
                               bool *outer_first) {
  json_write_raw_key(sb, key, outer_first);
  bool first = true;
  json_write_object_start(sb);
  json_write_int_field(sb, "games", (int64_t)summary->games, &first);
  json_write_int_field(sb, "wins", (int64_t)summary->p0_wins, &first);
  json_write_int_field(sb, "losses", (int64_t)summary->p0_losses, &first);
  json_write_int_field(sb, "ties", (int64_t)summary->p0_ties, &first);
  json_write_double_field(sb, "p1_score_mean", summary->p0_score_mean, &first);
  json_write_double_field(sb, "p1_score_sd", summary->p0_score_stdev, &first);
  json_write_double_field(sb, "p2_score_mean", summary->p1_score_mean, &first);
  json_write_double_field(sb, "p2_score_sd", summary->p1_score_stdev, &first);
  json_write_object_end(sb);
}

static char *execute_games(const ContributeConfigApi *config_api,
                           const JsonValue *request, bool game_pairs,
                           int threads, ErrorStack *error_stack) {
  const char *lexicon = NULL;
  const char *variant = NULL;
  if (!validate_common(request, &lexicon, &variant, error_stack)) {
    return NULL;
  }

  const uint64_t seed = json_get_uint64_string(request, "seed", error_stack);
  const int64_t num_games = json_get_int(request, "num_games", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  if (num_games <= 0 || num_games > MAX_BATCH_GAMES) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     get_formatted_string(
                         "server asked for an unreasonable batch size: %lld",
                         (long long)num_games));
    return NULL;
  }

  ensure_wordmap(config_api, lexicon, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  StringBuilder *command = string_builder_create();
  string_builder_add_formatted_string(
      command,
      "autoplay games %lld -lex %s -var %s -seed %llu -threads %d -hr false "
      "-printonfinish false",
      (long long)num_games, lexicon, variant, (unsigned long long)seed,
      threads);
  if (game_pairs) {
    string_builder_add_string(command, " -gp true");
  } else {
    string_builder_add_string(command, " -gp false");
  }
  append_player_args(command, json_object_get(request, "player1"), 1);
  append_player_args(command, json_object_get(request, "player2"), 2);

  char *command_string = string_builder_dump(command, NULL);
  string_builder_destroy(command);

  config_api->load_command(config_api->config, command_string, error_stack);
  free(command_string);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  config_api->execute_command(config_api->config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  const AutoplayResults *results =
      config_api->get_autoplay_results(config_api->config);
  AutoplayGameSummary all_games;
  if (!autoplay_results_get_game_summary(results, false, &all_games)) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     string_duplicate("autoplay produced no game results"));
    return NULL;
  }

  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  write_game_summary(sb, "all_games", &all_games, &first);
  if (game_pairs) {
    // The divergent subset is where a paired run's signal lives: pairs whose
    // two games played identically are guaranteed ties carrying no
    // information, and the server computes the LLR from this.
    AutoplayGameSummary divergent;
    if (autoplay_results_get_game_summary(results, true, &divergent)) {
      write_game_summary(sb, "divergent_games", &divergent, &first);
    }
  }
  json_write_object_end(sb);
  return string_builder_dump(sb, NULL);
}

static char *execute_opening_rack(const ContributeConfigApi *config_api,
                                  const JsonValue *request, int threads,
                                  ErrorStack *error_stack) {
  const char *lexicon = NULL;
  const char *variant = NULL;
  if (!validate_common(request, &lexicon, &variant, error_stack)) {
    return NULL;
  }
  const char *position = json_get_string(request, "position", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  ensure_wordmap(config_api, lexicon, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  const JsonValue *player = json_object_get(request, "player");
  const JsonValue *iterations = json_object_get(player, "max_iterations");
  const bool simming = iterations && !json_is_null(iterations);

  StringBuilder *command = string_builder_create();
  string_builder_add_formatted_string(
      command, "cgp %s -lex %s -var %s -threads %d -hr false", position,
      lexicon, variant, threads);
  append_player_args(command, player, 1);
  char *load_command = string_builder_dump(command, NULL);
  string_builder_destroy(command);

  config_api->load_command(config_api->config, load_command, error_stack);
  free(load_command);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  config_api->execute_command(config_api->config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  config_api->load_command(config_api->config,
                           simming ? "simulate" : "generate", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  config_api->execute_command(config_api->config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  const MoveList *moves = config_api->get_move_list(config_api->config);
  const Game *game = config_api->get_game(config_api->config);
  const LetterDistribution *ld = config_api->get_ld(config_api->config);
  const int count = move_list_get_count(moves);
  if (count == 0) {
    error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
                     string_duplicate("move generation produced no moves"));
    return NULL;
  }

  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_array_start(sb, "moves", &first);
  for (int i = 0; i < count; i++) {
    const Move *move = move_list_get_move(moves, i);
    if (i > 0) {
      string_builder_add_string(sb, ",");
    }
    bool move_first = true;
    json_write_object_start(sb);

    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, game_get_board(game), move, ld, false);
    char *move_string = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    json_write_string_field(sb, "move", move_string, &move_first);
    free(move_string);

    json_write_int_field(sb, "score", equity_to_int(move_get_score(move)),
                         &move_first);
    json_write_double_field(
        sb, "equity", equity_to_double(move_get_equity(move)), &move_first);
    json_write_object_end(sb);
  }
  json_write_array_end(sb);
  json_write_object_end(sb);
  return string_builder_dump(sb, NULL);
}

static char *execute_leave_gen(const ContributeConfigApi *config_api,
                               const JsonValue *request, int threads,
                               ErrorStack *error_stack) {
  (void)config_api;
  (void)threads;
  const char *lexicon = NULL;
  const char *variant = NULL;
  if (!validate_common(request, &lexicon, &variant, error_stack)) {
    return NULL;
  }
  (void)lexicon;
  (void)variant;
  // Leave generation needs the previous generation's KLV fetched over HTTP and
  // the rack-equity table read back out of RackList. Neither is wired up yet.
  error_stack_push(
      error_stack, ERROR_STATUS_CONTRIBUTE_UNKNOWN_JOB_TYPE,
      string_duplicate(
          "leave_generation tasks are not implemented in this build yet"));
  return NULL;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

typedef enum {
  CLAIM_GOT_TASK,
  CLAIM_NO_WORK,
  CLAIM_FAILED,
} claim_outcome_t;

static claim_outcome_t claim_task(HttpClient *client, JsonValue **assignment,
                                  ErrorStack *error_stack) {
  ChttpResponse response;
  http_client_post_json(client, "/api/worker/task", "", &response, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return CLAIM_FAILED;
  }

  // 204 is the normal state of a quiet server, not a failure.
  if (response.status_code == 204) {
    chttp_response_destroy(&response);
    return CLAIM_NO_WORK;
  }
  if (response.status_code != 200) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
        get_formatted_string("claiming a task failed with HTTP %ld: %.200s",
                             response.status_code,
                             response.body ? response.body : ""));
    chttp_response_destroy(&response);
    return CLAIM_FAILED;
  }

  *assignment = json_parse(response.body, error_stack);
  chttp_response_destroy(&response);
  return error_stack_is_empty(error_stack) ? CLAIM_GOT_TASK : CLAIM_FAILED;
}

static void submit_result(HttpClient *client, const char *claim_token,
                          const char *result_json, ErrorStack *error_stack) {
  StringBuilder *sb = string_builder_create();
  bool first = true;
  json_write_object_start(sb);
  json_write_string_field(sb, "claim_token", claim_token, &first);
  json_write_raw_key(sb, "result", &first);
  string_builder_add_string(sb, result_json);
  json_write_object_end(sb);
  char *body = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);

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

// A worker with neither an API key nor a UUID sends no identity at all --
// the server mints one and hands it back the first time it actually assigns a
// task (see claim_task). This persists that assignment for the rest of the
// run and to the settings file for every run after.
static void adopt_server_assigned_uuid(ClientState *state, HttpClient *client,
                                       const JsonValue *assignment) {
  if (state->api_key || state->worker_uuid) {
    return;
  }
  const char *worker_uuid = json_get_string_or_null(assignment, "worker_uuid");
  if (!worker_uuid) {
    return;
  }
  client_state_set_worker_uuid(state, worker_uuid);
  http_client_set_worker_uuid(client, worker_uuid);
}

void contribute_run(const ContributeConfigApi *config_api,
                    const char *settings_path, ErrorStack *error_stack) {
  ClientState *state = client_state_load(settings_path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  int threads = state->threads;
  if (threads <= 0) {
    // Leave the machine usable. Contributing is a background activity someone
    // opts into on their daily driver, and one that makes the machine
    // unresponsive is one they turn off.
    const int cores = config_api->get_num_threads(config_api->config);
    threads = cores > 1 ? cores - 1 : 1;
  }

  HttpClient *client =
      http_client_create(state->server_url, state->api_key, state->worker_uuid);

  ThreadControl *thread_control =
      config_api->get_thread_control(config_api->config);
  const char *identity_description = "a new anonymous worker";
  if (state->api_key) {
    identity_description = "an authenticated worker";
  } else if (state->worker_uuid) {
    identity_description = state->worker_uuid;
  }
  thread_control_print_formatted(
      thread_control, "contributing to %s as %s (%d threads)\n",
      state->server_url, identity_description, threads);

  int completed = 0;
  int consecutive_failures = 0;
  char *last_failure = NULL;
  while (state->max_tasks == 0 || completed < state->max_tasks) {
    JsonValue *assignment = NULL;
    const claim_outcome_t outcome =
        claim_task(client, &assignment, error_stack);
    if (outcome == CLAIM_FAILED) {
      break;
    }
    if (outcome == CLAIM_NO_WORK) {
      ctime_nap(state->idle_wait_seconds);
      continue;
    }

    adopt_server_assigned_uuid(state, client, assignment);

    const char *claim_token =
        json_get_string(assignment, "claim_token", error_stack);
    const JsonValue *request = json_object_get(assignment, "task_request");
    const char *job_type =
        request ? json_get_string(request, "job_type", error_stack) : "";
    if (!error_stack_is_empty(error_stack)) {
      json_destroy(assignment);
      break;
    }

    // The server states a minimum and the client reports clearly when it falls
    // short. Stopping rather than skipping means a GUI shows one actionable
    // message instead of a scrolling error; the claim lapses via the heartbeat
    // timeout and another worker picks it up.
    const char *min_version =
        json_get_string_or_null(assignment, "min_magpie_version");
    if (min_version && contribute_compare_versions(
                           config_api->get_magpie_version(), min_version) < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONTRIBUTE_MAGPIE_TOO_OLD,
          get_formatted_string(
              "this job requires MAGPIE %s but this build is %s. Update MAGPIE "
              "to continue contributing.",
              min_version, config_api->get_magpie_version()));
      json_destroy(assignment);
      break;
    }

    char *claim_token_copy = string_duplicate(claim_token);
    Heartbeat heartbeat;
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat_start(&heartbeat, client, claim_token_copy);

    // Execution failures are reported and the claim abandoned; the loop
    // continues, since another task may well be fine.
    ErrorStack *task_errors = error_stack_create();
    char *result_json = NULL;
    if (strings_equal(job_type, "games")) {
      result_json =
          execute_games(config_api, request, false, threads, task_errors);
    } else if (strings_equal(job_type, "game_pairs")) {
      result_json =
          execute_games(config_api, request, true, threads, task_errors);
    } else if (strings_equal(job_type, "opening_rack_analysis")) {
      result_json =
          execute_opening_rack(config_api, request, threads, task_errors);
    } else if (strings_equal(job_type, "leave_generation")) {
      result_json =
          execute_leave_gen(config_api, request, threads, task_errors);
    } else {
      // A job type this build does not recognise means the server is newer
      // than this MAGPIE, which is the same situation as a version mismatch.
      error_stack_push(
          task_errors, ERROR_STATUS_CONTRIBUTE_UNKNOWN_JOB_TYPE,
          get_formatted_string(
              "this MAGPIE does not know the job type '%s'. Update MAGPIE to "
              "continue contributing.",
              job_type));
    }

    heartbeat_stop(&heartbeat);

    const bool fatal = error_stack_top(task_errors) ==
                       ERROR_STATUS_CONTRIBUTE_UNKNOWN_JOB_TYPE;
    if (!error_stack_is_empty(task_errors)) {
      char *message = error_stack_get_string_and_reset(task_errors);
      thread_control_print_formatted(thread_control, "task failed: %s\n",
                                     message);
      consecutive_failures++;
      free(last_failure);
      last_failure = message;
      if (fatal) {
        error_stack_push(error_stack, ERROR_STATUS_CONTRIBUTE_UNKNOWN_JOB_TYPE,
                         string_duplicate(last_failure));
      }
    }
    error_stack_destroy(task_errors);

    if (result_json) {
      submit_result(client, claim_token_copy, result_json, error_stack);
      free(result_json);
      completed++;
      consecutive_failures = 0;
      thread_control_print_formatted(thread_control, "completed %d task(s)\n",
                                     completed);
    }

    free(claim_token_copy);
    json_destroy(assignment);

    if (fatal || !error_stack_is_empty(error_stack)) {
      break;
    }
    if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONTRIBUTE_SERVER_ERROR,
          get_formatted_string(
              "gave up after %d consecutive task failures. Last failure: %s",
              consecutive_failures, last_failure ? last_failure : "unknown"));
      break;
    }
  }

  free(last_failure);
  http_client_destroy(client);
  client_state_destroy(state);
}

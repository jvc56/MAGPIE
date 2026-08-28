#ifndef CONTRIBUTE_H
#define CONTRIBUTE_H

// The birdtest contribution protocol: claim a task, execute it, submit the
// result, repeat. This module owns only the HTTP/JSON/settings-file side of
// that loop -- claiming, heartbeating, submitting, and the anonymous-UUID
// handshake. It knows nothing about Config or how a task is actually
// executed: config.c owns the loop (impl_contribute) and, between a claim and
// its matching submit, does whatever running the task requires using its own
// direct methods (config_autoplay, game_load_cgp, ...). This keeps the two
// modules from ever needing each other's types, the same way get_gcg.c knows
// nothing about Config and config.c's impl_load_gcg does the translating.

#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include <stdbool.h>

typedef struct ContributeState ContributeState;

typedef enum {
  CONTRIBUTE_CLAIM_GOT_TASK,
  CONTRIBUTE_CLAIM_NO_WORK,
  // error_stack has the reason (a request failure, a bad HTTP status, or this
  // build being too old for the claimed job); the caller should stop.
  CONTRIBUTE_CLAIM_FAILED,
} contribute_claim_outcome_t;

// One iteration of the claim half of the contribute loop. On the very first
// call, pass *state == NULL: this loads ClientState from settings_path and
// builds the HttpClient, handing the new state back through *state. Pass the
// same *state into every later call this run, and free it with
// contribute_state_destroy once contribute_should_stop(state) is true.
//
// this_magpie_version is this build's own version string, compared against
// the claimed job's minimum ("MAGPIE too old" is reported as
// CONTRIBUTE_CLAIM_FAILED, same as any other claim failure).
//
// On CONTRIBUTE_CLAIM_GOT_TASK: *out_job_type and *out_task_request are
// borrowed views, valid only until the matching contribute_submit_result
// call, and a heartbeat thread has already been started for the claimed
// task -- the caller must call contribute_submit_result when done executing
// it, win or lose, to stop that heartbeat. On CONTRIBUTE_CLAIM_NO_WORK, this
// call has already slept idlewait seconds before returning.
// `thread_control` is used only for the "contributing to..."/"completed N
// task(s)"/"task failed: ..." status prints (on the first call and every
// later call's contribute_submit_result); it is borrowed, never owned or
// destroyed by this module.
contribute_claim_outcome_t
contribute_claim_task(ContributeState **state, const char *settings_path,
                      const char *this_magpie_version,
                      ThreadControl *thread_control, const char **out_job_type,
                      const JsonValue **out_task_request,
                      ErrorStack *error_stack);

// Submits the result for the task claimed by the last contribute_claim_task
// call and stops its heartbeat. Exactly one of result_json/error_message
// should be non-NULL: result_json on success, error_message (printed for the
// contributor, not sent to the server) on failure -- a failed task is never
// submitted, so its claim lapses via the heartbeat timeout and another
// worker picks it up. `fatal` means the caller has decided (e.g. an
// unrecognized job_type) that this run should stop after reporting; too many
// consecutive failures or reaching max_tasks stop it the same way.
void contribute_submit_result(ContributeState *state,
                              ThreadControl *thread_control,
                              const char *result_json,
                              const char *error_message, bool fatal,
                              ErrorStack *error_stack);

// True once the loop in config.c should stop.
bool contribute_should_stop(const ContributeState *state);

// Threads a claimed task should use: the settings file's explicit "threads",
// or (cores - 1) to leave the machine usable if it doesn't set one.
int contribute_get_threads(const ContributeState *state);

void contribute_state_destroy(ContributeState *state);

// Compares dotted numeric versions, returning <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" are equal. Exposed for testing: naive
// string comparison gets this wrong ("1.10" sorts below "1.9").
int contribute_compare_versions(const char *left, const char *right);

#endif

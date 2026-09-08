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

#include "../compat/chttp.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include <stdbool.h>

typedef struct ContributeState ContributeState;

// Identity of a file's contents as far as the filesystem can report it: path,
// size, mtime, inode and ctime. Digests are cached under this key so a 15 MB
// lexicon is hashed once per run rather than once per task.
//
// Size and mtime alone are not enough -- a file replaced with different bytes
// of the same size inside one mtime tick keys identically, and extracting an
// archive routinely sets mtimes rather than letting them fall to now. Exposed
// for tests, which is the only way that collision gets checked.
//
// Returns NULL if the file cannot be stat'ed; the caller frees.
char *contribute_digest_cache_key(const char *path);

typedef enum {
  CONTRIBUTE_CLAIM_GOT_TASK,
  CONTRIBUTE_CLAIM_NO_WORK,
  // A task was claimed and handed straight back: its data does not match what
  // the job pins, or it needs a newer MAGPIE. The job is remembered as
  // unsupported and not offered again this run, so the caller should simply
  // claim again -- this is an ordinary outcome, not a failure.
  CONTRIBUTE_CLAIM_DECLINED,
  // The server says nothing it has is doable until this worker changes
  // something -- its data, its MAGPIE, or both. The reason and the
  // accumulated gaps have already been printed; the caller should stop, and
  // contribute_should_stop is true from here on.
  CONTRIBUTE_CLAIM_SHUTDOWN,
  // error_stack has the reason (a request failure or a bad HTTP status); the
  // caller should stop.
  CONTRIBUTE_CLAIM_FAILED,
} contribute_claim_outcome_t;

// One iteration of the claim half of the contribute loop. On the very first
// call, pass *state == NULL: this loads ClientState from settings_path and
// builds the HttpClient, handing the new state back through *state. Pass the
// same *state into every later call this run, and free it with
// contribute_state_destroy once contribute_should_stop(state) is true.
//
// this_magpie_version is this build's own version string. It is sent with
// every claim so the server can filter jobs this build cannot run, and
// checked again against the claimed job's minimum as a cross-check -- a job
// above this build's version is declined, not fatal, because other jobs may
// still be within reach.
//
// data_paths is the client's own data search list. Every file the claimed
// task will load is resolved through it and hashed, and a task whose files do
// not match the digests the job pins is declined rather than run: results
// computed from different bytes are worse than no results, because nothing
// downstream would notice.
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
                      const char *this_magpie_version, const char *data_paths,
                      ThreadControl *thread_control, const char **out_job_type,
                      const JsonValue **out_task_request,
                      ErrorStack *error_stack);

// Hands a claimed task back without running it, for a reason the caller
// discovered after the claim -- currently only a job type this build does not
// know. Stops the heartbeat, releases the claim immediately rather than
// letting it lapse on the timeout, and remembers the job as unsupported so it
// is not claimed again this run.
//
// An unrecognised job type is not fatal: a client that predates the
// leave_generation executor can still play games all day. Exit is reserved
// for the case where nothing at all is doable, which the server detects and
// reports as a shutdown.
void contribute_decline_task(ContributeState *state,
                             ThreadControl *thread_control, const char *reason,
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

// GET /api/worker/artifact?key=<key> -- the previous leave-generation KLV, or
// any other server-minted artifact. `response` must be destroyed by the
// caller with chttp_response_destroy on success; the body may be binary
// (KLV), so it is length-delimited rather than NUL-terminated text.
void contribute_fetch_artifact(ContributeState *state, const char *key,
                               ChttpResponse *response,
                               ErrorStack *error_stack);

void contribute_state_destroy(ContributeState *state);

// Compares dotted numeric versions, returning <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" are equal. Exposed for testing: naive
// string comparison gets this wrong ("1.10" sorts below "1.9").
int contribute_compare_versions(const char *left, const char *right);

#endif

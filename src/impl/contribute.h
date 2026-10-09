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
#include <stdint.h>

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
  // something -- its data, its MAGPIE (version or build), or both. The reason
  // and the accumulated gaps have already been printed; the caller should
  // stop, and contribute_should_stop is true from here on.
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
// call, a heartbeat thread has already been started for the claimed task,
// "[HH:MM:SS] started task #<n>: <job name>" has been printed, and the
// task's clock is running (see contribute_task_hit_time_limit). The caller
// must call contribute_task_hit_time_limit when the executor returns, then
// hand the task back or call contribute_submit_result, win or lose, to stop
// that heartbeat. On CONTRIBUTE_CLAIM_NO_WORK, this call has already slept
// idlewait seconds before returning.
//
// A run prints little: the started line here, a finished line per task
// (contribute_submit_result), and, on the error stream, only what ends the
// run or needs the contributor to act -- a shutdown and what to fix, input
// data this worker lacks, a failed task. `thread_control` carries those
// prints and the stop request; it is borrowed, never owned or destroyed by
// this module.
contribute_claim_outcome_t
contribute_claim_task(ContributeState **state, const char *settings_path,
                      const char *this_magpie_version, const char *data_paths,
                      ThreadControl *thread_control, const char **out_job_type,
                      const JsonValue **out_task_request,
                      ErrorStack *error_stack);

// Hands a claimed task back without running it, for a reason the caller
// discovered after the claim -- a job type this build does not know, or a
// stop request that cut the task short. Stops the heartbeat, releases the claim
// immediately rather than letting it lapse on the timeout, and remembers the
// job as unsupported so it is not claimed again this run.
//
// An unrecognised job type is not fatal: a client that predates the
// leave_generation executor can still play games all day. Exit is reserved
// for the case where nothing at all is doable, which the server detects and
// reports as a shutdown.
void contribute_decline_task(ContributeState *state,
                             ThreadControl *thread_control, const char *reason,
                             ErrorStack *error_stack);

// ---------------------------------------------------------------------------
// The task's time limit
// ---------------------------------------------------------------------------

// Every assignment states the longest its task may run, `max_task_seconds`,
// and the server lapses a claim held much past it, heartbeat or not. So the
// worker stops a task at that point itself and hands it back: the result
// would be refused anyway, and the time is better spent on another task.

// A clock for one task: a thread that, `seconds` from now, stops the task the
// way the user's `stop` does -- setting `thread_control` to
// THREAD_CONTROL_STATUS_USER_INTERRUPT, which every executor ends early on --
// unless the user has stopped it first. NULL (no clock) for `seconds` <= 0.
// Exposed for tests.
typedef struct ContributeDeadline ContributeDeadline;
ContributeDeadline *contribute_deadline_start(ThreadControl *thread_control,
                                              double seconds);

// Stops and frees the clock; NULL is a no-op returning false. True when it
// was the clock that stopped the task, in which case the thread control's
// status is put back as it was, so the run goes on; a stop that was the
// user's is left in place. Exposed for tests.
bool contribute_deadline_finish(ContributeDeadline *deadline);

// To be called as soon as the executor returns: stops the claimed task's
// clock, and says whether it was the clock that stopped the task. If so, the
// task is not a result -- hand it back with
// contribute_hand_back_timed_out_task -- and the run goes on.
bool contribute_task_hit_time_limit(ContributeState *state);

// Hands a task the time limit stopped back to the server with reason
// "time_limit", saying so in one line, "[HH:MM:SS] stopped task #<n>: <job
// name> at its <N>-second limit, handed back (...)". The job is not set aside:
// the server, which sees every worker's time-limit declines, decides that.
void contribute_hand_back_timed_out_task(ContributeState *state,
                                         ThreadControl *thread_control);

// Reads the assignment's job_name and max_task_seconds, both required, the
// limit a whole number of seconds from 1. The name comes back as it is printed
// -- control characters replaced, cut to a sane length -- for the caller to
// free. Exposed for tests.
void contribute_read_job_name_and_limit(const JsonValue *assignment,
                                        char **job_name, int *max_task_seconds,
                                        ErrorStack *error_stack);

// ---------------------------------------------------------------------------
// Derived files
// ---------------------------------------------------------------------------

// A wordmap, rack info table or word info table is derived on the
// contributor's own machine from files the job pins, and is far too large to
// ship -- 179 MB and 1.9 GB for CSW24's wordmap and rack info table -- so
// birdtest cannot send one. What it sends instead is the SHA-256 its own
// pinned MAGPIE got when it built the same file from the same inputs, under
// `expected_data.derived`. The worker builds its own and uses it only if the
// bytes agree; a derived file the claim pins no hash for is not loaded at all.
//
// That is a stronger check than recording what a file was built *from*: a
// CSW24 wordmap built in December 2025 and one built nine months later differ
// in 72 million bytes with the same inputs and the same format version,
// because the builder changed underneath them. Comparing the output catches
// that; comparing the inputs does not.
typedef struct ContributeDerived {
  // "wmp", "rit" or "wit".
  const char *role;
  // The name the file is loaded under. A wordmap's is its lexicon's, but a
  // rack info table belongs to a (.kwg, .klv2) pair, so the server names it
  // explicitly -- "CSW24.CSW_quackle_leaves" -- and two jobs on CSW24 with
  // different leaves get different tables instead of silently sharing one.
  const char *name;
  const char *sha256;
  // The builder that produced that hash, e.g. "wmp-1". A build of MAGPIE with
  // a different builder version cannot match it and says so; see
  // src/def/builder_defs.h.
  const char *builder;
  // The instruction-set target the server's MAGPIE was built for. Recorded and
  // reported, never used to refuse work: measurement says these builders'
  // output does not depend on it, and a contributor who builds from source
  // should not be locked out on the strength of a field.
  const char *build_target;
} ContributeDerived;

// The derived file the current claim pins for (role, name), if any.
//
// False means the server pinned nothing for it. birdtest pins every derived
// file a job's players ask for, so the caller refuses the task rather than
// load a file nothing checked.
bool contribute_find_derived(const ContributeState *state, const char *role,
                             const char *name, ContributeDerived *out);

// Holds the build of the derived file at `output_path` for this process, so
// contributors that share a data directory -- several workers on one machine,
// all reading one MAGPIE's data/ -- build a file once between them instead of
// all at once: a rack info table is minutes of every core and about 2.4 GB of
// memory, and four simultaneous builds of the same table took a 16 GB machine
// into swap. A worker that finds the lock held says so and waits; the caller
// then checks the file again before building, since the holder has usually
// just built it.
//
// The lock is an flock on `<output_path>.lock`, so a process that dies holding
// it releases it. Returns the descriptor to pass to contribute_unlock_build, or
// -1 holding nothing: when the lock file cannot be opened (a data directory
// this worker cannot write, whose build then fails with its own message), and
// on a stop request while waiting, which pushes
// ERROR_STATUS_CONTRIBUTE_INTERRUPTED.
int contribute_lock_build(const char *output_path, const char *what,
                          ThreadControl *thread_control,
                          ErrorStack *error_stack);

// Releases what contribute_lock_build returned; -1 is a no-op.
void contribute_unlock_build(int lock_fd);

// The SHA-256 of `path`, from the run's digest cache when the file has not
// changed. Hashing a 1.9 GB rack info table takes about nine seconds, so the
// cache is what keeps that a once-per-file cost rather than a once-per-task
// one. Returns NULL and pushes an error if the file cannot be read; the caller
// frees.
char *contribute_hash_file(ContributeState *state, const char *path,
                           ErrorStack *error_stack);

// One entry of a decline's "missing" array, as a JSON object: a file whose
// bytes are not the ones the claim pins, with the digest it pins and the one
// found -- `actual` NULL for a file not found at all, which leaves the key out
// (birdtest reads its absence as null). The caller frees. Exposed for tests.
char *contribute_missing_file_json(const char *role, const char *name,
                                   const char *expected, const char *actual);

// The body of POST /api/worker/decline: the claim handed back, why, and the
// files behind it -- `missing_json` is the "missing" array's members, comma
// separated (contribute_missing_file_json each), or NULL for none. The caller
// frees. Exposed for tests.
char *contribute_decline_body(const char *claim_token, const char *reason,
                              const char *missing_json);

// Records a derived file whose bytes do not match what the claim pins, to be
// sent with the decline. Both digests go to the server so that a fleet-wide
// disagreement shows up in the admin view instead of being worked around
// silently by every worker independently.
void contribute_record_derived_mismatch(ContributeState *state,
                                        const char *role, const char *name,
                                        const char *expected,
                                        const char *actual);

// Hands the claim back with reason "derived_mismatch", carrying everything
// contribute_record_derived_mismatch collected for it, and forgets those
// records. Like every other decline this remembers the job as unsupported, so
// the worker does not spend another three minutes rebuilding a table it has
// just found it cannot match -- except when the mismatch is the server's leave
// KLV, which the server can put right: then the job is set aside for a while
// (contribute_defer_job: from the idle interval, doubling up to
// CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS) and claimed again after.
//
// `why` is the executor's account of the mismatch. It is printed, with the
// decline, only for a file built here -- the contributor's MAGPIE is what has
// to change -- and not for the server's KLV, which is the server's to fix.
void contribute_decline_derived_mismatch(ContributeState *state,
                                         ThreadControl *thread_control,
                                         const char *why,
                                         ErrorStack *error_stack);

// The body of POST /api/worker/result: the claim it answers, the task's
// result (`result_json`, an object, as is), and `movegens`, the move
// generations this machine made computing it -- what the server credits the
// contributor with. The caller frees. Exposed for tests.
char *contribute_result_body(const char *claim_token, const char *result_json,
                             uint64_t movegens);

// Submits the result for the task claimed by the last contribute_claim_task
// call, with the move generations it took (see contribute_result_body), and
// stops its heartbeat. A task that produced a result prints "[HH:MM:SS]
// finished task #<n>: <job name> (<started> started, <completed> completed)",
// the counts this run's. Exactly one of result_json/error_message
// should be non-NULL: result_json on success, error_message (printed for the
// contributor, not sent to the server) on failure -- a failed task is never
// submitted but handed straight back (a decline with reason `task_failed`),
// so another worker can pick it up at once. A result the server refuses is
// handed back the same way. Too many consecutive failures or reaching
// max_tasks stop the run.
void contribute_submit_result(ContributeState *state,
                              ThreadControl *thread_control,
                              const char *result_json, uint64_t movegens,
                              const char *error_message,
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
                               ChttpResponse *response, bool *not_found,
                               ErrorStack *error_stack);

// The claimed job's server KLV verified: the job is no longer set aside, and
// a later failure starts its wait short again.
void contribute_artifact_verified(ContributeState *state);

// Whether the user asked the running command to stop (the REPL's `stop`, the
// API's stop call).
bool contribute_interrupted(ThreadControl *thread_control);

// The run's state, from the settings file; what contribute_claim_task makes
// on its first call. Exposed for tests.
ContributeState *contribute_state_create(const char *settings_path,
                                         ThreadControl *thread_control,
                                         ErrorStack *error_stack);
void contribute_state_destroy(ContributeState *state);

// The body of a claim: this build's version, its BOARD_DIM and RACK_SIZE, and
// the jobs to leave out -- those it cannot run, and those set aside for now.
// Exposed for tests.
char *contribute_claim_body(ContributeState *state,
                            const char *this_magpie_version);

// Whether a shutdown is waited out rather than obeyed: a data shutdown
// (`data_out_of_date`, `both`) answering a claim that named a set-aside job.
// A version or build shutdown (`magpie_too_old`, `unsupported_build`) is
// always obeyed. Exposed for tests.
bool contribute_shutdown_waits_for_deferral(const JsonValue *shutdown,
                                            bool claim_named_deferred);

// Sets a job aside, for twice as long as last time (from the idle interval to
// CONTRIBUTE_BAD_ARTIFACT_MAX_WAIT_SECONDS); returns the interval. Exposed
// for tests.
int contribute_defer_job(ContributeState *state, const char *job_id);

// Refuses an assignment that states no `expected_data`, one whose digests
// use an algorithm this build does not know (anything but sha256), or one
// listing a file without a role, name or digest, or with a role this build
// does not know: its input data could not be checked, and a task is never run
// unverified. Exposed for tests.
void contribute_check_expected_data(const JsonValue *assignment,
                                    ErrorStack *error_stack);

// Compares dotted numeric versions, returning <0, 0 or >0. Missing components
// count as zero, so "1.4" and "1.4.0" are equal. Exposed for testing: naive
// string comparison gets this wrong ("1.10" sorts below "1.9").
int contribute_compare_versions(const char *left, const char *right);

#endif

#ifndef ANALYSIS_PROGRESS_H
#define ANALYSIS_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

#define ANALYSIS_PROGRESS_SCHEMA_VERSION 7U
#define ANALYSIS_PROGRESS_ELAPSED_NOW INT64_C(-1)
#define ANALYSIS_PROGRESS_CPU_NOW INT64_C(-1)
#define ANALYSIS_PROGRESS_SCORE_SPREAD_UNSET INT32_MIN

// A solver-independent description of where computation is being spent.
// Values are intentionally copy-safe: an event never owns, or points into,
// live solver state.
typedef enum {
  ANALYSIS_MODE_NONE,
  ANALYSIS_MODE_PLAY_CHOOSER,
  ANALYSIS_MODE_STATIC,
  ANALYSIS_MODE_SIM,
  ANALYSIS_MODE_PEG,
  ANALYSIS_MODE_ENDGAME,
} analysis_mode_t;

typedef enum {
  ANALYSIS_EVENT_START,
  ANALYSIS_EVENT_CANDIDATE_SET,
  ANALYSIS_EVENT_CHECKPOINT,
  ANALYSIS_EVENT_CANDIDATE_DONE,
  ANALYSIS_EVENT_DEPTH_DONE,
  // A decision at an indivisible solver-work boundary. Endgame uses this
  // after a completed IDS depth to describe whether the next whole depth is
  // predicted to fit. In shadow mode the recommendation is recorded but not
  // enforced.
  ANALYSIS_EVENT_ADMISSION,
  ANALYSIS_EVENT_FALLBACK,
  ANALYSIS_EVENT_FINISH,
} analysis_event_t;

typedef enum {
  ANALYSIS_ADMISSION_NONE,
  ANALYSIS_ADMISSION_ADMIT,
  ANALYSIS_ADMISSION_REFUSE,
} analysis_admission_t;

typedef enum {
  ANALYSIS_STATUS_NONE,
  ANALYSIS_STATUS_COMPLETED,
  ANALYSIS_STATUS_TIME_LIMIT,
  ANALYSIS_STATUS_WORK_LIMIT,
  ANALYSIS_STATUS_INTERRUPTED,
  ANALYSIS_STATUS_ERROR,
  ANALYSIS_STATUS_NO_RESULT,
  ANALYSIS_STATUS_SKIPPED,
} analysis_status_t;

typedef struct AnalysisProgressEvent {
  uint32_t schema_version;
  // Assigned by an AnalysisTrace when recorded. Emitters leave this at zero.
  uint64_t sequence;
  // One logical solver invocation. A child solver run points at its enclosing
  // PlayChooser decision through parent_run_id.
  uint64_t run_id;
  uint64_t parent_run_id;
  analysis_mode_t mode;
  analysis_event_t event;
  analysis_status_t status;

  // Nanoseconds since this run's listener start. Emitters use
  // ANALYSIS_PROGRESS_ELAPSED_NOW to have analysis_progress_emit stamp it.
  int64_t elapsed_ns;
  // Aggregate process CPU nanoseconds consumed during the same interval.
  // cpu_ns / elapsed_ns is the average number of scheduled cores and helps
  // identify benchmark intervals disturbed by unrelated machine activity.
  int64_t cpu_ns;
  double budget_seconds;
  // Requested worker count for this solver run; -1 when not applicable.
  int workers;

  // Cumulative deterministic work. work_units is the mode's natural unit:
  // SIM iterations, PEG scenarios, and endgame nodes. The explicit fields
  // make cross-mode traces unambiguous.
  uint64_t work_units;
  uint64_t nodes;
  uint64_t iterations;
  uint64_t scenarios;
  // Work attributable to item_id for item-completion events. PEG uses these
  // to reconstruct the candidate completion timeline even when parallel
  // callbacks are delivered after a barrier and cumulative counters therefore
  // arrive out of timestamp order.
  uint64_t item_work_units;
  uint64_t item_nodes;
  // Prediction attached to an ADMISSION event. expected_next_work_units is
  // used to price the chunk; completion_bound_work_units is the one-sided
  // whole-chunk gate. Zero means no prediction was supplied.
  uint64_t expected_next_work_units;
  uint64_t completion_bound_work_units;
  // Explicit prediction coordinates. These keep a PEG stage's scenarios,
  // nested-endgame nodes, and candidate overhead reconstructible instead of
  // collapsing them into one hardware-dependent pseudo-unit.
  uint64_t expected_next_nodes;
  uint64_t completion_bound_nodes;
  uint64_t expected_next_scenarios;
  uint64_t completion_bound_scenarios;
  uint64_t expected_next_candidates;
  uint64_t completion_bound_candidates;
  double expected_next_seconds;
  double completion_bound_seconds;
  double completion_confidence;
  analysis_admission_t admission;
  bool admission_safe_to_enforce;
  bool admission_enforced;
  // Optional TimeManager diagnostics attached to an ADMISSION event.
  bool has_time_manager_plan;
  double expected_regret_reduction;
  double current_value_per_second;
  double future_value_per_second;
  double maximum_current_seconds;
  double deposit_seconds;

  // Generic progress coordinates. Unused coordinates are -1.
  int phase;
  int depth;
  int candidate_index;
  int candidates_completed;
  int candidates_total;
  // Optional finer-grained coverage inside the current candidate. Endgame
  // uses this for ply-2 children while a root move is being searched.
  int subcandidates_completed;
  int subcandidates_total;
  int best_index;
  int challenger_index;
  // Number of non-incumbent arms whose calibrated difference interval still
  // reaches the incumbent. SIM populates this at checkpoints and FINISH;
  // other solvers leave it at -1.
  int near_tie_challengers;
  // Risk-set membership bitmask matching near_tie_challengers: bit arm_index
  // is set for each counted near-tie challenger (arm indices 0..63; the
  // incumbent's bit is never set — it is implicit via best_index). Arms at
  // index 64 and above cannot be represented; the count stays authoritative.
  // SIM populates this on CHECKPOINT events only (FINISH reports the count
  // recorded at stop and leaves the mask 0); other solvers leave it 0. Bits
  // are raw BAI arm indices — consumers that need candidate ranks must remap
  // through their own arm-to-rank tables (see the thinking-curve harness).
  uint64_t near_tie_member_mask;

  // Copy-safe identity for the event's move/arm. SIM uses the arm index,
  // endgame uses tiny_move, and PEG/PlayChooser use a stable Move fingerprint.
  uint64_t item_id;

  // Solver-native values. value is the item/depth value; best_value and
  // challenger_value describe the current decision boundary. secondary_value
  // carries a second currency when useful (for example PEG mean spread beside
  // win probability).
  double value;
  double best_value;
  double challenger_value;
  double secondary_value;

  // Position/clock context is normally populated by the enclosing
  // PlayChooser START event and inherited by joining on parent_run_id.
  int player_on_turn;
  int bag_tiles;
  int player_rack_tiles;
  int opponent_rack_tiles;
  int consecutive_scoreless_turns;
  // ANALYSIS_PROGRESS_SCORE_SPREAD_UNSET when this event has no position
  // context; child events can inherit it from their run's START event.
  int score_spread;
  double clock_seconds_remaining;
} AnalysisProgressEvent;

// May be called concurrently by solver workers. Implementations must be
// thread-safe, observational, and quick enough not to perturb the solve.
typedef void (*AnalysisProgressCallback)(const AnalysisProgressEvent *event,
                                         void *user_data);

// A listener is a cheap, caller-owned template. Solvers copy it into their
// invocation state. checkpoint_interval is interpreted in the mode's natural
// work units; zero disables periodic checkpoints while preserving structural
// events such as START, candidate/depth completions, and FINISH.
typedef struct AnalysisProgressListener {
  AnalysisProgressCallback callback;
  void *user_data;
  uint64_t run_id;
  uint64_t parent_run_id;
  int64_t start_ns;
  int64_t start_cpu_ns;
  uint64_t checkpoint_interval;
} AnalysisProgressListener;

AnalysisProgressEvent analysis_progress_event_create(analysis_mode_t mode,
                                                     analysis_event_t event);
AnalysisProgressListener analysis_progress_listener_for_run(
    const AnalysisProgressListener *listener_template, uint64_t run_id,
    uint64_t parent_run_id, int64_t start_ns);
bool analysis_progress_is_enabled(const AnalysisProgressListener *listener);
void analysis_progress_emit(const AnalysisProgressListener *listener,
                            const AnalysisProgressEvent *event);

const char *analysis_mode_name(analysis_mode_t mode);
const char *analysis_event_name(analysis_event_t event);
const char *analysis_status_name(analysis_status_t status);
const char *analysis_admission_name(analysis_admission_t admission);

#endif

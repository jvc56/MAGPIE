#ifndef ANALYSIS_PROGRESS_H
#define ANALYSIS_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

#define ANALYSIS_PROGRESS_SCHEMA_VERSION 1U
#define ANALYSIS_PROGRESS_ELAPSED_NOW INT64_C(-1)
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
  ANALYSIS_EVENT_FALLBACK,
  ANALYSIS_EVENT_FINISH,
} analysis_event_t;

typedef enum {
  ANALYSIS_STATUS_NONE,
  ANALYSIS_STATUS_COMPLETED,
  ANALYSIS_STATUS_TIME_LIMIT,
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
  double budget_seconds;

  // Cumulative deterministic work. work_units is the mode's natural unit:
  // SIM iterations, PEG scenarios, and endgame nodes. The explicit fields
  // make cross-mode traces unambiguous.
  uint64_t work_units;
  uint64_t nodes;
  uint64_t iterations;
  uint64_t scenarios;

  // Generic progress coordinates. Unused coordinates are -1.
  int phase;
  int depth;
  int candidate_index;
  int candidates_completed;
  int candidates_total;
  int best_index;
  int challenger_index;

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

#endif

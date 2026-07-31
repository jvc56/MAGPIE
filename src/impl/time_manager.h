#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include "../ent/analysis_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One incremental unit of solver work, measured from the last usable result
// boundary. Keeping the coordinates separate is intentional: an endgame depth
// is not interchangeable with a PEG candidate, and PEG has meaningful work
// even when no nested endgame nodes were searched.
typedef struct TimeManagerWork {
  analysis_mode_t mode;
  uint64_t nodes;
  uint64_t scenarios;
  uint64_t candidates;
  double fixed_seconds;
} TimeManagerWork;

// Local wall-clock conversion. Strength curves remain expressed in portable
// solver work; these coefficients describe the processing power currently
// available to this process. PEG's scenario and nested-endgame terms stay
// separate instead of being collapsed into a misleading PEG "NPS".
//
// TimeManager receives separate expected and deadline models. The expected
// model prices marginal value; the deadline model uses conservative live
// throughput so load/throttling uncertainty is part of the admission gate.
typedef struct TimeManagerCostModel {
  double sim_seconds_per_node;
  double endgame_seconds_per_node;
  // Per-PEG-boundary setup/coordination cost. This is separate from
  // TimeManagerWork.fixed_seconds: the latter belongs to a particular chunk,
  // while this coefficient is part of the currently observed machine/load
  // conversion and therefore scales with the other runtime coefficients.
  double peg_fixed_seconds_per_chunk;
  double peg_seconds_per_scenario;
  double peg_seconds_per_endgame_node;
  double peg_seconds_per_candidate;
} TimeManagerCostModel;

typedef enum TimeManagerBoundary {
  TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
  TIME_MANAGER_BOUNDARY_PEG_CANDIDATE,
  TIME_MANAGER_BOUNDARY_PEG_WAVE,
  TIME_MANAGER_BOUNDARY_PEG_STAGE,
  TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH,
} TimeManagerBoundary;

typedef struct TimeManagerChunk {
  TimeManagerBoundary boundary;
  // Expected work prices this chunk against other current/future work.
  TimeManagerWork work;
  // A one-sided work bound independently gates whether the indivisible chunk
  // may start. For an endgame depth this is calibrated at (initially) p99.
  // Endgame depths and PEG boundaries require this. When false, only an
  // interruptible/deterministic boundary such as a sim checkpoint may use
  // `work` for both costs.
  bool has_completion_bound;
  TimeManagerWork completion_bound_work;
  // Joint lower-bound confidence after accounting for both work-tail and
  // conservative-throughput uncertainty.
  double completion_confidence;
  // Expected reduction in the decision's utility regret if this whole chunk
  // completes. Partial chunks are never credited.
  double expected_regret_reduction;
} TimeManagerChunk;

typedef struct TimeManagerClock {
  double remaining_seconds;
  // Includes the current turn.
  int turns_remaining;
  // Clock that may not be spent: response/overtime safety.
  double safety_reserve_seconds;
  // Phase-aware total protected demand across all future turns. This is not
  // an equal current-turn slice: later endgame searches are usually cheaper.
  double future_reserve_seconds;
  // Irreducible current-turn work needed to return a legal fallback.
  double committed_current_seconds;
  // Expected utility-regret reduction per second available on future turns.
  // This is the shadow price lambda.
  double future_value_per_second;
  // Minimum one-sided completion confidence for indivisible solver work.
  // The initial production target is 0.99. A statistically valid bound below
  // this policy threshold is still refused.
  double minimum_completion_confidence;
} TimeManagerClock;

typedef enum TimeManagerStopReason {
  TIME_MANAGER_STOP_NO_MORE_CHUNKS,
  TIME_MANAGER_STOP_FUTURE_VALUE,
  TIME_MANAGER_STOP_CLOCK_RESERVE,
  TIME_MANAGER_STOP_COMPLETION_RISK,
  TIME_MANAGER_STOP_INVALID_INPUT,
} TimeManagerStopReason;

typedef struct TimeManagerPlan {
  size_t chunks_bought;
  // Expected wall time is used for value and deposit accounting.
  double planned_seconds;
  // Conservative cumulative wall bound used only for hard admission.
  double planned_completion_bound_seconds;
  double expected_regret_reduction;
  double maximum_current_seconds;
  double equal_slice_seconds;
  // Positive means the plan deposits time relative to equal slicing; negative
  // means it withdraws previously banked clock for a valuable turn.
  double deposit_seconds;
  double stopped_chunk_seconds;
  double stopped_chunk_completion_bound_seconds;
  double stopped_chunk_completion_confidence;
  double stopped_chunk_value_per_second;
  TimeManagerStopReason stop_reason;
  bool valid;
} TimeManagerPlan;

// Returns NAN when the model/work combination is invalid or unsupported.
double time_manager_estimate_seconds(const TimeManagerCostModel *model,
                                     const TimeManagerWork *work);

// Buy sequential result-boundary chunks while they fit under the protected
// clock and their marginal value is strictly greater than the future shadow
// price. A low-value chunk blocks all later chunks because the work is
// sequential: TimeManager cannot skip half an IDS depth or PEG stage.
// `chunks_bought` is a forecast, not permission to run every listed boundary
// without another check. Execute at most the next chunk, record its actual
// work/rate, and replan before starting another indivisible unit.
TimeManagerPlan
time_manager_plan(const TimeManagerClock *clock,
                  const TimeManagerCostModel *expected_cost_model,
                  const TimeManagerCostModel *deadline_cost_model,
                  const TimeManagerChunk *chunks, size_t num_chunks);

#endif

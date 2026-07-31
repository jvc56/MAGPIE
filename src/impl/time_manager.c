#include "time_manager.h"

#include <math.h>

static bool time_manager_nonnegative_finite(double value) {
  return isfinite(value) && value >= 0.0;
}

static bool
time_manager_boundary_requires_completion_bound(TimeManagerBoundary boundary) {
  return boundary == TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH ||
         boundary == TIME_MANAGER_BOUNDARY_PEG_CANDIDATE ||
         boundary == TIME_MANAGER_BOUNDARY_PEG_WAVE ||
         boundary == TIME_MANAGER_BOUNDARY_PEG_STAGE;
}

double time_manager_estimate_seconds(const TimeManagerCostModel *model,
                                     const TimeManagerWork *work) {
  if (model == NULL || work == NULL ||
      !time_manager_nonnegative_finite(work->fixed_seconds)) {
    return NAN;
  }

  double seconds = work->fixed_seconds;
  switch (work->mode) {
  case ANALYSIS_MODE_SIM:
    if (!time_manager_nonnegative_finite(model->sim_seconds_per_node)) {
      return NAN;
    }
    seconds += (double)work->nodes * model->sim_seconds_per_node;
    break;
  case ANALYSIS_MODE_ENDGAME:
    if (!time_manager_nonnegative_finite(model->endgame_seconds_per_node)) {
      return NAN;
    }
    seconds += (double)work->nodes * model->endgame_seconds_per_node;
    break;
  case ANALYSIS_MODE_PEG:
    if (!time_manager_nonnegative_finite(model->peg_fixed_seconds_per_chunk) ||
        !time_manager_nonnegative_finite(model->peg_seconds_per_scenario) ||
        !time_manager_nonnegative_finite(model->peg_seconds_per_endgame_node) ||
        !time_manager_nonnegative_finite(model->peg_seconds_per_candidate)) {
      return NAN;
    }
    seconds += model->peg_fixed_seconds_per_chunk +
               (double)work->scenarios * model->peg_seconds_per_scenario +
               (double)work->nodes * model->peg_seconds_per_endgame_node +
               (double)work->candidates * model->peg_seconds_per_candidate;
    break;
  case ANALYSIS_MODE_STATIC:
    break;
  case ANALYSIS_MODE_NONE:
  case ANALYSIS_MODE_PLAY_CHOOSER:
  default:
    return NAN;
  }
  return isfinite(seconds) && seconds >= 0.0 ? seconds : NAN;
}

static TimeManagerPlan time_manager_invalid_plan(void) {
  TimeManagerPlan plan = {0};
  plan.valid = false;
  plan.stop_reason = TIME_MANAGER_STOP_INVALID_INPUT;
  plan.planned_seconds = NAN;
  plan.planned_completion_bound_seconds = NAN;
  plan.expected_regret_reduction = NAN;
  plan.maximum_current_seconds = NAN;
  plan.equal_slice_seconds = NAN;
  plan.deposit_seconds = NAN;
  plan.stopped_chunk_seconds = NAN;
  plan.stopped_chunk_completion_bound_seconds = NAN;
  plan.stopped_chunk_completion_confidence = NAN;
  plan.stopped_chunk_value_per_second = NAN;
  return plan;
}

TimeManagerPlan
time_manager_plan(const TimeManagerClock *clock,
                  const TimeManagerCostModel *expected_cost_model,
                  const TimeManagerCostModel *deadline_cost_model,
                  const TimeManagerChunk *chunks, size_t num_chunks) {
  if (clock == NULL || expected_cost_model == NULL ||
      deadline_cost_model == NULL || (num_chunks > 0 && chunks == NULL) ||
      clock->turns_remaining <= 0 ||
      !time_manager_nonnegative_finite(clock->remaining_seconds) ||
      !time_manager_nonnegative_finite(clock->safety_reserve_seconds) ||
      !time_manager_nonnegative_finite(clock->future_reserve_seconds) ||
      !time_manager_nonnegative_finite(clock->committed_current_seconds) ||
      !time_manager_nonnegative_finite(clock->future_value_per_second) ||
      !isfinite(clock->minimum_completion_confidence) ||
      clock->minimum_completion_confidence <= 0.0 ||
      clock->minimum_completion_confidence > 1.0) {
    return time_manager_invalid_plan();
  }

  const double clock_after_safety =
      fmax(0.0, clock->remaining_seconds - clock->safety_reserve_seconds);
  const double maximum_current_seconds =
      fmax(0.0, clock_after_safety - clock->future_reserve_seconds);

  TimeManagerPlan plan = {0};
  plan.valid = true;
  plan.maximum_current_seconds = maximum_current_seconds;
  plan.equal_slice_seconds =
      clock_after_safety / (double)clock->turns_remaining;
  plan.planned_seconds = clock->committed_current_seconds;
  plan.planned_completion_bound_seconds = clock->committed_current_seconds;
  plan.stop_reason = TIME_MANAGER_STOP_NO_MORE_CHUNKS;
  plan.stopped_chunk_seconds = NAN;
  plan.stopped_chunk_completion_bound_seconds = NAN;
  plan.stopped_chunk_completion_confidence = NAN;
  plan.stopped_chunk_value_per_second = NAN;

  if (plan.planned_seconds > maximum_current_seconds) {
    plan.stop_reason = TIME_MANAGER_STOP_CLOCK_RESERVE;
    plan.deposit_seconds = plan.equal_slice_seconds - plan.planned_seconds;
    return plan;
  }

  for (size_t chunk_index = 0; chunk_index < num_chunks; chunk_index++) {
    const TimeManagerChunk *chunk = &chunks[chunk_index];
    const double expected_seconds =
        time_manager_estimate_seconds(expected_cost_model, &chunk->work);
    const TimeManagerWork *completion_work = chunk->has_completion_bound
                                                 ? &chunk->completion_bound_work
                                                 : &chunk->work;
    const double completion_bound_seconds =
        time_manager_estimate_seconds(deadline_cost_model, completion_work);
    const bool valid_completion_bound =
        time_manager_nonnegative_finite(completion_bound_seconds) &&
        completion_bound_seconds >= expected_seconds &&
        (!chunk->has_completion_bound ||
         (chunk->completion_bound_work.mode == chunk->work.mode &&
          isfinite(chunk->completion_confidence) &&
          chunk->completion_confidence > 0.0 &&
          chunk->completion_confidence <= 1.0));
    if (!time_manager_nonnegative_finite(expected_seconds) ||
        !valid_completion_bound ||
        !time_manager_nonnegative_finite(chunk->expected_regret_reduction)) {
      plan.valid = false;
      plan.stop_reason = TIME_MANAGER_STOP_INVALID_INPUT;
      plan.stopped_chunk_seconds = expected_seconds;
      plan.stopped_chunk_completion_bound_seconds = completion_bound_seconds;
      plan.stopped_chunk_completion_confidence = chunk->completion_confidence;
      plan.stopped_chunk_value_per_second = NAN;
      plan.deposit_seconds = plan.equal_slice_seconds - plan.planned_seconds;
      return plan;
    }

    double value_per_second = 0.0;
    if (expected_seconds == 0.0) {
      value_per_second =
          chunk->expected_regret_reduction > 0.0 ? INFINITY : 0.0;
    } else {
      value_per_second = chunk->expected_regret_reduction / expected_seconds;
    }
    if (time_manager_boundary_requires_completion_bound(chunk->boundary) &&
        !chunk->has_completion_bound) {
      plan.stop_reason = TIME_MANAGER_STOP_COMPLETION_RISK;
      plan.stopped_chunk_seconds = expected_seconds;
      plan.stopped_chunk_completion_bound_seconds = NAN;
      plan.stopped_chunk_completion_confidence = NAN;
      plan.stopped_chunk_value_per_second = value_per_second;
      break;
    }
    if (time_manager_boundary_requires_completion_bound(chunk->boundary) &&
        chunk->completion_confidence < clock->minimum_completion_confidence) {
      plan.stop_reason = TIME_MANAGER_STOP_COMPLETION_RISK;
      plan.stopped_chunk_seconds = expected_seconds;
      plan.stopped_chunk_completion_bound_seconds = completion_bound_seconds;
      plan.stopped_chunk_completion_confidence = chunk->completion_confidence;
      plan.stopped_chunk_value_per_second = value_per_second;
      break;
    }
    if (plan.planned_completion_bound_seconds + completion_bound_seconds >
        maximum_current_seconds) {
      plan.stop_reason = chunk->has_completion_bound
                             ? TIME_MANAGER_STOP_COMPLETION_RISK
                             : TIME_MANAGER_STOP_CLOCK_RESERVE;
      plan.stopped_chunk_seconds = expected_seconds;
      plan.stopped_chunk_completion_bound_seconds = completion_bound_seconds;
      plan.stopped_chunk_completion_confidence = chunk->completion_confidence;
      plan.stopped_chunk_value_per_second = value_per_second;
      break;
    }
    if (value_per_second <= clock->future_value_per_second) {
      plan.stop_reason = TIME_MANAGER_STOP_FUTURE_VALUE;
      plan.stopped_chunk_seconds = expected_seconds;
      plan.stopped_chunk_completion_bound_seconds = completion_bound_seconds;
      plan.stopped_chunk_completion_confidence = chunk->completion_confidence;
      plan.stopped_chunk_value_per_second = value_per_second;
      break;
    }

    plan.chunks_bought++;
    plan.planned_seconds += expected_seconds;
    plan.planned_completion_bound_seconds += completion_bound_seconds;
    plan.expected_regret_reduction += chunk->expected_regret_reduction;
  }

  plan.deposit_seconds = plan.equal_slice_seconds - plan.planned_seconds;
  return plan;
}

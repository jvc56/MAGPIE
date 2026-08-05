#include "analysis_progress.h"

#include "../compat/ctime.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>

AnalysisProgressEvent analysis_progress_event_create(analysis_mode_t mode,
                                                     analysis_event_t event) {
  AnalysisProgressEvent progress = {
      .schema_version = ANALYSIS_PROGRESS_SCHEMA_VERSION,
      .mode = mode,
      .event = event,
      .elapsed_ns = ANALYSIS_PROGRESS_ELAPSED_NOW,
      .cpu_ns = ANALYSIS_PROGRESS_CPU_NOW,
      .workers = -1,
      .phase = -1,
      .depth = -1,
      .candidate_index = -1,
      .candidates_completed = -1,
      .candidates_total = -1,
      .subcandidates_completed = -1,
      .subcandidates_total = -1,
      .best_index = -1,
      .challenger_index = -1,
      .near_tie_challengers = -1,
      .value = NAN,
      .best_value = NAN,
      .challenger_value = NAN,
      .secondary_value = NAN,
      .expected_next_seconds = NAN,
      .completion_bound_seconds = NAN,
      .completion_confidence = NAN,
      .admission = ANALYSIS_ADMISSION_NONE,
      .expected_regret_reduction = NAN,
      .current_value_per_second = NAN,
      .future_value_per_second = NAN,
      .maximum_current_seconds = NAN,
      .deposit_seconds = NAN,
      .player_on_turn = -1,
      .bag_tiles = -1,
      .player_rack_tiles = -1,
      .opponent_rack_tiles = -1,
      .consecutive_scoreless_turns = -1,
      .score_spread = ANALYSIS_PROGRESS_SCORE_SPREAD_UNSET,
      .clock_seconds_remaining = NAN,
  };
  return progress;
}

AnalysisProgressListener analysis_progress_listener_for_run(
    const AnalysisProgressListener *listener_template, uint64_t run_id,
    uint64_t parent_run_id, int64_t start_ns) {
  AnalysisProgressListener listener = {0};
  if (listener_template != NULL) {
    listener = *listener_template;
  }
  listener.run_id = run_id;
  listener.parent_run_id = parent_run_id;
  listener.start_ns = start_ns;
  listener.start_cpu_ns = start_ns > 0 ? ctimer_process_cpu_ns() : 0;
  return listener;
}

bool analysis_progress_is_enabled(const AnalysisProgressListener *listener) {
  return listener != NULL && listener->callback != NULL;
}

void analysis_progress_emit(const AnalysisProgressListener *listener,
                            const AnalysisProgressEvent *event) {
  if (!analysis_progress_is_enabled(listener) || event == NULL) {
    return;
  }
  AnalysisProgressEvent stamped = *event;
  stamped.schema_version = ANALYSIS_PROGRESS_SCHEMA_VERSION;
  stamped.sequence = 0;
  stamped.run_id = listener->run_id;
  stamped.parent_run_id = listener->parent_run_id;
  if (stamped.elapsed_ns == ANALYSIS_PROGRESS_ELAPSED_NOW) {
    stamped.elapsed_ns =
        listener->start_ns > 0 ? ctimer_monotonic_ns() - listener->start_ns : 0;
  }
  if (stamped.cpu_ns == ANALYSIS_PROGRESS_CPU_NOW) {
    stamped.cpu_ns = listener->start_cpu_ns > 0
                         ? ctimer_process_cpu_ns() - listener->start_cpu_ns
                         : 0;
  }
  listener->callback(&stamped, listener->user_data);
}

const char *analysis_mode_name(analysis_mode_t mode) {
  switch (mode) {
  case ANALYSIS_MODE_NONE:
    return "none";
  case ANALYSIS_MODE_PLAY_CHOOSER:
    return "play_chooser";
  case ANALYSIS_MODE_STATIC:
    return "static";
  case ANALYSIS_MODE_SIM:
    return "sim";
  case ANALYSIS_MODE_PEG:
    return "peg";
  case ANALYSIS_MODE_ENDGAME:
    return "endgame";
  }
  return "unknown";
}

const char *analysis_event_name(analysis_event_t event) {
  switch (event) {
  case ANALYSIS_EVENT_START:
    return "start";
  case ANALYSIS_EVENT_CANDIDATE_SET:
    return "candidate_set";
  case ANALYSIS_EVENT_CHECKPOINT:
    return "checkpoint";
  case ANALYSIS_EVENT_CANDIDATE_DONE:
    return "candidate_done";
  case ANALYSIS_EVENT_DEPTH_DONE:
    return "depth_done";
  case ANALYSIS_EVENT_ADMISSION:
    return "admission";
  case ANALYSIS_EVENT_FALLBACK:
    return "fallback";
  case ANALYSIS_EVENT_FINISH:
    return "finish";
  }
  return "unknown";
}

const char *analysis_admission_name(analysis_admission_t admission) {
  switch (admission) {
  case ANALYSIS_ADMISSION_NONE:
    return "none";
  case ANALYSIS_ADMISSION_ADMIT:
    return "admit";
  case ANALYSIS_ADMISSION_REFUSE:
    return "refuse";
  }
  return "unknown";
}

const char *analysis_status_name(analysis_status_t status) {
  switch (status) {
  case ANALYSIS_STATUS_NONE:
    return "none";
  case ANALYSIS_STATUS_COMPLETED:
    return "completed";
  case ANALYSIS_STATUS_TIME_LIMIT:
    return "time_limit";
  case ANALYSIS_STATUS_WORK_LIMIT:
    return "work_limit";
  case ANALYSIS_STATUS_INTERRUPTED:
    return "interrupted";
  case ANALYSIS_STATUS_ERROR:
    return "error";
  case ANALYSIS_STATUS_NO_RESULT:
    return "no_result";
  case ANALYSIS_STATUS_SKIPPED:
    return "skipped";
  }
  return "unknown";
}

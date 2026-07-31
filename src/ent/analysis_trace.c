#include "analysis_trace.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <inttypes.h>
#include <stdlib.h>

struct AnalysisTrace {
  cpthread_mutex_t mutex;
  AnalysisProgressEvent *events;
  size_t capacity;
  size_t count;
  uint64_t dropped;
  uint64_t next_sequence;
};

AnalysisTrace *analysis_trace_create(size_t capacity) {
  if (capacity > SIZE_MAX / sizeof(AnalysisProgressEvent)) {
    log_fatal("analysis trace capacity is too large");
  }
  AnalysisTrace *trace = malloc_or_die(sizeof(AnalysisTrace));
  cpthread_mutex_init(&trace->mutex);
  trace->events =
      capacity > 0 ? malloc_or_die(capacity * sizeof(*trace->events)) : NULL;
  trace->capacity = capacity;
  trace->count = 0;
  trace->dropped = 0;
  trace->next_sequence = 1;
  return trace;
}

void analysis_trace_destroy(AnalysisTrace *trace) {
  if (trace == NULL) {
    return;
  }
  // Project mutexes do not dynamically allocate; see cpthread conventions.
  free(trace->events);
  free(trace);
}

void analysis_trace_reset(AnalysisTrace *trace) {
  if (trace == NULL) {
    return;
  }
  cpthread_mutex_lock(&trace->mutex);
  trace->count = 0;
  trace->dropped = 0;
  trace->next_sequence = 1;
  cpthread_mutex_unlock(&trace->mutex);
}

size_t analysis_trace_get_count(AnalysisTrace *trace) {
  if (trace == NULL) {
    return 0;
  }
  cpthread_mutex_lock(&trace->mutex);
  const size_t count = trace->count;
  cpthread_mutex_unlock(&trace->mutex);
  return count;
}

uint64_t analysis_trace_get_dropped(AnalysisTrace *trace) {
  if (trace == NULL) {
    return 0;
  }
  cpthread_mutex_lock(&trace->mutex);
  const uint64_t dropped = trace->dropped;
  cpthread_mutex_unlock(&trace->mutex);
  return dropped;
}

bool analysis_trace_get_event(AnalysisTrace *trace, size_t index,
                              AnalysisProgressEvent *event) {
  if (trace == NULL || event == NULL) {
    return false;
  }
  cpthread_mutex_lock(&trace->mutex);
  const bool found = index < trace->count;
  if (found) {
    *event = trace->events[index];
  }
  cpthread_mutex_unlock(&trace->mutex);
  return found;
}

void analysis_trace_record(const AnalysisProgressEvent *event,
                           void *user_data) {
  AnalysisTrace *trace = user_data;
  if (trace == NULL || event == NULL) {
    return;
  }
  cpthread_mutex_lock(&trace->mutex);
  const uint64_t sequence = trace->next_sequence++;
  if (trace->count < trace->capacity) {
    trace->events[trace->count] = *event;
    trace->events[trace->count].sequence = sequence;
    trace->count++;
  } else {
    trace->dropped++;
  }
  cpthread_mutex_unlock(&trace->mutex);
}

bool analysis_trace_write_tsv(AnalysisTrace *trace, FILE *stream) {
  if (trace == NULL || stream == NULL) {
    return false;
  }

  cpthread_mutex_lock(&trace->mutex);
  const size_t count = trace->count;
  AnalysisProgressEvent *events =
      count > 0 ? malloc_or_die(count * sizeof(*events)) : NULL;
  for (size_t i = 0; i < count; i++) {
    events[i] = trace->events[i];
  }
  cpthread_mutex_unlock(&trace->mutex);

  bool ok =
      fprintf(stream,
              "schema_version\tsequence\trun_id\tparent_run_id\tmode\tevent\t"
              "status\t"
              "elapsed_ns\tcpu_ns\tbudget_seconds\tworkers\twork_units\tnodes\t"
              "iterations\tscenarios\titem_work_units\titem_nodes\t"
              "expected_next_work_units\tcompletion_bound_work_units\t"
              "expected_next_nodes\tcompletion_bound_nodes\t"
              "expected_next_scenarios\tcompletion_bound_scenarios\t"
              "expected_next_candidates\tcompletion_bound_candidates\t"
              "expected_next_seconds\tcompletion_bound_seconds\t"
              "completion_confidence\tadmission\t"
              "admission_safe_to_enforce\tadmission_enforced\t"
              "has_time_manager_plan\texpected_regret_reduction\t"
              "current_value_per_second\tfuture_value_per_second\t"
              "maximum_current_seconds\tdeposit_seconds\tphase\t"
              "depth\tcandidate_index\t"
              "candidates_completed\tcandidates_total\t"
              "subcandidates_completed\tsubcandidates_total\tbest_index\t"
              "challenger_index\titem_id\tvalue\tbest_value\t"
              "challenger_value\tsecondary_value\tplayer_on_turn\tbag_tiles\t"
              "player_rack_tiles\topponent_rack_tiles\t"
              "consecutive_scoreless_turns\t"
              "score_spread\tclock_seconds_remaining\n") >= 0;
  for (size_t i = 0; ok && i < count; i++) {
    const AnalysisProgressEvent *event = &events[i];
    ok = fprintf(stream,
                 "%" PRIu32 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
                 "\t%s\t%s\t%s\t%" PRId64 "\t%" PRId64 "\t%.17g\t%d",
                 event->schema_version, event->sequence, event->run_id,
                 event->parent_run_id, analysis_mode_name(event->mode),
                 analysis_event_name(event->event),
                 analysis_status_name(event->status), event->elapsed_ns,
                 event->cpu_ns, event->budget_seconds, event->workers) >= 0;
    ok = ok &&
         fprintf(
             stream,
             "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
             "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
             "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
             "\t%.17g\t%.17g\t%.17g\t%s\t%d\t%d",
             event->work_units, event->nodes, event->iterations,
             event->scenarios, event->item_work_units, event->item_nodes,
             event->expected_next_work_units,
             event->completion_bound_work_units, event->expected_next_nodes,
             event->completion_bound_nodes, event->expected_next_scenarios,
             event->completion_bound_scenarios, event->expected_next_candidates,
             event->completion_bound_candidates, event->expected_next_seconds,
             event->completion_bound_seconds, event->completion_confidence,
             analysis_admission_name(event->admission),
             event->admission_safe_to_enforce ? 1 : 0,
             event->admission_enforced ? 1 : 0) >= 0;
    ok =
        ok &&
        fprintf(stream,
                "\t%d\t%.17g\t%.17g\t%.17g\t%.17g\t%.17g"
                "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d",
                event->has_time_manager_plan ? 1 : 0,
                event->expected_regret_reduction,
                event->current_value_per_second, event->future_value_per_second,
                event->maximum_current_seconds, event->deposit_seconds,
                event->phase, event->depth, event->candidate_index,
                event->candidates_completed, event->candidates_total,
                event->subcandidates_completed, event->subcandidates_total,
                event->best_index, event->challenger_index) >= 0;
    ok = ok && fprintf(stream,
                       "\t%" PRIu64 "\t%.17g\t%.17g\t%.17g\t%.17g"
                       "\t%d\t%d\t%d\t%d\t%d\t%d\t%.17g\n",
                       event->item_id, event->value, event->best_value,
                       event->challenger_value, event->secondary_value,
                       event->player_on_turn, event->bag_tiles,
                       event->player_rack_tiles, event->opponent_rack_tiles,
                       event->consecutive_scoreless_turns, event->score_spread,
                       event->clock_seconds_remaining) >= 0;
  }
  free(events);
  return ok;
}

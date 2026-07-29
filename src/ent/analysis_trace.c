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
              "elapsed_ns\tbudget_seconds\twork_units\tnodes\titerations\t"
              "scenarios\tphase\tdepth\tcandidate_index\t"
              "candidates_completed\tcandidates_total\tbest_index\t"
              "challenger_index\titem_id\tvalue\tbest_value\t"
              "challenger_value\tsecondary_value\tplayer_on_turn\tbag_tiles\t"
              "score_spread\tclock_seconds_remaining\n") >= 0;
  for (size_t i = 0; ok && i < count; i++) {
    const AnalysisProgressEvent *event = &events[i];
    ok = fprintf(
             stream,
             "%" PRIu32 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
             "\t%s\t%s\t%s\t%" PRId64 "\t%.17g\t%" PRIu64 "\t%" PRIu64
             "\t%" PRIu64 "\t%" PRIu64 "\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%" PRIu64
             "\t%.17g\t%.17g\t%.17g\t%.17g\t%d\t%d\t%d\t%.17g\n",
             event->schema_version, event->sequence, event->run_id,
             event->parent_run_id, analysis_mode_name(event->mode),
             analysis_event_name(event->event),
             analysis_status_name(event->status), event->elapsed_ns,
             event->budget_seconds, event->work_units, event->nodes,
             event->iterations, event->scenarios, event->phase, event->depth,
             event->candidate_index, event->candidates_completed,
             event->candidates_total, event->best_index,
             event->challenger_index, event->item_id, event->value,
             event->best_value, event->challenger_value, event->secondary_value,
             event->player_on_turn, event->bag_tiles, event->score_spread,
             event->clock_seconds_remaining) >= 0;
  }
  free(events);
  return ok;
}

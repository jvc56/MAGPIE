#ifndef ANALYSIS_TRACE_H
#define ANALYSIS_TRACE_H

#include "analysis_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct AnalysisTrace AnalysisTrace;

// Fixed-capacity, thread-safe trace. Recording never allocates and never does
// file I/O on a solver thread; excess events are counted and dropped.
AnalysisTrace *analysis_trace_create(size_t capacity);
// Producers must be quiescent before destroy. reset/get/record/export are
// internally synchronized and may run while producers are active.
void analysis_trace_destroy(AnalysisTrace *trace);
void analysis_trace_reset(AnalysisTrace *trace);
size_t analysis_trace_get_count(AnalysisTrace *trace);
uint64_t analysis_trace_get_dropped(AnalysisTrace *trace);
bool analysis_trace_get_event(AnalysisTrace *trace, size_t index,
                              AnalysisProgressEvent *event);

// Signature-compatible AnalysisProgressCallback.
void analysis_trace_record(const AnalysisProgressEvent *event, void *user_data);

// Stable tab-separated schema intended for offline calibration. Call only
// after (or otherwise externally synchronized with) the solve when a coherent
// point-in-time export is required.
bool analysis_trace_write_tsv(AnalysisTrace *trace, FILE *stream);

#endif

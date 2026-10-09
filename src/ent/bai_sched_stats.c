#include "bai_sched_stats.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static cpthread_mutex_t bai_sched_stats_mutex =
    PTHREAD_MUTEX_INITIALIZER; // NOLINT(misc-include-cleaner)
static BAISchedSimStats *bai_sched_stats_records = NULL;
static int bai_sched_stats_num_records = 0;
static int bai_sched_stats_capacity = 0;

void bai_sched_histogram_reset(BAISchedHistogram *histogram) {
  memset(histogram, 0, sizeof(*histogram));
}

static int bai_sched_histogram_bucket(const int64_t cost_ns) {
  if (cost_ns <= 1) {
    return 0;
  }
  const int bucket =
      (int)(log2((double)cost_ns) * (double)BAI_SCHED_HIST_BUCKETS_PER_OCTAVE);
  if (bucket >= BAI_SCHED_HIST_NUM_BUCKETS) {
    return BAI_SCHED_HIST_NUM_BUCKETS - 1;
  }
  return bucket;
}

void bai_sched_histogram_add(BAISchedHistogram *histogram,
                             const int64_t cost_ns) {
  histogram->counts[bai_sched_histogram_bucket(cost_ns)]++;
  histogram->num_samples++;
  histogram->total_ns += cost_ns;
  if (cost_ns > histogram->max_ns) {
    histogram->max_ns = cost_ns;
  }
}

void bai_sched_histogram_merge(BAISchedHistogram *dst,
                               const BAISchedHistogram *src) {
  for (int bucket = 0; bucket < BAI_SCHED_HIST_NUM_BUCKETS; bucket++) {
    dst->counts[bucket] += src->counts[bucket];
  }
  dst->num_samples += src->num_samples;
  dst->total_ns += src->total_ns;
  if (src->max_ns > dst->max_ns) {
    dst->max_ns = src->max_ns;
  }
}

int64_t bai_sched_histogram_quantile(const BAISchedHistogram *histogram,
                                     const double quantile) {
  if (histogram->num_samples == 0) {
    return 0;
  }
  const double target = quantile * (double)histogram->num_samples;
  uint64_t cumulative = 0;
  for (int bucket = 0; bucket < BAI_SCHED_HIST_NUM_BUCKETS; bucket++) {
    cumulative += histogram->counts[bucket];
    if ((double)cumulative >= target) {
      const int64_t upper_bound = (int64_t)exp2(
          (double)(bucket + 1) / (double)BAI_SCHED_HIST_BUCKETS_PER_OCTAVE);
      return upper_bound < histogram->max_ns ? upper_bound : histogram->max_ns;
    }
  }
  return histogram->max_ns;
}

void bai_sched_stats_reset(void) {
  cpthread_mutex_lock(&bai_sched_stats_mutex);
  free(bai_sched_stats_records);
  bai_sched_stats_records = NULL;
  bai_sched_stats_num_records = 0;
  bai_sched_stats_capacity = 0;
  cpthread_mutex_unlock(&bai_sched_stats_mutex);
}

void bai_sched_stats_record(const BAISchedSimStats *sim_stats) {
  cpthread_mutex_lock(&bai_sched_stats_mutex);
  if (bai_sched_stats_num_records == bai_sched_stats_capacity) {
    bai_sched_stats_capacity =
        bai_sched_stats_capacity == 0 ? 64 : bai_sched_stats_capacity * 2;
    bai_sched_stats_records = realloc_or_die(
        bai_sched_stats_records,
        sizeof(BAISchedSimStats) * (size_t)bai_sched_stats_capacity);
  }
  bai_sched_stats_records[bai_sched_stats_num_records++] = *sim_stats;
  cpthread_mutex_unlock(&bai_sched_stats_mutex);
}

int bai_sched_stats_get_count(void) {
  cpthread_mutex_lock(&bai_sched_stats_mutex);
  const int count = bai_sched_stats_num_records;
  cpthread_mutex_unlock(&bai_sched_stats_mutex);
  return count;
}

void bai_sched_stats_get(const int index, BAISchedSimStats *sim_stats) {
  cpthread_mutex_lock(&bai_sched_stats_mutex);
  if (index < 0 || index >= bai_sched_stats_num_records) {
    cpthread_mutex_unlock(&bai_sched_stats_mutex);
    log_fatal("bai sched stats index %d out of range (%d recorded)", index,
              bai_sched_stats_num_records);
    return;
  }
  *sim_stats = bai_sched_stats_records[index];
  cpthread_mutex_unlock(&bai_sched_stats_mutex);
}

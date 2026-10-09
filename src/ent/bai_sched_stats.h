#ifndef BAI_SCHED_STATS_H
#define BAI_SCHED_STATS_H

#include <stdint.h>

// Per-sim scheduling measurements recorded by bai() when MAGPIE is built with
// -DBAI_SCHED_STATS (make BUILD=bai_stats). Without that define bai() records
// nothing, so this module costs nothing on the production path; the collector
// below simply stays empty.
//
// Every *_ns field is wall time in nanoseconds. Fields marked "thread-ns" are
// summed over worker threads, so they compare against wall_ns * num_threads.
typedef struct BAISchedSimStats {
  int num_threads;
  int num_arms;
  int round_size;
  int lag_rounds;
  uint64_t sample_limit;
  // Samples completed in the initial phase and in the BAI phase.
  uint64_t initial_samples;
  uint64_t bai_samples;
  // BAI-phase samples folded into the committed statistics, and those
  // completed but never folded (abandoned after a stop).
  uint64_t folded_samples;
  uint64_t abandoned_samples;
  uint64_t num_rounds_folded;
  // Speculative samples started, and those a round went on to use.
  uint64_t speculations;
  uint64_t speculation_hits;
  // Samples completed after the fold that set a stop status.
  uint64_t overshoot_samples;
  // For each BAI-phase claim: samples completed so far minus samples
  // committed when the claimed round was scheduled.
  uint64_t stale_sum;
  uint64_t stale_max;
  uint64_t stale_count;
  int64_t wall_ns;
  // Thread-ns spent computing samples.
  int64_t busy_ns;
  // Thread-ns spent with nothing to claim while the sim still had work.
  int64_t idle_ns;
  uint64_t idle_events;
  // Thread-ns spent waiting at the initial-phase and avoid-prune barriers.
  int64_t initial_barrier_ns;
  int64_t avoid_prune_barrier_ns;
  // Thread-ns from each worker leaving the sampling loop to the last worker
  // leaving it.
  int64_t tail_ns;
  // Thread-ns from bai() starting to each worker's first claim, and from the
  // last worker leaving the sampling loop to bai() returning.
  int64_t startup_ns;
  int64_t end_ns;
  // Thread-ns spent waiting to acquire the BAI mutex.
  int64_t lock_wait_ns;
  // Time spent folding rounds and scheduling rounds (held under the mutex).
  int64_t fold_ns;
  int64_t schedule_ns;
  // Per-sample cost distribution within this sim.
  int64_t sample_mean_ns;
  int64_t sample_p50_ns;
  int64_t sample_p99_ns;
  int64_t sample_p999_ns;
  int64_t sample_max_ns;
} BAISchedSimStats;

// Quarter-octave histogram of sample costs. Bucket b covers costs whose
// log2 lies in [b / 4, (b + 1) / 4).
enum {
  BAI_SCHED_HIST_BUCKETS_PER_OCTAVE = 4,
  BAI_SCHED_HIST_NUM_BUCKETS = 64 * BAI_SCHED_HIST_BUCKETS_PER_OCTAVE,
};

typedef struct BAISchedHistogram {
  uint64_t counts[BAI_SCHED_HIST_NUM_BUCKETS];
  uint64_t num_samples;
  int64_t total_ns;
  int64_t max_ns;
} BAISchedHistogram;

void bai_sched_histogram_reset(BAISchedHistogram *histogram);
void bai_sched_histogram_add(BAISchedHistogram *histogram, int64_t cost_ns);
void bai_sched_histogram_merge(BAISchedHistogram *dst,
                               const BAISchedHistogram *src);
// Upper bound of the bucket holding the given quantile, in nanoseconds.
int64_t bai_sched_histogram_quantile(const BAISchedHistogram *histogram,
                                     double quantile);

// Process-wide collector. Thread safe.
void bai_sched_stats_reset(void);
void bai_sched_stats_record(const BAISchedSimStats *sim_stats);
int bai_sched_stats_get_count(void);
// Copies the recorded sim at index into sim_stats.
void bai_sched_stats_get(int index, BAISchedSimStats *sim_stats);

#endif

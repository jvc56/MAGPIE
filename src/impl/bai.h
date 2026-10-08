#ifndef BAI_H
#define BAI_H

/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 *   Paper: https://arxiv.org/pdf/2210.00974
 *   Code: https://marcjourdan.netlify.app/publication/baiuv/
 *   (the code was kindly provided by Marc Jourdan)
 *
 * Information-Directed Selection for Top-Two Algorithms
 *   Paper: https://arxiv.org/pdf/2205.12086
 *   Code: https://github.com/zihaophys/topk_colt23
 */

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/bai_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bai_result.h"
#include "../ent/bai_sched_stats.h"
#include "../ent/checkpoint.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "bai_logger.h"
#include "random_variable.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MINIMUM_VARIANCE 1e-10

enum {
  // Sample values are accumulated as integers scaled by
  // 2^BAI_SAMPLE_FIXED_POINT_SHIFT so that the sums do not depend on the order
  // samples complete in, which is what lets a multithreaded sim reproduce a
  // single-threaded one exactly. Integer addition is exactly commutative and
  // associative; double addition is not, so an identical multiset of samples
  // summed in a different order gives last-bit-different means, which can tip
  // the comparison between two near-tied arms.
  BAI_SAMPLE_FIXED_POINT_SHIFT = 30,
  // A single sample is rejected beyond this magnitude, well outside the range
  // of any random variable this is used with, so that the llround below cannot
  // be handed a value outside the range of int64_t.
  BAI_SAMPLE_MAX_MAGNITUDE = 1 << 20,
};

#define BAI_SAMPLE_FIXED_POINT_SCALE                                           \
  ((int64_t)1 << BAI_SAMPLE_FIXED_POINT_SHIFT)

// samples_squared_sum accumulates the quantized value of (value * value), NOT
// the square of the quantized value. Squaring after quantizing would
// accumulate 2^60-scale terms and overflow int64 after only eight samples,
// whereas quantizing the square gives it exactly the same bound as the value
// sum.
//
// For sim utilities, which sim_utility_blend bounds to [0, 1], a quantized
// sample and a quantized square are each at most 2^30, so both accumulators
// stay exact past 8e9 samples (int64 holds about 9.22e18). bai() is generic
// over RandomVariables, though, and the normal RVs the unit tests drive it
// with are unbounded, so nothing here can bound an arbitrary RV's range in
// advance. bai_accumulate_checked therefore fails loudly rather than silently
// wrapping.
static_assert(BAI_SAMPLE_FIXED_POINT_SHIFT < 62,
              "BAI fixed-point scale must leave headroom in int64");

enum {
  // BAI-phase samples are scheduled in rounds. A round's schedule is computed
  // from the arm statistics committed when an earlier round finished, never
  // from whatever has merged at the instant a thread asks for work, so the
  // allocation is the same whichever way the worker threads interleave.
  //
  // Every constant here is fixed: none depends on -threads, which would make
  // a sim's result depend on how many threads ran it, and none depends on the
  // sample budget, so an N-sample sim's schedule is a prefix of a 2N-sample
  // one up to the point where a stop condition fires.
  //
  // BAI_SCHEDULE_ROUND_SIZE is the fold granularity. A fold and a schedule
  // each cost O(num_arms) under the mutex.
  BAI_SCHEDULE_ROUND_SIZE = 32,
  // BAI_SCHEDULE_DEPTH_SAMPLES is how many samples stay scheduled ahead of
  // the round being folded, so that a slow sample holding up the oldest round
  // cannot starve the other threads: they keep claiming from the rounds
  // behind it. It must cover (threads - 1) times the ratio of a straggler's
  // cost to the mean sample cost. The cost is staleness: a round is scheduled
  // from statistics up to this many samples old. Scheduling counts the
  // samples already handed out to each arm, so stale rounds still rotate
  // between arms rather than piling onto one pair.
  BAI_SCHEDULE_DEPTH_SAMPLES = 1024,
  BAI_SCHEDULE_LAG = BAI_SCHEDULE_DEPTH_SAMPLES / BAI_SCHEDULE_ROUND_SIZE,
  // The ring needs one more round than the lag: the slot freed by the round
  // that just folded is the one the newly scheduled round takes.
  BAI_SCHEDULE_ROUNDS = BAI_SCHEDULE_LAG + 1,
};

static_assert(BAI_SCHEDULE_DEPTH_SAMPLES % BAI_SCHEDULE_ROUND_SIZE == 0,
              "BAI schedule depth must be a whole number of rounds");

// Internal BAI structs

typedef struct BAIRound {
  // arm_indices[0, num_samples) is the round's schedule, claimed in order.
  int *arm_indices;
  // The seed reserved for each scheduled slot. Reserving at schedule time
  // rather than letting whichever thread claims first draw the next seed is
  // what makes a round's contents -- and so the statistics its fold commits
  // -- independent of how the threads interleave. Several rounds are live at
  // once and the arms share one seed stream, so a seed taken at claim time
  // would land in whichever round happened to get there first.
  uint64_t *seeds;
  int num_samples;
  int num_claimed;
  int num_completed;
  // Each slot's sample value and, for random variables with deferrable
  // effects, its record (record_size bytes per slot). They are held here
  // until the round is complete and then folded in slot order, so both the
  // committed statistics and the results the records feed are a function of
  // the sample set, never of completion order, and a round that is never
  // folded leaves no trace.
  double *values;
  unsigned char *records;
#ifdef BAI_SCHED_STATS
  uint64_t committed_at_schedule;
#endif
} BAIRound;

typedef struct BAIArmDatum {
  uint64_t num_samples;
  // Samples handed out to this arm so far: the committed num_samples plus
  // those scheduled in rounds not yet folded. Scheduling weighs arms by this
  // count, as if every outstanding sample will return the current mean, so a
  // round scheduled from stale statistics still accounts for the rounds
  // scheduled after them.
  uint64_t num_scheduled;
  int64_t samples_sum;
  int64_t samples_squared_sum;
  double mean;
  double var;
  double *Zs;
} BAIArmDatum;

typedef struct BAISyncData {
  int num_arms;
  int num_arms_reached_threshold;
  uint64_t num_total_samples_completed;
  uint64_t num_total_samples_requested;
  int astar_index;
  bool initial_phase;
  BAIArmDatum *arm_data;
  RandomVariables *rng;
  cpthread_mutex_t mutex;
  ThreadControl *thread_control;
  BAIResult *bai_result;
  // Non-owning pointer into bai_options->arm_avoid_prune.
  // bai() modifies this array in-place (swap-and-shrink); guarded by mutex.
  int *avoid_prune_arms;
  int avoid_prune_count;    // remaining arms still needing top-up
  int avoid_prune_next_idx; // round-robin cursor
  int avoid_prune_best_arm_idx;
  // BAI-phase round schedule. Rounds [next_round_to_fold,
  // next_round_to_schedule) are live; rounds are claimed in order from
  // next_round_to_claim and folded in order from next_round_to_fold.
  BAIRound rounds[BAI_SCHEDULE_ROUNDS];
  int round_size;
  uint64_t next_round_to_claim;
  uint64_t next_round_to_fold;
  uint64_t next_round_to_schedule;
  // True once no further round will be scheduled: the round just scheduled
  // spent the last of the budget, or the sim stopped. A worker that finds
  // nothing to claim exits once this is set, rather than waiting for folds
  // that can only drain rounds already handed out.
  bool scheduling_finished;
  // True once the results are final: a fold raised a stop condition, or a
  // worker abandoned the rounds on a timeout or user interrupt. Nothing more
  // is claimed or folded, and samples still in flight are discarded, so the
  // results hold exactly the rounds folded before the stop.
  bool stopped;
  size_t record_size;
  // Signaled whenever a fold may have made new work claimable, when claims
  // become exhausted, and when a worker abandons the rounds. A worker with
  // nothing to claim waits on it instead of spinning on the mutex.
  cpthread_cond_t work_available;
#ifdef BAI_SCHED_STATS
  BAISchedSimStats sched_stats;
  BAISchedHistogram sched_histogram;
  uint64_t sched_bai_completions;
  bool sched_stop_folded;
  int64_t sched_last_exit_ns;
  int64_t sched_exit_ns_sum;
  int sched_num_exits;
#endif
} BAISyncData;

// Scheduling instrumentation. With BAI_SCHED_STATS undefined every hook below
// is an empty inline function and the per-thread struct is a single unused
// byte, so the production build compiles all of it away.
#ifdef BAI_SCHED_STATS
typedef struct BAIWorkerSchedStats {
  int64_t busy_ns;
  int64_t idle_ns;
  int64_t idle_start_ns;
  uint64_t idle_episodes;
  int64_t lock_wait_ns;
  int64_t initial_barrier_ns;
  int64_t avoid_prune_barrier_ns;
  BAISchedHistogram histogram;
} BAIWorkerSchedStats;

static inline int64_t bai_sched_now(void) { return ctimer_monotonic_ns(); }

static inline void bai_sched_worker_init(BAIWorkerSchedStats *worker_stats) {
  memset(worker_stats, 0, sizeof(*worker_stats));
}

static inline void bai_sched_sample_done(BAIWorkerSchedStats *worker_stats,
                                         const int64_t start_ns) {
  const int64_t cost_ns = ctimer_monotonic_ns() - start_ns;
  worker_stats->busy_ns += cost_ns;
  bai_sched_histogram_add(&worker_stats->histogram, cost_ns);
}

static inline void bai_sched_idle_begin(BAIWorkerSchedStats *worker_stats) {
  if (worker_stats->idle_start_ns == 0) {
    worker_stats->idle_start_ns = ctimer_monotonic_ns();
    worker_stats->idle_episodes++;
  }
}

static inline void bai_sched_idle_end(BAIWorkerSchedStats *worker_stats) {
  if (worker_stats->idle_start_ns != 0) {
    worker_stats->idle_ns +=
        ctimer_monotonic_ns() - worker_stats->idle_start_ns;
    worker_stats->idle_start_ns = 0;
  }
}
#else
typedef struct BAIWorkerSchedStats {
  char unused;
} BAIWorkerSchedStats;

static inline int64_t bai_sched_now(void) { return 0; }

static inline void bai_sched_worker_init(BAIWorkerSchedStats
                                         __attribute__((unused)) *
                                         worker_stats) {}

static inline void bai_sched_sample_done(BAIWorkerSchedStats
                                             __attribute__((unused)) *
                                             worker_stats,
                                         const int64_t
                                         __attribute__((unused)) start_ns) {}

static inline void bai_sched_idle_begin(BAIWorkerSchedStats
                                        __attribute__((unused)) *
                                        worker_stats) {}

static inline void
bai_sched_idle_end(BAIWorkerSchedStats __attribute__((unused)) * worker_stats) {
}
#endif

// Locks the BAI mutex, charging the wait to the worker when instrumented.
static inline void bai_sync_lock(BAISyncData *bai_sync_data,
                                 BAIWorkerSchedStats *worker_stats) {
#ifdef BAI_SCHED_STATS
  const int64_t start_ns = ctimer_monotonic_ns();
  cpthread_mutex_lock(&bai_sync_data->mutex);
  // A worker that is already idle is charged to idle time, not lock time.
  if (worker_stats->idle_start_ns == 0) {
    worker_stats->lock_wait_ns += ctimer_monotonic_ns() - start_ns;
  }
#else
  (void)worker_stats;
  cpthread_mutex_lock(&bai_sync_data->mutex);
#endif
}

static inline BAISyncData *bai_sync_data_create(BAIResult *bai_result,
                                                ThreadControl *thread_control,
                                                const int num_initial_arms,
                                                RandomVariables *rng) {
  BAISyncData *bai_sync_data = malloc_or_die(sizeof(BAISyncData));
  bai_sync_data->num_arms = num_initial_arms;
  bai_sync_data->num_arms_reached_threshold = 0;
  bai_sync_data->num_total_samples_completed = 0;
  bai_sync_data->num_total_samples_requested = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->avoid_prune_best_arm_idx = -1;
  bai_sync_data->initial_phase = true;
  bai_sync_data->arm_data =
      calloc_or_die(num_initial_arms, sizeof(BAIArmDatum));
  for (int i = 0; i < num_initial_arms; i++) {
    bai_sync_data->arm_data[i].Zs =
        malloc_or_die(num_initial_arms * sizeof(double));
  }
  bai_sync_data->rng = rng;
  cpthread_mutex_init(&bai_sync_data->mutex);
  cpthread_cond_init(&bai_sync_data->work_available);
  bai_sync_data->thread_control = thread_control;
  bai_sync_data->bai_result = bai_result;
  bai_sync_data->avoid_prune_arms = NULL;
  bai_sync_data->avoid_prune_count = 0;
  bai_sync_data->avoid_prune_next_idx = 0;
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    bai_sync_data->rounds[i].arm_indices = NULL;
    bai_sync_data->rounds[i].seeds = NULL;
    bai_sync_data->rounds[i].values = NULL;
    bai_sync_data->rounds[i].records = NULL;
    bai_sync_data->rounds[i].num_samples = 0;
    bai_sync_data->rounds[i].num_claimed = 0;
    bai_sync_data->rounds[i].num_completed = 0;
  }
  bai_sync_data->round_size = 0;
  bai_sync_data->next_round_to_claim = 0;
  bai_sync_data->next_round_to_fold = 0;
  bai_sync_data->next_round_to_schedule = 0;
  // Nothing is scheduled until the initial phase hands over, so a BAI-phase
  // worker that starts after an initial-phase stop exits immediately.
  bai_sync_data->scheduling_finished = true;
  bai_sync_data->stopped = false;
  bai_sync_data->record_size = 0;
#ifdef BAI_SCHED_STATS
  memset(&bai_sync_data->sched_stats, 0, sizeof(bai_sync_data->sched_stats));
  bai_sched_histogram_reset(&bai_sync_data->sched_histogram);
  bai_sync_data->sched_bai_completions = 0;
  bai_sync_data->sched_stop_folded = false;
  bai_sync_data->sched_last_exit_ns = 0;
  bai_sync_data->sched_exit_ns_sum = 0;
  bai_sync_data->sched_num_exits = 0;
#endif
  return bai_sync_data;
}

static inline void bai_sync_data_destroy(BAISyncData *bai_sync_data) {
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    free(bai_sync_data->arm_data[i].Zs);
  }
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    free(bai_sync_data->rounds[i].arm_indices);
    free(bai_sync_data->rounds[i].seeds);
    free(bai_sync_data->rounds[i].values);
    free(bai_sync_data->rounds[i].records);
  }
  free(bai_sync_data->arm_data);
  free(bai_sync_data);
}

typedef struct BAISampleArgs {
  BAISyncData *bai_sync_data;
  RandomVariables *rvs;
  double delta;
  uint64_t sample_limit;
  uint64_t sample_minimum;
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  double cutoff;
  // Per-thread batch state for the initial phase. When batch_remaining == 0
  // the worker atomically reserves num_arms consecutive sample indices from
  // the global counter, forming a batch of (iter N, arms 0..num_arms-1)
  // processed contiguously on this thread. Each arm in iter N draws its
  // pre-candidate opp rack from the same iter-local PRNG state, so racks
  // repeat across arms within a batch and the RIT cache hits. This matters
  // in the all-initial-phase regime (short MCTS-style sims with no BAI
  // pruning); it's neutral in normal BAI-phase-dominated sims.
  uint64_t initial_batch_next_total_index;
  int initial_batch_remaining;
} BAISampleArgs;

// Quantizes a sample value into the fixed-point integer domain the arm
// accumulators use. Does not clamp to [0, 1]: sim samples are utilities in
// that range, but bai() is generic over RandomVariables and the normal RVs
// the unit tests drive it with are unbounded.
static inline int64_t bai_quantize_sample(const double value) {
  if (!(value >= -(double)BAI_SAMPLE_MAX_MAGNITUDE &&
        value <= (double)BAI_SAMPLE_MAX_MAGNITUDE)) {
    log_fatal("BAI sample value out of range: %f", value);
  }
  return llround(value * (double)BAI_SAMPLE_FIXED_POINT_SCALE);
}

// Adds to a fixed-point accumulator, failing loudly rather than silently
// wrapping. See the overflow bound documented with
// BAI_SAMPLE_FIXED_POINT_SHIFT.
static inline void bai_accumulate_checked(int64_t *accumulator,
                                          const int64_t addend) {
  if ((addend > 0 && *accumulator > INT64_MAX - addend) ||
      (addend < 0 && *accumulator < INT64_MIN - addend)) {
    log_fatal("BAI sample accumulator overflowed");
  }
  *accumulator += addend;
}

// Derives mean and variance from the exact integer accumulators. The
// floating-point work here runs on exact integer inputs in a fixed operation
// order, so it is bit-reproducible for a given set of samples regardless of
// the order they were added in.
static inline void
bai_arm_datum_recompute_mean_and_var(BAIArmDatum *arm_datum) {
  const double inverse_scale = 1.0 / (double)BAI_SAMPLE_FIXED_POINT_SCALE;
  const double num_samples = (double)arm_datum->num_samples;
  arm_datum->mean =
      (double)arm_datum->samples_sum * inverse_scale / num_samples;
  arm_datum->var =
      (double)arm_datum->samples_squared_sum * inverse_scale / num_samples -
      arm_datum->mean * arm_datum->mean;
  if (arm_datum->var < MINIMUM_VARIANCE) {
    arm_datum->var = MINIMUM_VARIANCE;
  }
}

static inline double bai_alt_lambda(const double mu1, const double sigma21,
                                    const double w1, const double mua,
                                    const double sigma2a, const double wa) {
  if (w1 == 0) {
    return mua;
  }
  if (wa == 0 || mu1 == mua) {
    return mu1;
  }
  const double x = wa / w1;
  const double result =
      (sigma2a * mu1 + x * sigma21 * mua) / (sigma2a + x * sigma21);
  return result;
}

static inline double bai_d(const double mu, const double sigma2,
                           const double lambda) {
  const double diff = mu - lambda;
  return 0.5 * (diff * diff) / sigma2;
}

// The GLR statistic for "astar beats challenger", with the two arms weighted
// by the given sample counts.
static inline double
bai_arm_z_with_counts(const BAIArmDatum *astar_arm_data,
                      const double astar_num_samples,
                      const BAIArmDatum *challenger_arm_data,
                      const double challenger_num_samples) {
  const double alt_lambda =
      bai_alt_lambda(astar_arm_data->mean, astar_arm_data->var,
                     astar_num_samples, challenger_arm_data->mean,
                     challenger_arm_data->var, challenger_num_samples);
  const double d_astar =
      bai_d(astar_arm_data->mean, astar_arm_data->var, alt_lambda);
  const double d_a =
      bai_d(challenger_arm_data->mean, challenger_arm_data->var, alt_lambda);
  return astar_num_samples * d_astar + challenger_num_samples * d_a;
}

static inline double bai_get_arm_z(BAISyncData *bai_sync_data,
                                   const int astar_index,
                                   const int challenger_index) {
  const BAIArmDatum *astar_arm_data = &bai_sync_data->arm_data[astar_index];
  const BAIArmDatum *challenger_arm_data =
      &bai_sync_data->arm_data[challenger_index];
  return bai_arm_z_with_counts(
      astar_arm_data, (double)astar_arm_data->num_samples, challenger_arm_data,
      (double)challenger_arm_data->num_samples);
}

static inline int
bai_sync_data_sample_limit_reached(const BAISyncData *bai_sync_data,
                                   uint64_t sample_limit) {
  return bai_sync_data->num_total_samples_requested >= sample_limit;
}

static inline int
bai_sync_data_get_next_initial_sample_index_while_locked(BAISampleArgs *args) {
  const int num_arms = args->bai_sync_data->num_arms;
  const uint64_t initial_limit = (uint64_t)num_arms * args->sample_minimum;
  if (args->initial_batch_remaining == 0) {
    if (args->bai_sync_data->num_total_samples_requested >= initial_limit) {
      return -1;
    }
    args->initial_batch_next_total_index =
        args->bai_sync_data->num_total_samples_requested;
    args->bai_sync_data->num_total_samples_requested += (uint64_t)num_arms;
    args->initial_batch_remaining = num_arms;
  }
  const uint64_t total_index = args->initial_batch_next_total_index++;
  args->initial_batch_remaining--;
  if (total_index >= initial_limit) {
    args->initial_batch_remaining = 0;
    return -1;
  }
  return (int)(total_index % (uint64_t)num_arms);
}

static inline int
bai_sync_data_get_next_initial_sample_index(BAISampleArgs *args,
                                            BAIWorkerSchedStats *worker_stats) {
  bai_sync_lock(args->bai_sync_data, worker_stats);
  const int arm_index =
      bai_sync_data_get_next_initial_sample_index_while_locked(args);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
  return arm_index;
}

// Assumes the caller has locked bai_sync_data or is the only thread running
// Checks the stopping threshold against the committed statistics. The
// challenger is not chosen here: scheduling picks it when it lays out a round,
// from the counts handed out by then (see bai_schedule_challenger).
static inline void bai_update_threshold(BAISyncData *bai_sync_data,
                                        RandomVariables *rvs,
                                        bai_threshold_t threshold,
                                        const double delta, const int arm_index,
                                        const bool update_all) {
  if (threshold == BAI_THRESHOLD_NONE) {
    return;
  }

  BAIArmDatum *astar_arm_datum =
      &bai_sync_data->arm_data[bai_sync_data->astar_index];
  int num_arms_at_threshold = 0;

  double bai_threshold;
  switch (threshold) {
  case BAI_THRESHOLD_NONE:
    break;
  case BAI_THRESHOLD_GK16:
    bai_threshold = log(
        (log((double)bai_sync_data->num_total_samples_completed) + 1) / delta);
    break;
  }

  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    if (i == bai_sync_data->astar_index ||
        rvs_are_similar(rvs, bai_sync_data->astar_index, i)) {
      num_arms_at_threshold++;
      astar_arm_datum->Zs[i] = INFINITY;
      continue;
    }
    if (update_all || i == arm_index) {
      astar_arm_datum->Zs[i] =
          bai_get_arm_z(bai_sync_data, bai_sync_data->astar_index, i);
    }
    double arm_Z = astar_arm_datum->Zs[i];
    switch (threshold) {
    case BAI_THRESHOLD_NONE:
      break;
    case BAI_THRESHOLD_GK16:
      if (arm_Z > bai_threshold) {
        num_arms_at_threshold++;
      }
      break;
    }
  }
  if (!bai_sync_data->initial_phase &&
      num_arms_at_threshold == bai_sync_data->num_arms) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_THRESHOLD);
  }
}

// Assumes the caller has locked the bai sync data mutex
static inline void
bai_sync_data_add_sample_while_locked(BAISampleArgs *args, const int arm_index,
                                      const double sample_value) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  BAIArmDatum *arm_data = bai_sync_data->arm_data;
  bai_sync_data->num_total_samples_completed++;
  BAIArmDatum *sample_arm_datum = &arm_data[arm_index];
  sample_arm_datum->num_samples++;
  bai_accumulate_checked(&sample_arm_datum->samples_sum,
                         bai_quantize_sample(sample_value));
  bai_accumulate_checked(&sample_arm_datum->samples_squared_sum,
                         bai_quantize_sample(sample_value * sample_value));
  bai_arm_datum_recompute_mean_and_var(sample_arm_datum);

  const int old_astar_index = bai_sync_data->astar_index;
  bai_sync_data->astar_index = 0;
  double astar_mean = arm_data[0].mean;
  for (int i = 1; i < bai_sync_data->num_arms; i++) {
    const BAIArmDatum *arm_datum = &arm_data[i];
    if (arm_datum->mean > astar_mean) {
      bai_sync_data->astar_index = i;
      astar_mean = arm_datum->mean;
    }
  }

  if (!bai_sync_data->initial_phase &&
      is_win_pct_within_cutoff(astar_mean, args->cutoff)) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
  } else {
    const bool update_all = old_astar_index == -1 ||
                            old_astar_index != bai_sync_data->astar_index ||
                            bai_sync_data->astar_index == arm_index;

    bai_update_threshold(bai_sync_data, args->rvs, args->threshold, args->delta,
                         arm_index, update_all);
  }
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. The top-two challenger: the arm minimizing Z + log(N)
// against astar, as in the sequential rule, but with every arm weighted by the
// samples handed out to it (num_scheduled) rather than only those committed.
// Returns astar when every other arm is similar to it.
static inline int bai_schedule_challenger(const BAISyncData *bai_sync_data,
                                          RandomVariables *rvs,
                                          const int astar_index) {
  const BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[astar_index];
  const double astar_num_scheduled = (double)astar_arm_datum->num_scheduled;
  int challenger_index = -1;
  double challenger_value = 0.0;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == astar_index ||
        rvs_are_similar(rvs, astar_index, arm_index)) {
      continue;
    }
    const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    const double num_scheduled = (double)arm_datum->num_scheduled;
    const double value =
        bai_arm_z_with_counts(astar_arm_datum, astar_num_scheduled, arm_datum,
                              num_scheduled) +
        log(num_scheduled);
    if (challenger_index < 0 || value < challenger_value) {
      challenger_index = arm_index;
      challenger_value = value;
    }
  }
  return challenger_index < 0 ? astar_index : challenger_index;
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. Lays out one round's worth of arm indices from the
// committed statistics and charges them to the sample budget.
static inline void bai_schedule_round_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
#ifdef BAI_SCHED_STATS
  const int64_t sched_start_ns = bai_sched_now();
#endif
  if (bai_sync_data->scheduling_finished) {
    return;
  }
  const uint64_t requested = bai_sync_data->num_total_samples_requested;
  if (requested >= args->sample_limit) {
    bai_sync_data->scheduling_finished = true;
    return;
  }
  const uint64_t available = args->sample_limit - requested;
  int num_samples = bai_sync_data->round_size;
  if ((uint64_t)num_samples > available) {
    num_samples = (int)available;
  }
  BAIRound *round =
      &bai_sync_data->rounds[bai_sync_data->next_round_to_schedule %
                             BAI_SCHEDULE_ROUNDS];
  round->num_samples = num_samples;
  round->num_claimed = 0;
  round->num_completed = 0;
  switch (args->sampling_rule) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    for (int slot = 0; slot < num_samples; slot++) {
      round->arm_indices[slot] = (int)((requested + (uint64_t)slot) %
                                       (uint64_t)bai_sync_data->num_arms);
    }
    break;
  case BAI_SAMPLING_RULE_TOP_TWO_IDS:;
    const int astar_arm_index = bai_sync_data->astar_index;
    const int challenger_arm_index =
        bai_schedule_challenger(bai_sync_data, args->rvs, astar_arm_index);
    const BAIArmDatum *astar_arm_datum =
        &bai_sync_data->arm_data[astar_arm_index];
    const BAIArmDatum *challenger_arm_datum =
        &bai_sync_data->arm_data[challenger_arm_index];
    const double astar_num_samples = (double)astar_arm_datum->num_scheduled;
    const double challenger_num_samples =
        (double)challenger_arm_datum->num_scheduled;
    const double theta_bar =
        (astar_num_samples * astar_arm_datum->mean +
         challenger_num_samples * challenger_arm_datum->mean) /
        (astar_num_samples + challenger_num_samples);
    const double numerator =
        astar_num_samples *
        bai_d(astar_arm_datum->mean, astar_arm_datum->var, theta_bar);
    const double denominator =
        numerator + challenger_num_samples * bai_d(challenger_arm_datum->mean,
                                                   challenger_arm_datum->var,
                                                   theta_bar);
    // This replaces a per-sample coin flip against numerator/denominator with
    // the stratified version of the same rule: allocate the round between the
    // two arms by rounding that proportion. It matches the coin's mean exactly
    // with zero variance, so it is statistically no worse, and it removes the
    // shared rng stream from the BAI phase entirely. Equal means make both
    // terms zero; the coin fell through to the challenger there, so a
    // degenerate proportion allocates the round to the challenger too.
    double astar_share = 0.0;
    if (denominator > 0.0) {
      astar_share = numerator / denominator;
      if (!(astar_share > 0.0)) {
        astar_share = 0.0;
      } else if (astar_share > 1.0) {
        astar_share = 1.0;
      }
    }
    int astar_count = (int)llround(astar_share * (double)num_samples);
    if (astar_count < 0) {
      astar_count = 0;
    } else if (astar_count > num_samples) {
      astar_count = num_samples;
    }
    for (int slot = 0; slot < num_samples; slot++) {
      round->arm_indices[slot] =
          (slot < astar_count) ? astar_arm_index : challenger_arm_index;
    }
    break;
  }
  for (int slot = 0; slot < num_samples; slot++) {
    const int arm_index = round->arm_indices[slot];
    round->seeds[slot] = rvs_next_seed(args->rvs, (uint64_t)arm_index);
    bai_sync_data->arm_data[arm_index].num_scheduled++;
  }
  bai_sync_data->num_total_samples_requested += (uint64_t)num_samples;
  bai_sync_data->next_round_to_schedule++;
  if (bai_sync_data->num_total_samples_requested >= args->sample_limit) {
    // This round spends the last of the budget. The sample-limit status is
    // recorded when it folds, but no worker needs to wait for that.
    bai_sync_data->scheduling_finished = true;
  }
#ifdef BAI_SCHED_STATS
  round->committed_at_schedule = bai_sync_data->num_total_samples_completed;
  bai_sync_data->sched_stats.schedule_ns += bai_sched_now() - sched_start_ns;
#endif
}

// Assumes the caller has locked the bai sync data mutex. Hands out the next
// unclaimed slot of the oldest round that still has one, or -1 when every
// scheduled slot is taken or the sim has stopped. Threads pull greedily
// across round boundaries, so a slow sample in one round does not keep the
// others from working on the rounds behind it.
static inline int bai_schedule_claim_while_locked(BAISyncData *bai_sync_data,
                                                  uint64_t *round_number,
                                                  int *slot, uint64_t *seed) {
  if (bai_sync_data->stopped) {
    return -1;
  }
  while (bai_sync_data->next_round_to_claim <
         bai_sync_data->next_round_to_schedule) {
    BAIRound *round =
        &bai_sync_data
             ->rounds[bai_sync_data->next_round_to_claim % BAI_SCHEDULE_ROUNDS];
    if (round->num_claimed < round->num_samples) {
      *round_number = bai_sync_data->next_round_to_claim;
      *slot = round->num_claimed;
      const int arm_index = round->arm_indices[round->num_claimed];
      *seed = round->seeds[round->num_claimed];
      round->num_claimed++;
#ifdef BAI_SCHED_STATS
      const uint64_t completed = bai_sync_data->sched_stats.initial_samples +
                                 bai_sync_data->sched_bai_completions;
      const uint64_t stale = completed > round->committed_at_schedule
                                 ? completed - round->committed_at_schedule
                                 : 0;
      bai_sync_data->sched_stats.stale_sum += stale;
      bai_sync_data->sched_stats.stale_count++;
      if (stale > bai_sync_data->sched_stats.stale_max) {
        bai_sync_data->sched_stats.stale_max = stale;
      }
#endif
      return arm_index;
    }
    bai_sync_data->next_round_to_claim++;
  }
  return -1;
}

// Assumes the caller has locked the bai sync data mutex. Folds one finished
// round into the committed statistics and re-derives everything the next
// schedule reads from them.
static inline void bai_commit_round_while_locked_impl(BAISampleArgs *args,
                                                      BAIRound *round) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  for (int slot = 0; slot < round->num_samples; slot++) {
    const int arm_index = round->arm_indices[slot];
    const double sample_value = round->values[slot];
    BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    arm_datum->num_samples++;
    bai_accumulate_checked(&arm_datum->samples_sum,
                           bai_quantize_sample(sample_value));
    bai_accumulate_checked(&arm_datum->samples_squared_sum,
                           bai_quantize_sample(sample_value * sample_value));
    if (round->records != NULL) {
      rvs_apply_sample_record(args->rvs, (uint64_t)arm_index,
                              round->records +
                                  (size_t)slot * bai_sync_data->record_size);
    }
  }
  for (int slot = 0; slot < round->num_samples; slot++) {
    bai_arm_datum_recompute_mean_and_var(
        &bai_sync_data->arm_data[round->arm_indices[slot]]);
  }
  bai_sync_data->num_total_samples_completed += (uint64_t)round->num_samples;

  bai_sync_data->astar_index = 0;
  double astar_mean = bai_sync_data->arm_data[0].mean;
  for (int arm_index = 1; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (bai_sync_data->arm_data[arm_index].mean > astar_mean) {
      bai_sync_data->astar_index = arm_index;
      astar_mean = bai_sync_data->arm_data[arm_index].mean;
    }
  }
  if (is_win_pct_within_cutoff(astar_mean, args->cutoff)) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
    return;
  }
  bai_update_threshold(bai_sync_data, args->rvs, args->threshold, args->delta,
                       bai_sync_data->astar_index, true);
}

static inline void bai_commit_round_while_locked(BAISampleArgs *args,
                                                 BAIRound *round) {
#ifdef BAI_SCHED_STATS
  BAISyncData *bai_sync_data = args->bai_sync_data;
  const int64_t fold_start_ns = bai_sched_now();
  const bool stopped_before =
      bai_result_get_status(bai_sync_data->bai_result) !=
      BAI_RESULT_STATUS_NONE;
  bai_commit_round_while_locked_impl(args, round);
  bai_sync_data->sched_stats.fold_ns += bai_sched_now() - fold_start_ns;
  bai_sync_data->sched_stats.folded_samples += (uint64_t)round->num_samples;
  bai_sync_data->sched_stats.num_rounds_folded++;
  if (!stopped_before && bai_result_get_status(bai_sync_data->bai_result) !=
                             BAI_RESULT_STATUS_NONE) {
    bai_sync_data->sched_stop_folded = true;
  }
#else
  bai_commit_round_while_locked_impl(args, round);
#endif
}

// Assumes the caller has locked the bai sync data mutex. Records one finished
// sample in its round's slot (its record, if any, was written in place), then
// folds every round that is complete, oldest first, scheduling a replacement
// for each one folded. A sample that finishes after the sim stopped is
// discarded.
static inline void
bai_schedule_complete_while_locked(BAISampleArgs *args,
                                   const uint64_t round_number, const int slot,
                                   const double sample_value) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
#ifdef BAI_SCHED_STATS
  bai_sync_data->sched_bai_completions++;
  if (bai_sync_data->stopped) {
    bai_sync_data->sched_stats.overshoot_samples++;
  }
#endif
  if (bai_sync_data->stopped) {
    return;
  }
  BAIRound *round = &bai_sync_data->rounds[round_number % BAI_SCHEDULE_ROUNDS];
  round->values[slot] = sample_value;
  round->num_completed++;
  bool folded = false;
  // Fold in round order. A round whose samples are all in but which is not
  // the oldest unfolded one waits, because folding out of order would make
  // the committed statistics depend on completion order again. No thread
  // waits for that: it simply does not do the fold and goes back to claiming.
  while (bai_sync_data->next_round_to_fold <
         bai_sync_data->next_round_to_schedule) {
    BAIRound *oldest =
        &bai_sync_data
             ->rounds[bai_sync_data->next_round_to_fold % BAI_SCHEDULE_ROUNDS];
    if (oldest->num_completed < oldest->num_samples) {
      break;
    }
    bai_commit_round_while_locked(args, oldest);
    bai_sync_data->next_round_to_fold++;
    folded = true;
    if (bai_result_get_status(bai_sync_data->bai_result) !=
        BAI_RESULT_STATUS_NONE) {
      // A stop condition: the results are exactly the rounds folded so far.
      // Rounds already handed out are abandoned rather than drained.
      bai_sync_data->stopped = true;
      bai_sync_data->scheduling_finished = true;
      break;
    }
    bai_schedule_round_while_locked(args);
  }
  if (!bai_sync_data->stopped && bai_sync_data->scheduling_finished &&
      bai_sync_data->next_round_to_fold ==
          bai_sync_data->next_round_to_schedule) {
    // Every scheduled round has folded and the budget is spent.
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_SAMPLE_LIMIT);
    bai_sync_data->stopped = true;
  }
  if (folded) {
    cpthread_cond_broadcast(&bai_sync_data->work_available);
  }
}

// Sizes the rounds and fills the pipeline. Runs once, single-threaded, at the
// checkpoint between the initial and BAI phases.
static inline void bai_sync_data_start_schedule(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  const uint64_t requested = bai_sync_data->num_total_samples_requested;
  if (requested >= args->sample_limit) {
    return;
  }
  const size_t round_size = BAI_SCHEDULE_ROUND_SIZE;
  bai_sync_data->round_size = (int)round_size;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    bai_sync_data->arm_data[arm_index].num_scheduled =
        bai_sync_data->arm_data[arm_index].num_samples;
  }
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    bai_sync_data->rounds[i].arm_indices =
        malloc_or_die(sizeof(int) * round_size);
    bai_sync_data->rounds[i].seeds =
        malloc_or_die(sizeof(uint64_t) * round_size);
    bai_sync_data->rounds[i].values =
        malloc_or_die(sizeof(double) * round_size);
    if (bai_sync_data->record_size > 0) {
      bai_sync_data->rounds[i].records =
          malloc_or_die(bai_sync_data->record_size * round_size);
    }
  }
  bai_sync_data->next_round_to_claim = 0;
  bai_sync_data->next_round_to_fold = 0;
  bai_sync_data->next_round_to_schedule = 0;
  bai_sync_data->scheduling_finished = false;
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    bai_schedule_round_while_locked(args);
  }
}

static inline void bai_sync_data_add_sample(BAISampleArgs *args,
                                            const int arm_index,
                                            const double sample_value,
                                            BAIWorkerSchedStats *worker_stats) {
  bai_sync_lock(args->bai_sync_data, worker_stats);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
}

typedef struct BAIWorkerArgs {
  BAISyncData *sync_data;
  RandomVariables *rvs;
  const BAIOptions *bai_options;
  BAILogger *bai_logger;
  Checkpoint *checkpoint;
  Checkpoint *avoid_prune_checkpoint;
  int thread_index;
} BAIWorkerArgs;

// Builds the per-sample argument block from the options. Used by the worker
// loops and by the handover between phases, which both need the same view.
static inline BAISampleArgs
bai_sample_args_create(BAISyncData *sync_data, RandomVariables *rvs,
                       const BAIOptions *bai_options) {
  BAISampleArgs sample_args = {
      .bai_sync_data = sync_data,
      .rvs = rvs,
      .delta = bai_options->delta,
      .sample_limit = bai_options->sample_limit,
      .sample_minimum = bai_options->sample_minimum,
      .sampling_rule = bai_options->sampling_rule,
      .threshold = bai_options->threshold,
      .cutoff = bai_options->cutoff,
      .initial_batch_next_total_index = 0,
      .initial_batch_remaining = 0,
  };
  return sample_args;
}

static inline bool bai_should_stop(BAIResult *bai_result,
                                   ThreadControl *thread_control) {
  return bai_result_set_and_get_status(
             bai_result, thread_control_get_status(thread_control) ==
                             THREAD_CONTROL_STATUS_USER_INTERRUPT) !=
         BAI_RESULT_STATUS_NONE;
}

static inline void bai_avoid_prune_prebroadcast(void *data) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)data;
  BAISyncData *sync_data = bai_worker_args->sync_data;
  sync_data->avoid_prune_best_arm_idx =
      rvs_get_best_arm_index(bai_worker_args->rvs);
}

static inline void bai_finish_initial_phase(void *uncasted_bai_worker_args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)uncasted_bai_worker_args;
  BAISyncData *bai_sync_data = bai_worker_args->sync_data;
  bai_sync_data->initial_phase = false;
#ifdef BAI_SCHED_STATS
  bai_sync_data->sched_stats.initial_samples =
      bai_sync_data->num_total_samples_completed;
#endif
  if (bai_should_stop(bai_sync_data->bai_result,
                      bai_worker_args->sync_data->thread_control)) {
    return;
  }
  assert(bai_sync_data->astar_index != -1);
  if (bai_sync_data_sample_limit_reached(
          bai_sync_data, bai_worker_args->bai_options->sample_limit)) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_SAMPLE_LIMIT);
    return;
  }
  if (is_win_pct_within_cutoff(
          bai_sync_data->arm_data[bai_sync_data->astar_index].mean,
          bai_worker_args->bai_options->cutoff)) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
    return;
  }
  bai_update_threshold(bai_sync_data, bai_worker_args->rvs,
                       bai_worker_args->bai_options->threshold,
                       bai_worker_args->bai_options->delta,
                       bai_sync_data->astar_index, true);
  // Fill the round pipeline from the statistics the initial phase produced.
  // Runs here, single-threaded at the checkpoint, so the first schedules are
  // computed before any BAI-phase worker can claim.
  BAISampleArgs sample_args = bai_sample_args_create(
      bai_sync_data, bai_worker_args->rvs, bai_worker_args->bai_options);
  bai_sync_data_start_schedule(&sample_args);
}

// Selects the next arm to sample from the avoid-prune list. Modifies the
// BAIArmDatum for the selected arm by incrementing its num_samples field,
// which tracks the number of samples requested for that arm.
static inline int get_avoid_prune_next_idx(BAISyncData *sync_data,
                                           const uint64_t winner_count) {
  cpthread_mutex_lock(&sync_data->mutex);
  int result = -1;
  while (sync_data->avoid_prune_count > 0) {
    const int idx =
        sync_data->avoid_prune_next_idx % sync_data->avoid_prune_count;
    const int arm_index = sync_data->avoid_prune_arms[idx];
    if (sync_data->arm_data[arm_index].num_samples >= winner_count) {
      // Arm is done: swap with last and shrink
      sync_data->avoid_prune_arms[idx] =
          sync_data->avoid_prune_arms[sync_data->avoid_prune_count - 1];
      sync_data->avoid_prune_count--;
      // Do not advance next_idx; re-examine the swapped-in arm
    } else {
      sync_data->avoid_prune_next_idx++;
      result = arm_index;
      sync_data->arm_data[arm_index].num_samples++;
      break;
    }
  }
  cpthread_mutex_unlock(&sync_data->mutex);
  return result;
}

static inline void sim_unpruned_to_winner(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int rvs_thread_index =
      bai_options->parent_worker_thread_index + bai_worker_args->thread_index;
  const uint64_t winner_count =
      sync_data->arm_data[sync_data->avoid_prune_best_arm_idx].num_samples;
  while (thread_control_get_status(sync_data->thread_control) !=
         THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    const int arm_index = get_avoid_prune_next_idx(sync_data, winner_count);
    if (arm_index < 0) {
      break;
    }
    rvs_sample(rvs, (uint64_t)arm_index, rvs_thread_index, NULL);
  }
}

// Returns the rvs thread index for this worker, and fails loudly on the index
// confusion that would make two threads share one movegen slot.
static inline int
bai_worker_rvs_thread_index(const BAIWorkerArgs *bai_worker_args) {
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  const int bai_thread_index = bai_worker_args->thread_index;
  if (bai_thread_index > 0 && bai_options->parent_worker_thread_index > 0) {
    log_fatal("Both BAI worker thread index (%d) and parent worker "
              "thread index (%d) are greater than 0.",
              bai_thread_index, bai_options->parent_worker_thread_index);
  }
  // In IGP mode, parent_worker_thread_index == 0 and bai_thread_index
  // distinguishes concurrent BAI threads. In PGP mode, bai_thread_index == 0
  // and parent_worker_thread_index distinguishes concurrent autoplay workers.
  // Using the wrong index causes multiple threads to share cached_gens[0].
  return bai_options->parent_worker_thread_index + bai_thread_index;
}

// The initial phase allocates a fixed number of samples to every arm, so its
// samples merge straight into the committed statistics.
static inline void
bai_worker_initial_sample_loop(BAIWorkerArgs *bai_worker_args,
                               BAIWorkerSchedStats *worker_stats) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = sync_data->thread_control;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int rvs_thread_index = bai_worker_rvs_thread_index(bai_worker_args);

  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, bai_worker_args->bai_options);

  while (!bai_should_stop(sync_data->bai_result, thread_control)) {
    const int arm_index =
        bai_sync_data_get_next_initial_sample_index(&sample_args, worker_stats);
    if (arm_index < 0) {
      break;
    }
    const int64_t sample_start_ns = bai_sched_now();
    const double sample = rvs_sample(rvs, arm_index, rvs_thread_index, NULL);
    bai_sched_sample_done(worker_stats, sample_start_ns);
    bai_sync_data_add_sample(&sample_args, arm_index, sample, worker_stats);
  }
}

// A clock or a user interrupt abandons the rounds from outside the fold
// order, so where it cuts a sim short is not reproducible; a stop condition
// raised by a fold is handled by the fold itself (see
// bai_schedule_complete_while_locked).
static inline bool bai_should_abandon_rounds(BAIResult *bai_result,
                                             ThreadControl *thread_control) {
  const bai_result_status_t status = bai_result_set_and_get_status(
      bai_result, thread_control_get_status(thread_control) ==
                      THREAD_CONTROL_STATUS_USER_INTERRUPT);
  return status == BAI_RESULT_STATUS_USER_INTERRUPT ||
         status == BAI_RESULT_STATUS_TIMEOUT;
}

static inline void bai_worker_round_loop(BAIWorkerArgs *bai_worker_args,
                                         BAIWorkerSchedStats *worker_stats) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = sync_data->thread_control;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int rvs_thread_index = bai_worker_rvs_thread_index(bai_worker_args);

  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, bai_worker_args->bai_options);

  bool abandoned = false;
  while (true) {
    if (bai_should_abandon_rounds(sync_data->bai_result, thread_control)) {
      abandoned = true;
      break;
    }
    uint64_t round_number = 0;
    int slot = 0;
    uint64_t reserved_seed = RVS_SEED_UNRESERVED;
    bai_sync_lock(sync_data, worker_stats);
    const int arm_index = bai_schedule_claim_while_locked(
        sync_data, &round_number, &slot, &reserved_seed);
    if (arm_index < 0) {
      if (sync_data->scheduling_finished) {
        cpthread_mutex_unlock(&sync_data->mutex);
        break;
      }
      // Every scheduled slot is taken but a round is still in flight, so
      // nothing can be scheduled until it folds. Sleep until a fold (or an
      // abandoning worker) signals, rather than spinning on the mutex.
      bai_sched_idle_begin(worker_stats);
      cpthread_cond_wait(&sync_data->work_available, &sync_data->mutex);
      cpthread_mutex_unlock(&sync_data->mutex);
      continue;
    }
    const BAIRound *round =
        &sync_data->rounds[round_number % BAI_SCHEDULE_ROUNDS];
    unsigned char *record =
        round->records != NULL
            ? round->records + (size_t)slot * sync_data->record_size
            : NULL;
    cpthread_mutex_unlock(&sync_data->mutex);
    bai_sched_idle_end(worker_stats);
    const int64_t sample_start_ns = bai_sched_now();
    const double sample =
        rvs_sample_with_seed(rvs, (uint64_t)arm_index, reserved_seed,
                             rvs_thread_index, NULL, record);
    bai_sched_sample_done(worker_stats, sample_start_ns);
    bai_sync_lock(sync_data, worker_stats);
    bai_schedule_complete_while_locked(&sample_args, round_number, slot,
                                       sample);
    cpthread_mutex_unlock(&sync_data->mutex);
  }
  bai_sched_idle_end(worker_stats);
  cpthread_mutex_lock(&sync_data->mutex);
  if (abandoned) {
    // The clock or a user interrupt: freeze the results at the rounds folded
    // so far, as a stop condition would.
    sync_data->stopped = true;
    sync_data->scheduling_finished = true;
  }
  // Wake any worker waiting for work so it can see the same exit condition.
  cpthread_cond_broadcast(&sync_data->work_available);
  cpthread_mutex_unlock(&sync_data->mutex);
}

#ifdef BAI_SCHED_STATS
// Merges one worker's measurements into the sim's, under the mutex.
static inline void bai_sched_worker_merge(BAISyncData *sync_data,
                                          const BAIWorkerSchedStats *stats,
                                          const int64_t exit_ns) {
  cpthread_mutex_lock(&sync_data->mutex);
  BAISchedSimStats *sim_stats = &sync_data->sched_stats;
  sim_stats->busy_ns += stats->busy_ns;
  sim_stats->idle_ns += stats->idle_ns;
  sim_stats->idle_events += stats->idle_episodes;
  sim_stats->lock_wait_ns += stats->lock_wait_ns;
  sim_stats->initial_barrier_ns += stats->initial_barrier_ns;
  sim_stats->avoid_prune_barrier_ns += stats->avoid_prune_barrier_ns;
  bai_sched_histogram_merge(&sync_data->sched_histogram, &stats->histogram);
  sync_data->sched_exit_ns_sum += exit_ns;
  sync_data->sched_num_exits++;
  if (exit_ns > sync_data->sched_last_exit_ns) {
    sync_data->sched_last_exit_ns = exit_ns;
  }
  cpthread_mutex_unlock(&sync_data->mutex);
}

// Derives the per-sim summary once every worker has merged, and hands it to
// the process-wide collector.
static inline void bai_sched_record(BAISyncData *sync_data,
                                    const BAIOptions *bai_options,
                                    const int64_t start_ns) {
  BAISchedSimStats *sim_stats = &sync_data->sched_stats;
  const BAISchedHistogram *histogram = &sync_data->sched_histogram;
  sim_stats->num_threads = bai_options->num_threads;
  sim_stats->num_arms = sync_data->num_arms;
  sim_stats->round_size = sync_data->round_size;
  sim_stats->lag_rounds = BAI_SCHEDULE_LAG;
  sim_stats->sample_limit = bai_options->sample_limit;
  sim_stats->bai_samples = sync_data->sched_bai_completions;
  sim_stats->abandoned_samples =
      sync_data->sched_bai_completions > sim_stats->folded_samples
          ? sync_data->sched_bai_completions - sim_stats->folded_samples
          : 0;
  sim_stats->wall_ns = bai_sched_now() - start_ns;
  sim_stats->tail_ns =
      sync_data->sched_last_exit_ns * (int64_t)sync_data->sched_num_exits -
      sync_data->sched_exit_ns_sum;
  sim_stats->sample_mean_ns =
      histogram->num_samples > 0
          ? histogram->total_ns / (int64_t)histogram->num_samples
          : 0;
  sim_stats->sample_p50_ns = bai_sched_histogram_quantile(histogram, 0.5);
  sim_stats->sample_p99_ns = bai_sched_histogram_quantile(histogram, 0.99);
  sim_stats->sample_p999_ns = bai_sched_histogram_quantile(histogram, 0.999);
  sim_stats->sample_max_ns = histogram->max_ns;
  bai_sched_stats_record(sim_stats);
}
#endif

static inline void *bai_worker(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  BAIWorkerSchedStats worker_stats;
  bai_sched_worker_init(&worker_stats);
  bai_worker_initial_sample_loop(bai_worker_args, &worker_stats);
#ifdef BAI_SCHED_STATS
  const int64_t initial_barrier_start_ns = bai_sched_now();
#endif
  checkpoint_wait(bai_worker_args->checkpoint, bai_worker_args);
#ifdef BAI_SCHED_STATS
  worker_stats.initial_barrier_ns += bai_sched_now() - initial_barrier_start_ns;
#endif
  bai_worker_round_loop(bai_worker_args, &worker_stats);
#ifdef BAI_SCHED_STATS
  const int64_t exit_ns = bai_sched_now();
#endif
  if (bai_worker_args->sync_data->avoid_prune_arms) {
#ifdef BAI_SCHED_STATS
    const int64_t avoid_prune_barrier_start_ns = bai_sched_now();
#endif
    checkpoint_wait(bai_worker_args->avoid_prune_checkpoint, bai_worker_args);
#ifdef BAI_SCHED_STATS
    worker_stats.avoid_prune_barrier_ns +=
        bai_sched_now() - avoid_prune_barrier_start_ns;
#endif
    sim_unpruned_to_winner(bai_worker_args);
  }
#ifdef BAI_SCHED_STATS
  bai_sched_worker_merge(bai_worker_args->sync_data, &worker_stats, exit_ns);
#endif
  return NULL;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
static inline void bai(const BAIOptions *bai_options, RandomVariables *rvs,
                       RandomVariables *rng, ThreadControl *thread_control,
                       BAILogger *bai_logger, BAIResult *bai_result) {
#ifdef BAI_SCHED_STATS
  const int64_t sched_start_ns = bai_sched_now();
#endif
  bai_result_reset(bai_result, bai_options->time_limit_seconds);

  Checkpoint *checkpoint =
      checkpoint_create(bai_options->num_threads, bai_finish_initial_phase);

  BAISyncData *sync_data = bai_sync_data_create(bai_result, thread_control,
                                                (int)rvs_get_num_rvs(rvs), rng);
  sync_data->record_size = rvs_get_sample_record_size(rvs);

  if (bai_options->arm_avoid_prune && bai_options->num_arm_avoid_prune > 0) {
    sync_data->avoid_prune_arms = bai_options->arm_avoid_prune;
    sync_data->avoid_prune_count = bai_options->num_arm_avoid_prune;
    sync_data->avoid_prune_next_idx = 0;
  }

  Checkpoint *avoid_prune_checkpoint =
      checkpoint_create(bai_options->num_threads, bai_avoid_prune_prebroadcast);

  BAIWorkerArgs bai_worker_args = {
      .sync_data = sync_data,
      .rvs = rvs,
      .bai_options = bai_options,
      .bai_logger = bai_logger,
      .checkpoint = checkpoint,
      .avoid_prune_checkpoint = avoid_prune_checkpoint,
  };

  cpthread_t *worker_ids =
      malloc_or_die((sizeof(cpthread_t)) * bai_options->num_threads);
  BAIWorkerArgs *bai_worker_args_array =
      malloc_or_die((sizeof(BAIWorkerArgs)) * bai_options->num_threads);
  for (int thread_index = 0; thread_index < bai_options->num_threads;
       thread_index++) {
    bai_worker_args_array[thread_index] = bai_worker_args;
    bai_worker_args_array[thread_index].thread_index = thread_index;
    cpthread_create(&worker_ids[thread_index], bai_worker,
                    &bai_worker_args_array[thread_index]);
  }
  for (int thread_index = 0; thread_index < bai_options->num_threads;
       thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }
  bai_result_set_best_arm(bai_result, sync_data->astar_index);
  bai_result_stop_timer(bai_result);
#ifdef BAI_SCHED_STATS
  bai_sched_record(sync_data, bai_options, sched_start_ns);
#endif
  free(bai_worker_args_array);
  free(worker_ids);
  bai_sync_data_destroy(sync_data);
  checkpoint_destroy(avoid_prune_checkpoint);
  checkpoint_destroy(checkpoint);
}

#endif
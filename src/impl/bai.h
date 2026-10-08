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
  // Sample values are accumulated in the fixed-point domain described with
  // BAI_FIXED_POINT_SHIFT (bai_defs.h), which is what lets a multithreaded sim
  // reproduce a single-threaded one exactly.
  //
  // A single sample is rejected beyond this magnitude, well outside the range
  // of any random variable this is used with, so that the llround below cannot
  // be handed a value outside the range of int64_t.
  BAI_SAMPLE_MAX_MAGNITUDE = 1 << 20,
};

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
static_assert(BAI_FIXED_POINT_SHIFT < 62,
              "BAI fixed-point scale must leave headroom in int64");

enum {
  // Every sample is scheduled in rounds. A round's schedule is computed from
  // the arm statistics committed when an earlier round folded, never from
  // whatever has merged at the instant a thread asks for work, so the
  // allocation is the same whichever way the worker threads interleave.
  //
  // The first rounds lay the initial phase out round-robin: every arm gets
  // sample_minimum samples, iteration-major, so a run of consecutive slots is
  // the same iteration across consecutive arms. Those rounds need no
  // statistics, so they go out as soon as the sim starts and there is no
  // barrier between the phases: rounds scheduled before the initial phase has
  // fully folded are round-robin too, and the first adaptive round is the
  // first one scheduled after it has.
  //
  // Every constant here is fixed: none depends on -threads, which would make
  // a sim's result depend on how many threads ran it, and none depends on the
  // sample budget, so an N-sample sim's schedule is a prefix of a 2N-sample
  // one up to the point where a stop condition fires.
  //
  // BAI_SCHEDULE_ROUND_SIZE is the fold granularity. A fold and a schedule
  // each cost O(num_arms + round size) under the mutex.
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
  // A worker claims up to this many consecutive slots of a round-robin round
  // at once. Every arm draws its seeds from an identically seeded stream, so
  // consecutive slots of one iteration replay the same opponent rack, and
  // running them on one thread keeps that thread's RIT cache hot. Adaptive
  // rounds are claimed one slot at a time.
  BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH = 8,
};

static_assert(BAI_SCHEDULE_DEPTH_SAMPLES % BAI_SCHEDULE_ROUND_SIZE == 0,
              "BAI schedule depth must be a whole number of rounds");
static_assert(BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH <= BAI_SCHEDULE_ROUND_SIZE,
              "a claim never spans rounds");

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
  // Each slot's sample value and, for random variables with deferrable
  // effects, its record (record_size bytes per slot). They are held here
  // until the round is complete and then folded in slot order, so both the
  // committed statistics and the results the records feed are a function of
  // the sample set, never of completion order, and a round that is never
  // folded leaves no trace.
  double *values;
  unsigned char *records;
  int num_samples;
  int num_claimed;
  int num_completed;
  // The most slots one claim takes from this round.
  int claim_batch;
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
} BAIArmDatum;

typedef struct BAISyncData {
  int num_arms;
  uint64_t num_total_samples_completed;
  uint64_t num_total_samples_requested;
  // num_arms * sample_minimum: the round-robin samples every arm gets before
  // any statistics are read.
  uint64_t initial_limit;
  int astar_index;
  // True until the round holding the last initial-phase sample has folded.
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
  // The round schedule. Rounds [next_round_to_fold, next_round_to_schedule)
  // are live; rounds are claimed in order from next_round_to_claim and folded
  // in order from next_round_to_fold.
  BAIRound rounds[BAI_SCHEDULE_ROUNDS];
  uint64_t next_round_to_claim;
  uint64_t next_round_to_fold;
  uint64_t next_round_to_schedule;
  // True once no further round will be scheduled: the round just scheduled
  // spent the last of the budget, or the sim stopped. A worker that finds
  // nothing to claim exits once this is set, rather than waiting for folds
  // that can only drain rounds already handed out.
  bool scheduling_finished;
  // True once the results are final: a fold raised a stop condition, the
  // budget is spent and folded, or a worker abandoned the rounds on a timeout
  // or user interrupt. Nothing more is claimed or folded, and samples still
  // in flight are discarded, so the results hold exactly the rounds folded
  // before the stop.
  bool stopped;
  size_t record_size;
  // Signaled whenever a fold may have made new work claimable, when the sim
  // stops, and when a worker abandons the rounds. A worker with nothing to
  // claim waits on it instead of spinning on the mutex.
  cpthread_cond_t work_available;
#ifdef BAI_SCHED_STATS
  BAISchedSimStats sched_stats;
  BAISchedHistogram sched_histogram;
  uint64_t sched_completions;
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
  bai_sync_data->num_total_samples_completed = 0;
  bai_sync_data->num_total_samples_requested = 0;
  bai_sync_data->initial_limit = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->avoid_prune_best_arm_idx = -1;
  bai_sync_data->initial_phase = true;
  bai_sync_data->arm_data =
      calloc_or_die(num_initial_arms, sizeof(BAIArmDatum));
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
    bai_sync_data->rounds[i].claim_batch = 1;
  }
  bai_sync_data->next_round_to_claim = 0;
  bai_sync_data->next_round_to_fold = 0;
  bai_sync_data->next_round_to_schedule = 0;
  bai_sync_data->scheduling_finished = false;
  bai_sync_data->stopped = false;
  bai_sync_data->record_size = 0;
#ifdef BAI_SCHED_STATS
  memset(&bai_sync_data->sched_stats, 0, sizeof(bai_sync_data->sched_stats));
  bai_sched_histogram_reset(&bai_sync_data->sched_histogram);
  bai_sync_data->sched_completions = 0;
  bai_sync_data->sched_stop_folded = false;
  bai_sync_data->sched_last_exit_ns = 0;
  bai_sync_data->sched_exit_ns_sum = 0;
  bai_sync_data->sched_num_exits = 0;
#endif
  return bai_sync_data;
}

static inline void bai_sync_data_destroy(BAISyncData *bai_sync_data) {
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
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  double cutoff;
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
  return llround(value * (double)BAI_FIXED_POINT_SCALE);
}

// Adds to a fixed-point accumulator, failing loudly rather than silently
// wrapping. See the overflow bound documented with
// BAI_FIXED_POINT_SHIFT.
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
  const double inverse_scale = 1.0 / (double)BAI_FIXED_POINT_SCALE;
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

// Assumes the caller has locked the bai sync data mutex. Checks the stopping
// threshold against the committed statistics. The challenger is not chosen
// here: scheduling picks it when it lays out a round, from the counts handed
// out by then (see bai_schedule_challenger).
static inline void bai_update_threshold(BAISyncData *bai_sync_data,
                                        RandomVariables *rvs,
                                        const bai_threshold_t threshold,
                                        const double delta) {
  if (threshold == BAI_THRESHOLD_NONE) {
    return;
  }
  const double bai_threshold = log(
      (log((double)bai_sync_data->num_total_samples_completed) + 1) / delta);
  const int astar_index = bai_sync_data->astar_index;
  const BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[astar_index];
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == astar_index ||
        rvs_are_similar(rvs, astar_index, arm_index)) {
      continue;
    }
    const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    const double arm_z = bai_arm_z_with_counts(
        astar_arm_datum, (double)astar_arm_datum->num_samples, arm_datum,
        (double)arm_datum->num_samples);
    if (!(arm_z > bai_threshold)) {
      return;
    }
  }
  bai_result_set_status(bai_sync_data->bai_result, BAI_RESULT_STATUS_THRESHOLD);
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
// thread running. Lays out a top-two round: the samples are split between
// astar and the challenger by rounding the top-two IDS proportion.
static inline void bai_schedule_top_two_round(BAISampleArgs *args,
                                              BAIRound *round) {
  const BAISyncData *bai_sync_data = args->bai_sync_data;
  const int num_samples = round->num_samples;
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
  // shared rng stream from scheduling entirely. Equal means make both terms
  // zero; the coin fell through to the challenger there, so a degenerate
  // proportion allocates the round to the challenger too.
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
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. Lays out the next round and charges it to the budget. The
// budget is the sample limit, but never less than the initial phase, which
// always runs to completion. Rounds scheduled before the initial phase has
// folded, and every round under the round-robin rule, cycle through the arms
// in global sample order; the rest follow the top-two rule from the
// committed statistics.
static inline void bai_schedule_round_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
#ifdef BAI_SCHED_STATS
  const int64_t sched_start_ns = bai_sched_now();
#endif
  if (bai_sync_data->scheduling_finished) {
    return;
  }
  const uint64_t budget = args->sample_limit > bai_sync_data->initial_limit
                              ? args->sample_limit
                              : bai_sync_data->initial_limit;
  const uint64_t requested = bai_sync_data->num_total_samples_requested;
  if (requested >= budget) {
    bai_sync_data->scheduling_finished = true;
    return;
  }
  // A round never straddles the end of the initial phase, so the fold that
  // completes it runs before any later sample is handed out.
  uint64_t round_end = budget;
  if (requested < bai_sync_data->initial_limit &&
      bai_sync_data->initial_limit < round_end) {
    round_end = bai_sync_data->initial_limit;
  }
  int num_samples = BAI_SCHEDULE_ROUND_SIZE;
  if ((uint64_t)num_samples > round_end - requested) {
    num_samples = (int)(round_end - requested);
  }
  BAIRound *round =
      &bai_sync_data->rounds[bai_sync_data->next_round_to_schedule %
                             BAI_SCHEDULE_ROUNDS];
  round->num_samples = num_samples;
  round->num_claimed = 0;
  round->num_completed = 0;
  if (bai_sync_data->initial_phase ||
      args->sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN) {
    for (int slot = 0; slot < num_samples; slot++) {
      round->arm_indices[slot] = (int)((requested + (uint64_t)slot) %
                                       (uint64_t)bai_sync_data->num_arms);
    }
    round->claim_batch = BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH;
  } else {
    bai_schedule_top_two_round(args, round);
    round->claim_batch = 1;
  }
  for (int slot = 0; slot < num_samples; slot++) {
    const int arm_index = round->arm_indices[slot];
    round->seeds[slot] = rvs_next_seed(args->rvs, (uint64_t)arm_index);
    bai_sync_data->arm_data[arm_index].num_scheduled++;
  }
  bai_sync_data->num_total_samples_requested += (uint64_t)num_samples;
  bai_sync_data->next_round_to_schedule++;
  if (bai_sync_data->num_total_samples_requested >= budget) {
    // This round spends the last of the budget. The sample-limit status is
    // recorded when it folds, but no worker needs to wait for that.
    bai_sync_data->scheduling_finished = true;
  }
#ifdef BAI_SCHED_STATS
  round->committed_at_schedule = bai_sync_data->num_total_samples_completed;
  bai_sync_data->sched_stats.schedule_ns += bai_sched_now() - sched_start_ns;
#endif
}

// A run of consecutive slots of one round, handed to one worker.
typedef struct BAIClaim {
  uint64_t round_number;
  int first_slot;
  int num_slots;
} BAIClaim;

// Assumes the caller has locked the bai sync data mutex. Hands out the next
// unclaimed slots of the oldest round that still has some, up to the round's
// claim batch, and returns false when every scheduled slot is taken or the
// sim has stopped. Threads pull greedily across round boundaries, so a slow
// sample in one round does not keep the others from working on the rounds
// behind it.
static inline bool bai_schedule_claim_while_locked(BAISyncData *bai_sync_data,
                                                   BAIClaim *claim) {
  if (bai_sync_data->stopped) {
    return false;
  }
  while (bai_sync_data->next_round_to_claim <
         bai_sync_data->next_round_to_schedule) {
    BAIRound *round =
        &bai_sync_data
             ->rounds[bai_sync_data->next_round_to_claim % BAI_SCHEDULE_ROUNDS];
    if (round->num_claimed < round->num_samples) {
      int num_slots = round->num_samples - round->num_claimed;
      if (num_slots > round->claim_batch) {
        num_slots = round->claim_batch;
      }
      claim->round_number = bai_sync_data->next_round_to_claim;
      claim->first_slot = round->num_claimed;
      claim->num_slots = num_slots;
      round->num_claimed += num_slots;
      // Move past a fully claimed round now rather than on the next claim.
      // Once its samples complete it folds and its ring slot takes a new
      // round; a cursor still pointing at it would hand out the new round's
      // slots under the old round's number, ahead of the rounds before it.
      if (round->num_claimed == round->num_samples) {
        bai_sync_data->next_round_to_claim++;
      }
#ifdef BAI_SCHED_STATS
      const uint64_t completed = bai_sync_data->sched_completions;
      const uint64_t stale = completed > round->committed_at_schedule
                                 ? completed - round->committed_at_schedule
                                 : 0;
      bai_sync_data->sched_stats.stale_sum += stale * (uint64_t)num_slots;
      bai_sync_data->sched_stats.stale_count += (uint64_t)num_slots;
      if (stale > bai_sync_data->sched_stats.stale_max) {
        bai_sync_data->sched_stats.stale_max = stale;
      }
#endif
      return true;
    }
    bai_sync_data->next_round_to_claim++;
  }
  return false;
}

// Assumes the caller has locked the bai sync data mutex. Folds one finished
// round into the committed statistics and the random variables' results, in
// slot order, then re-derives everything the next schedule reads and checks
// the stop conditions. None are checked during the initial phase.
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

  if (bai_sync_data->initial_phase) {
    if (bai_sync_data->num_total_samples_completed <
        bai_sync_data->initial_limit) {
      return;
    }
    bai_sync_data->initial_phase = false;
#ifdef BAI_SCHED_STATS
    bai_sync_data->sched_stats.initial_samples = bai_sync_data->initial_limit;
#endif
  }
  // When the initial phase alone spends the budget, the sim simply ends on
  // the sample limit.
  if (args->sample_limit <= bai_sync_data->initial_limit) {
    return;
  }
  if (is_win_pct_within_cutoff(astar_mean, args->cutoff)) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
    return;
  }
  bai_update_threshold(bai_sync_data, args->rvs, args->threshold, args->delta);
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
  bai_sync_data->sched_completions++;
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

// Allocates the rounds and fills the pipeline with the first rounds of the
// initial phase. Runs once, single-threaded, before any worker starts.
static inline void bai_sync_data_start_schedule(BAISampleArgs *args,
                                                const uint64_t sample_minimum) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  // Every arm needs at least one committed sample before the top-two rule
  // can read its mean.
  const uint64_t samples_per_arm = sample_minimum > 0 ? sample_minimum : 1;
  bai_sync_data->initial_limit =
      (uint64_t)bai_sync_data->num_arms * samples_per_arm;
  const size_t round_size = BAI_SCHEDULE_ROUND_SIZE;
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
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    bai_schedule_round_while_locked(args);
  }
}

typedef struct BAIWorkerArgs {
  BAISyncData *sync_data;
  RandomVariables *rvs;
  const BAIOptions *bai_options;
  BAILogger *bai_logger;
  Checkpoint *avoid_prune_checkpoint;
  int thread_index;
} BAIWorkerArgs;

// Builds the per-sample argument block from the options.
static inline BAISampleArgs
bai_sample_args_create(BAISyncData *sync_data, RandomVariables *rvs,
                       const BAIOptions *bai_options) {
  BAISampleArgs sample_args = {
      .bai_sync_data = sync_data,
      .rvs = rvs,
      .delta = bai_options->delta,
      .sample_limit = bai_options->sample_limit,
      .sampling_rule = bai_options->sampling_rule,
      .threshold = bai_options->threshold,
      .cutoff = bai_options->cutoff,
  };
  return sample_args;
}

static inline void bai_avoid_prune_prebroadcast(void *data) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)data;
  BAISyncData *sync_data = bai_worker_args->sync_data;
  sync_data->avoid_prune_best_arm_idx =
      rvs_get_best_arm_index(bai_worker_args->rvs);
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

  double sample_values[BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH];
  bool abandoned = false;
  while (true) {
    if (bai_should_abandon_rounds(sync_data->bai_result, thread_control)) {
      abandoned = true;
      break;
    }
    BAIClaim claim;
    bai_sync_lock(sync_data, worker_stats);
    if (!bai_schedule_claim_while_locked(sync_data, &claim)) {
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
    // The claimed slots were laid out before they were handed out, under the
    // mutex, and nothing rewrites them until the round folds, which needs
    // these very samples; they are safe to read without the lock.
    const BAIRound *round =
        &sync_data->rounds[claim.round_number % BAI_SCHEDULE_ROUNDS];
    cpthread_mutex_unlock(&sync_data->mutex);
    bai_sched_idle_end(worker_stats);
    for (int claim_idx = 0; claim_idx < claim.num_slots; claim_idx++) {
      const int slot = claim.first_slot + claim_idx;
      unsigned char *record =
          round->records != NULL
              ? round->records + (size_t)slot * sync_data->record_size
              : NULL;
      const int64_t sample_start_ns = bai_sched_now();
      sample_values[claim_idx] = rvs_sample_with_seed(
          rvs, (uint64_t)round->arm_indices[slot], round->seeds[slot],
          rvs_thread_index, NULL, record);
      bai_sched_sample_done(worker_stats, sample_start_ns);
    }
    bai_sync_lock(sync_data, worker_stats);
    for (int claim_idx = 0; claim_idx < claim.num_slots; claim_idx++) {
      bai_schedule_complete_while_locked(&sample_args, claim.round_number,
                                         claim.first_slot + claim_idx,
                                         sample_values[claim_idx]);
    }
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
  sim_stats->round_size = BAI_SCHEDULE_ROUND_SIZE;
  sim_stats->lag_rounds = BAI_SCHEDULE_LAG;
  sim_stats->sample_limit = bai_options->sample_limit;
  sim_stats->bai_samples =
      sync_data->sched_completions > sim_stats->initial_samples
          ? sync_data->sched_completions - sim_stats->initial_samples
          : 0;
  sim_stats->abandoned_samples =
      sync_data->sched_completions > sim_stats->folded_samples
          ? sync_data->sched_completions - sim_stats->folded_samples
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

  BAISyncData *sync_data = bai_sync_data_create(bai_result, thread_control,
                                                (int)rvs_get_num_rvs(rvs), rng);
  sync_data->record_size = rvs_get_sample_record_size(rvs);

  if (bai_options->arm_avoid_prune && bai_options->num_arm_avoid_prune > 0) {
    sync_data->avoid_prune_arms = bai_options->arm_avoid_prune;
    sync_data->avoid_prune_count = bai_options->num_arm_avoid_prune;
    sync_data->avoid_prune_next_idx = 0;
  }

  // The first rounds need no statistics, so the pipeline is filled before
  // any worker starts.
  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, bai_options);
  bai_sync_data_start_schedule(&sample_args, bai_options->sample_minimum);

  Checkpoint *avoid_prune_checkpoint =
      checkpoint_create(bai_options->num_threads, bai_avoid_prune_prebroadcast);

  BAIWorkerArgs bai_worker_args = {
      .sync_data = sync_data,
      .rvs = rvs,
      .bai_options = bai_options,
      .bai_logger = bai_logger,
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
  bai_result_set_num_samples(bai_result,
                             sync_data->num_total_samples_completed);
  bai_result_stop_timer(bai_result);
#ifdef BAI_SCHED_STATS
  bai_sched_record(sync_data, bai_options, sched_start_ns);
#endif
  free(bai_worker_args_array);
  free(worker_ids);
  bai_sync_data_destroy(sync_data);
  checkpoint_destroy(avoid_prune_checkpoint);
}

#endif

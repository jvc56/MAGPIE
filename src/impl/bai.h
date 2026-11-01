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
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../def/bai_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bai_result.h"
#include "../ent/bai_sched_stats.h"
#include "../ent/checkpoint.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "bai_logger.h"
#include "move_gen.h"
#include "move_gen_base.h"
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
  // A sample is rejected beyond this magnitude, well outside the range of
  // any random variable this is used with. The bound is on the raw sample, and
  // its square is what limits it: a sample of at most 2^10 has a square of at
  // most 2^20, which quantizes to at most 2^50, far inside int64_t.
  BAI_SAMPLE_MAX_MAGNITUDE = 1 << 10,
};

// samples_squared_sum accumulates the quantized value of (value * value), NOT
// the square of the quantized value. Squaring after quantizing would
// accumulate 2^60-scale terms and overflow int64 after only eight samples.
//
// For sim utilities, which sim_utility_blend bounds to [0, 1], a quantized
// sample and a quantized square are each at most 2^30, so both accumulators
// stay exact past 8e9 samples (int64 holds about 9.22e18). At the magnitude
// bound a quantized square is 2^50, so samples_squared_sum overflows after
// about 8000 such samples. bai() is generic over RandomVariables, though, and
// the normal RVs the unit tests drive it with are unbounded, so nothing here
// can bound an arbitrary RV's range in advance. bai_accumulate_checked
// therefore fails loudly rather than silently wrapping.
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
  // BAI_SCHEDULE_ROUND_SIZE is the fold granularity, in draws: a round holds
  // that many single-draw samples, or about that many draws' worth of samples
  // that average several (see bai_round_size), so that the lags below cost
  // the same number of draws either way. A fold and a schedule each cost
  // O(num_arms + round size) under the mutex.
  BAI_SCHEDULE_ROUND_SIZE = 32,
  // Round j is scheduled when round j - lag - 1 folds, from the statistics
  // committed by then. Initial-phase rounds need no statistics, so they run
  // far ahead: a slow sample holding up the oldest round cannot starve the
  // other threads, which keep claiming from the rounds behind it.
  BAI_SCHEDULE_ROUND_ROBIN_LAG = 32,
  // Adaptive rounds read statistics, and every round of lag makes them
  // staler. Measured as move-choice regret against a high-budget reference,
  // two rounds (64 samples) choose as well as scheduling every sample from
  // fully current statistics, while 128 samples of lag already choose
  // measurably worse and 1024 much worse at 500-1000 sample budgets. Bag-
  // cycled sims, whose samples average about 12 rollouts, chose much worse
  // than plain ones at 1000-3000 rollouts with 32-sample rounds, and as well
  // with rounds sized in rollouts. When this little lag leaves a worker
  // nothing to claim, it speculates instead of waiting (see BAISpeculation).
  BAI_SCHEDULE_ADAPTIVE_LAG = 2,
  // The ring needs one more round than the longest lag: the slot freed by
  // the round that just folded is the one the newly scheduled round takes.
  BAI_SCHEDULE_ROUNDS = BAI_SCHEDULE_ROUND_ROBIN_LAG + 1,
  // A worker claims up to this many consecutive slots of a round-robin round
  // at once. Every arm draws its seeds from an identically seeded stream, so
  // consecutive slots of one iteration replay the same opponent rack, and
  // running them on one thread keeps that thread's RIT cache hot. Adaptive
  // rounds are claimed one slot at a time.
  BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH = 8,
  // Speculative samples held per worker thread, and how many rounds of
  // BAI_SCHEDULE_ROUND_SIZE slots past the last one laid out speculation
  // looks. A straggler under OS preemption can run 10x the mean sample,
  // during which each other worker wants about that many samples of other
  // work.
  BAI_SPECULATIONS_PER_THREAD = 16,
  BAI_SPECULATION_ROUNDS = 8,
  BAI_SPECULATION_SLOTS = BAI_SPECULATION_ROUNDS * BAI_SCHEDULE_ROUND_SIZE,
  // The cache never holds more entries than there are expected slots to fill,
  // so its scans stay bounded however many threads run the sim. At 16 or fewer
  // threads the per-thread count is below this cap.
  BAI_SPECULATIONS_MAX = BAI_SPECULATION_SLOTS,
};

enum {
  // When a sim's arms share seeds, the stopping test estimates how each pair
  // of arms' samples covary from at most this many of the ordinals both
  // have (see bai_paired_z). Replaying sims at 2 ply, the test stopped just
  // as early with 128 as with every sample, so this bounds the memory
  // without costing anything.
  BAI_PAIRED_MAX_ORDINALS = 1024,
};

static_assert(BAI_SCHEDULE_ADAPTIVE_LAG < BAI_SCHEDULE_ROUND_ROBIN_LAG,
              "adaptive rounds never run further ahead than round-robin ones");
static_assert(BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH <= BAI_SCHEDULE_ROUND_SIZE,
              "a claim never spans rounds");

typedef enum {
  BAI_SLOT_FREE,
  // Claimed by a worker, or adopted from a speculative sample still running.
  BAI_SLOT_TAKEN,
  // Its value and record are in the round.
  BAI_SLOT_DONE,
} bai_slot_state_t;

typedef enum {
  BAI_SPECULATION_EMPTY,
  BAI_SPECULATION_RUNNING,
  BAI_SPECULATION_DONE,
} bai_speculation_state_t;

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
  // A bai_slot_state_t per slot. A slot is taken once it is handed out:
  // claimed by a worker, or matched when the round was laid out to a
  // speculative sample (see BAISpeculation). Claims skip every slot that is
  // not free. A slot is done once its value and record are in place, which is
  // what lets a sim cut short by the clock fold the samples that finished.
  unsigned char *slot_state;
  int num_samples;
  // Claims scan forward from here; every slot before it is taken.
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

// A sample run ahead of the schedule by a worker that had nothing to claim.
// A sample is a pure function of its arm and its seed, and an arm's seeds come
// from its own stream in order, so the n-th sample of an arm (its ordinal) is
// the same whenever and on whichever thread it runs. A worker that would
// otherwise wait therefore samples the slots the next round will most likely
// hold, at the seeds they will reserve, and keeps the results here. When a
// round is laid out, a slot whose (arm, ordinal) is already done is filled in
// from here and never claimed; one still running is adopted, and the
// speculating worker completes it when it finishes. Results that no round
// ever schedules are dropped. Which samples get speculated depends on thread
// timing, but a slot's value never does, so the results do not either.
typedef struct BAISpeculation {
  bai_speculation_state_t state;
  int arm_index;
  uint64_t ordinal;
  uint64_t seed;
  double value;
  unsigned char *record;
  // Set when a round laid out the slot this sample turned out to be while it
  // was still running.
  bool adopted;
  uint64_t round_number;
  int slot;
  // Whether it is one of the currently expected slots (see BAISyncData).
  bool expected;
} BAISpeculation;

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
  // The stopping test's paired statistics (see bai_paired_z), or NULL when
  // the sim has no stopping threshold or its arms don't share seeds. Each arm
  // keeps its first BAI_PAIRED_MAX_ORDINALS committed samples by ordinal,
  // with fixed-point prefix sums of them and of their squares, and each pair
  // of arms (row the lower index) the fixed-point sum of their products at
  // the ordinals both have.
  double *paired_values;
  int64_t *paired_prefix;
  int64_t *paired_prefix_squares;
  int64_t *paired_cross;
  // With BAIOptions.eliminate, whether each arm is out (see
  // bai_update_elimination) and how many are; NULL otherwise.
  bool *eliminated;
  int num_eliminated;
  // BAIOptions.arm_sample_limit where it applies (see bai()); 0 otherwise.
  uint64_t arm_sample_limit;
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
  // The round that holds the last initial-phase sample, once it has been
  // laid out; UINT64_MAX before that.
  uint64_t initial_last_round;
  // Samples per round (see bai_round_size).
  int round_size;
  // The speculation cache, or NULL when the random variables cannot peek at
  // future seeds (and so a sample is not a function of its slot).
  BAISpeculation *speculations;
  int num_speculations;
  // Finds the entry holding an arm's sample at an ordinal without scanning the
  // cache: num_arms rows of BAI_SPECULATION_SLOTS, where row arm_index, column
  // ordinal % BAI_SPECULATION_SLOTS holds the entry's index, or -1. Every
  // speculation an arm can still use has an ordinal in [num_scheduled,
  // num_scheduled + BAI_SPECULATION_SLOTS), so at most one of the ordinals
  // that share a column is live. A newer entry can overwrite a stale one,
  // which is never looked up again; see bai_speculation_find.
  int *speculation_index;
  // The slots the next rounds are expected to hold, which only change when a
  // round is laid out or folded: schedule_epoch counts those events, and the
  // expectation is recomputed when it moves. covered marks the expected slots
  // a speculation already holds; claims take uncovered ones in order from
  // speculation_cursor.
  uint64_t schedule_epoch;
  uint64_t speculation_epoch;
  bool speculation_valid;
  int speculation_cursor;
  int speculation_arms[BAI_SPECULATION_SLOTS];
  uint64_t speculation_ordinals[BAI_SPECULATION_SLOTS];
  bool speculation_covered[BAI_SPECULATION_SLOTS];
  // Scratch for the refresh: per arm, a running count of its expected slots.
  uint64_t *speculation_arm_count;
  // Slots scheduled but not yet claimed, and the number of workers. Together
  // they size claims near the end of a sim (see
  // bai_schedule_claim_while_locked); neither affects what is sampled.
  uint64_t num_unclaimed;
  int num_threads;
  // Signaled whenever a fold may have made new work claimable, when the sim
  // stops, and when a worker abandons the rounds. A worker with nothing to
  // claim waits on it instead of spinning on the mutex.
  cpthread_cond_t work_available;
#ifdef BAI_SCHED_STATS
  BAISchedSimStats sched_stats;
  BAISchedHistogram sched_histogram;
  uint64_t sched_completions;
  bool sched_stop_folded;
  int64_t sched_start_ns;
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
  int64_t first_claim_ns;
  BAISchedHistogram histogram;
} BAIWorkerSchedStats;

static inline int64_t bai_sched_now(void) { return ctimer_monotonic_ns(); }

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
  if (worker_stats->first_claim_ns == 0) {
    worker_stats->first_claim_ns = ctimer_monotonic_ns();
  }
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

static inline void bai_sched_sample_done(const BAIWorkerSchedStats
                                             __attribute__((unused)) *
                                             worker_stats,
                                         const int64_t
                                         __attribute__((unused)) start_ns) {}

static inline void bai_sched_idle_begin(const BAIWorkerSchedStats
                                        __attribute__((unused)) *
                                        worker_stats) {}

static inline void bai_sched_idle_end(const BAIWorkerSchedStats
                                      __attribute__((unused)) *
                                      worker_stats) {}
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
                                                const int num_initial_arms) {
  BAISyncData *bai_sync_data = malloc_or_die(sizeof(BAISyncData));
  bai_sync_data->num_arms = num_initial_arms;
  bai_sync_data->num_total_samples_completed = 0;
  bai_sync_data->num_total_samples_requested = 0;
  bai_sync_data->initial_limit = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->avoid_prune_best_arm_idx = -1;
  bai_sync_data->initial_phase = true;
  // bai() sets it from the options.
  bai_sync_data->round_size = BAI_SCHEDULE_ROUND_SIZE;
  bai_sync_data->arm_data =
      calloc_or_die(num_initial_arms, sizeof(BAIArmDatum));
  // bai() allocates them when the stopping test uses them.
  bai_sync_data->paired_values = NULL;
  bai_sync_data->paired_prefix = NULL;
  bai_sync_data->paired_prefix_squares = NULL;
  bai_sync_data->paired_cross = NULL;
  // bai() allocates it when arms can be eliminated.
  bai_sync_data->eliminated = NULL;
  bai_sync_data->num_eliminated = 0;
  bai_sync_data->arm_sample_limit = 0;
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
    bai_sync_data->rounds[i].slot_state = NULL;
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
  bai_sync_data->num_unclaimed = 0;
  bai_sync_data->num_threads = 1;
  bai_sync_data->initial_last_round = UINT64_MAX;
  bai_sync_data->speculations = NULL;
  bai_sync_data->num_speculations = 0;
  bai_sync_data->schedule_epoch = 0;
  bai_sync_data->speculation_epoch = UINT64_MAX;
  bai_sync_data->speculation_valid = false;
  bai_sync_data->speculation_cursor = 0;
  bai_sync_data->speculation_index = NULL;
  bai_sync_data->speculation_arm_count = NULL;
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
    free(bai_sync_data->rounds[i].slot_state);
  }
  for (int spec_idx = 0; spec_idx < bai_sync_data->num_speculations;
       spec_idx++) {
    free(bai_sync_data->speculations[spec_idx].record);
  }
  free(bai_sync_data->speculations);
  free(bai_sync_data->speculation_index);
  free(bai_sync_data->speculation_arm_count);
  free(bai_sync_data->paired_values);
  free(bai_sync_data->paired_prefix);
  free(bai_sync_data->paired_prefix_squares);
  free(bai_sync_data->paired_cross);
  free(bai_sync_data->eliminated);
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
  double elimination_z;
  double cutoff;
} BAISampleArgs;

// Quantizes a value into the fixed-point integer domain the arm accumulators
// use. The caller bounds the value (see bai_accumulate_sample), so that the
// llround cannot be handed a value outside the range of int64_t.
static inline int64_t bai_quantize(const double value) {
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

// Adds one sample to an arm's accumulators. Does not clamp to [0, 1]: sim
// samples are utilities in that range, but bai() is generic over
// RandomVariables and the normal RVs the unit tests drive it with are
// unbounded. The raw sample is checked against BAI_SAMPLE_MAX_MAGNITUDE, which
// also bounds its square.
static inline void bai_accumulate_sample(BAIArmDatum *arm_datum,
                                         const double value) {
  if (!(value >= -(double)BAI_SAMPLE_MAX_MAGNITUDE &&
        value <= (double)BAI_SAMPLE_MAX_MAGNITUDE)) {
    log_fatal("BAI sample value out of range: %f", value);
  }
  arm_datum->num_samples++;
  bai_accumulate_checked(&arm_datum->samples_sum, bai_quantize(value));
  bai_accumulate_checked(&arm_datum->samples_squared_sum,
                         bai_quantize(value * value));
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

// Allocates the paired statistics, which makes the stopping test use
// bai_paired_z. Runs before any sample is committed.
static inline void bai_sync_data_enable_paired(BAISyncData *bai_sync_data) {
  const size_t num_arms = (size_t)bai_sync_data->num_arms;
  bai_sync_data->paired_values =
      malloc_or_die(sizeof(double) * num_arms * BAI_PAIRED_MAX_ORDINALS);
  bai_sync_data->paired_prefix =
      calloc_or_die(num_arms * (BAI_PAIRED_MAX_ORDINALS + 1), sizeof(int64_t));
  bai_sync_data->paired_prefix_squares =
      calloc_or_die(num_arms * (BAI_PAIRED_MAX_ORDINALS + 1), sizeof(int64_t));
  bai_sync_data->paired_cross =
      calloc_or_die(num_arms * num_arms, sizeof(int64_t));
}

// Assumes the caller has locked the bai sync data mutex. Records an arm's
// committed sample at ordinal in the paired statistics, and adds its product
// with each other arm's sample at the same ordinal, if that one is committed
// already. A round folds in slot order and an arm's slots in a round have
// increasing ordinals, so each arm's samples arrive in ordinal order, and
// each pair's product at an ordinal is added once, by whichever arm commits
// it second.
static inline void bai_paired_record_while_locked(BAISyncData *bai_sync_data,
                                                  const int arm_index,
                                                  const uint64_t ordinal,
                                                  const double value) {
  if (bai_sync_data->paired_values == NULL ||
      ordinal >= BAI_PAIRED_MAX_ORDINALS) {
    return;
  }
  const int num_arms = bai_sync_data->num_arms;
  bai_sync_data
      ->paired_values[(size_t)arm_index * BAI_PAIRED_MAX_ORDINALS + ordinal] =
      value;
  int64_t *prefix = bai_sync_data->paired_prefix +
                    (size_t)arm_index * (BAI_PAIRED_MAX_ORDINALS + 1);
  prefix[ordinal + 1] = prefix[ordinal] + bai_quantize(value);
  int64_t *prefix_squares = bai_sync_data->paired_prefix_squares +
                            (size_t)arm_index * (BAI_PAIRED_MAX_ORDINALS + 1);
  prefix_squares[ordinal + 1] =
      prefix_squares[ordinal] + bai_quantize(value * value);
  for (int other_index = 0; other_index < num_arms; other_index++) {
    if (other_index == arm_index ||
        bai_sync_data->arm_data[other_index].num_samples <= ordinal) {
      continue;
    }
    const double other_value =
        bai_sync_data
            ->paired_values[(size_t)other_index * BAI_PAIRED_MAX_ORDINALS +
                            ordinal];
    const int low_index = arm_index < other_index ? arm_index : other_index;
    const int high_index = arm_index < other_index ? other_index : arm_index;
    bai_accumulate_checked(
        &bai_sync_data
             ->paired_cross[(size_t)low_index * (size_t)num_arms + high_index],
        bai_quantize(value * other_value));
  }
}

// The paired statistics' sum for arm_index over its first num_pairs ordinals,
// from one of the per-arm prefix arrays, unscaled.
static inline double bai_paired_prefix_sum(const int64_t *prefix_array,
                                           const int arm_index,
                                           const uint64_t num_pairs) {
  return (double)
             prefix_array[(size_t)arm_index * (BAI_PAIRED_MAX_ORDINALS + 1) +
                          num_pairs] /
         (double)BAI_FIXED_POINT_SCALE;
}

// Assumes the caller has locked the bai sync data mutex. The Z statistic for
// "astar beats arm" when the two arms' samples at each ordinal share a seed,
// as a sim's do, so that much of their luck is common to both. With n_a and
// n_b samples, m of them at ordinals both have, variances v_a and v_b, and c
// the covariance of a pair of samples at one ordinal,
//
//   Var(mean_a - mean_b) = v_a / n_a + v_b / n_b - 2 c m / (n_a n_b)
//
// and Z is the squared gap over twice that. v_a, v_b and c all come from the
// same ordinals, the first m (at most BAI_PAIRED_MAX_ORDINALS), so together
// they are a valid covariance, and since m is at most n_a and n_b, the
// variance above can't be negative. Mixing in variances from more samples
// than the covariance saw could make it negative for strongly correlated
// arms. If the estimate still isn't positive (too few pairs, or a difference
// that never varied), this falls back to bai_arm_z_with_counts, which
// ignores the shared luck and so never overstates the evidence.
static inline double bai_paired_z(const BAISyncData *bai_sync_data,
                                  const int astar_index, const int arm_index) {
  const BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[astar_index];
  const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
  const double astar_num_samples = (double)astar_arm_datum->num_samples;
  const double arm_num_samples = (double)arm_datum->num_samples;
  const uint64_t common_ordinals =
      astar_arm_datum->num_samples < arm_datum->num_samples
          ? astar_arm_datum->num_samples
          : arm_datum->num_samples;
  const uint64_t num_pairs = common_ordinals < BAI_PAIRED_MAX_ORDINALS
                                 ? common_ordinals
                                 : BAI_PAIRED_MAX_ORDINALS;
  double variance = 0.0;
  if (num_pairs > 1) {
    const double pairs = (double)num_pairs;
    const double astar_mean =
        bai_paired_prefix_sum(bai_sync_data->paired_prefix, astar_index,
                              num_pairs) /
        pairs;
    const double arm_mean = bai_paired_prefix_sum(bai_sync_data->paired_prefix,
                                                  arm_index, num_pairs) /
                            pairs;
    const double astar_var =
        bai_paired_prefix_sum(bai_sync_data->paired_prefix_squares, astar_index,
                              num_pairs) /
            pairs -
        astar_mean * astar_mean;
    const double arm_var =
        bai_paired_prefix_sum(bai_sync_data->paired_prefix_squares, arm_index,
                              num_pairs) /
            pairs -
        arm_mean * arm_mean;
    const int low_index = astar_index < arm_index ? astar_index : arm_index;
    const int high_index = astar_index < arm_index ? arm_index : astar_index;
    const double cross_sum =
        (double)bai_sync_data
            ->paired_cross[(size_t)low_index * (size_t)bai_sync_data->num_arms +
                           high_index] /
        (double)BAI_FIXED_POINT_SCALE;
    const double covariance = cross_sum / pairs - astar_mean * arm_mean;
    variance = astar_var / astar_num_samples + arm_var / arm_num_samples -
               2.0 * covariance * (double)common_ordinals /
                   (astar_num_samples * arm_num_samples);
  }
  if (!(variance > 0.0)) {
    return bai_arm_z_with_counts(astar_arm_datum, astar_num_samples, arm_datum,
                                 arm_num_samples);
  }
  const double gap = astar_arm_datum->mean - arm_datum->mean;
  return gap * gap / (2.0 * variance);
}

// Assumes the caller has locked the bai sync data mutex. Checks the stopping
// threshold against the committed statistics: every arm other than astar must
// either be similar to it or have a Z statistic above the GK16 threshold.
// With no threshold, only the first condition can hold, so under the top-two
// rule a sim whose plays are all similar to astar still stops; under
// round-robin it never does. The challenger is not chosen here: scheduling
// picks it when it lays out a round (see bai_schedule_challenger).
static inline void bai_update_threshold(BAISyncData *bai_sync_data,
                                        RandomVariables *rvs,
                                        const bai_threshold_t threshold,
                                        const bai_sampling_rule_t sampling_rule,
                                        const double delta) {
  if (threshold == BAI_THRESHOLD_NONE &&
      sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN) {
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
    if (threshold == BAI_THRESHOLD_NONE) {
      return;
    }
    const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    const double arm_z =
        bai_sync_data->paired_values != NULL
            ? bai_paired_z(bai_sync_data, astar_index, arm_index)
            : bai_arm_z_with_counts(astar_arm_datum,
                                    (double)astar_arm_datum->num_samples,
                                    arm_datum, (double)arm_datum->num_samples);
    if (!(arm_z > bai_threshold)) {
      return;
    }
  }
  bai_result_set_status(bai_sync_data->bai_result, BAI_RESULT_STATUS_THRESHOLD);
}

// Assumes the caller has locked the bai sync data mutex. The stopping test
// with elimination, in place of bai_update_threshold: an arm the best arm
// beats by the GK16 threshold, or by elimination_z standard errors when that
// is positive, is out, and the sim stops when every arm other than the best is
// out or similar to it. Arms only leave between folds, from the committed
// statistics, so which arms are out never depends on how the worker threads
// interleave. The best arm is the highest mean of all arms
// (bai_update_astar_while_locked), as the sim's chosen play is; an arm that is
// out and becomes the best again, as the others' means fall, comes back.
static inline void bai_update_elimination(BAISyncData *bai_sync_data,
                                          RandomVariables *rvs,
                                          const double delta,
                                          const double elimination_z) {
  // The Z statistic is the squared gap over twice its variance (see
  // bai_paired_z), so leading by z standard errors is Z > z^2 / 2.
  const double bai_threshold =
      elimination_z > 0
          ? elimination_z * elimination_z / 2.0
          : log((log((double)bai_sync_data->num_total_samples_completed) + 1) /
                delta);
  const int astar_index = bai_sync_data->astar_index;
  bool *eliminated = bai_sync_data->eliminated;
  if (eliminated[astar_index]) {
    eliminated[astar_index] = false;
    bai_sync_data->num_eliminated--;
  }
  const BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[astar_index];
  bool contested = false;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == astar_index || eliminated[arm_index] ||
        rvs_are_similar(rvs, astar_index, arm_index)) {
      continue;
    }
    const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    const double arm_z =
        bai_sync_data->paired_values != NULL
            ? bai_paired_z(bai_sync_data, astar_index, arm_index)
            : bai_arm_z_with_counts(astar_arm_datum,
                                    (double)astar_arm_datum->num_samples,
                                    arm_datum, (double)arm_datum->num_samples);
    if (arm_z > bai_threshold) {
      eliminated[arm_index] = true;
      bai_sync_data->num_eliminated++;
    } else {
      contested = true;
    }
  }
  if (!contested) {
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_THRESHOLD);
  }
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. Lays out up to num_slots round-robin slots starting at
// global sample requested: arm after arm in index order, or, once arms have
// been eliminated or under a per-arm limit, each slot to the arm still in with
// the fewest samples handed out (the lowest index on a tie), which is the same
// order while their counts are level. An arm at the per-arm limit gets no
// more. Returns the slots laid out, fewer than num_slots only when every arm
// still in is at the limit.
static inline int bai_round_robin_arms(BAISyncData *bai_sync_data,
                                       const uint64_t requested,
                                       const int num_slots, int *arm_indices) {
  if (bai_sync_data->num_eliminated == 0 &&
      bai_sync_data->arm_sample_limit == 0) {
    for (int slot = 0; slot < num_slots; slot++) {
      arm_indices[slot] = (int)((requested + (uint64_t)slot) %
                                (uint64_t)bai_sync_data->num_arms);
    }
    return num_slots;
  }
  const bool *eliminated = bai_sync_data->eliminated;
  const uint64_t arm_sample_limit = bai_sync_data->arm_sample_limit;
  int num_laid_out = 0;
  while (num_laid_out < num_slots) {
    int next_index = -1;
    for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
      const uint64_t num_scheduled =
          bai_sync_data->arm_data[arm_index].num_scheduled;
      if ((eliminated != NULL && eliminated[arm_index]) ||
          (arm_sample_limit > 0 && num_scheduled >= arm_sample_limit)) {
        continue;
      }
      if (next_index < 0 ||
          num_scheduled < bai_sync_data->arm_data[next_index].num_scheduled) {
        next_index = arm_index;
      }
    }
    if (next_index < 0) {
      break;
    }
    arm_indices[num_laid_out] = next_index;
    bai_sync_data->arm_data[next_index].num_scheduled++;
    num_laid_out++;
  }
  // The caller counts the slots it lays out.
  for (int slot = 0; slot < num_laid_out; slot++) {
    bai_sync_data->arm_data[arm_indices[slot]].num_scheduled--;
  }
  return num_laid_out;
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. The top-two challenger: the arm minimizing Z + log(N)
// against astar, as in the sequential rule, but with every arm weighted by the
// samples handed out to it (num_scheduled) rather than only those committed.
// Eliminated arms are never the challenger. Returns astar when every other arm
// is similar to it or out.
static inline int bai_schedule_challenger(const BAISyncData *bai_sync_data,
                                          RandomVariables *rvs,
                                          const int astar_index) {
  const BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[astar_index];
  const double astar_num_scheduled = (double)astar_arm_datum->num_scheduled;
  const bool *eliminated = bai_sync_data->eliminated;
  int challenger_index = -1;
  double challenger_value = 0.0;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == astar_index ||
        (eliminated != NULL && eliminated[arm_index]) ||
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
// thread running. Lays out num_samples slots of a top-two round: the samples
// are split between astar and the challenger by rounding the top-two IDS
// proportion.
static inline void bai_schedule_top_two_arms(const BAISampleArgs *args,
                                             const int num_samples,
                                             int *arm_indices) {
  const BAISyncData *bai_sync_data = args->bai_sync_data;
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
    arm_indices[slot] =
        (slot < astar_count) ? astar_arm_index : challenger_arm_index;
  }
}

// Assumes the caller has locked the bai sync data mutex. Whether the next
// round to be laid out will be round-robin: every initial-phase round, every
// round under the round-robin rule, and the adaptive-lag rounds laid out
// before the initial phase has folded.
static inline bool bai_next_round_is_round_robin(const BAISampleArgs *args) {
  const BAISyncData *bai_sync_data = args->bai_sync_data;
  if (args->sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN ||
      bai_sync_data->num_total_samples_requested <
          bai_sync_data->initial_limit) {
    return true;
  }
  if (!bai_sync_data->initial_phase) {
    return false;
  }
  // The next round will be laid out when the round BAI_SCHEDULE_ADAPTIVE_LAG
  // + 1 before it folds; it is adaptive if the initial phase has folded by
  // then.
  const uint64_t round_number = bai_sync_data->next_round_to_schedule;
  return round_number < BAI_SCHEDULE_ADAPTIVE_LAG + 1 ||
         round_number - BAI_SCHEDULE_ADAPTIVE_LAG - 1 <
             bai_sync_data->initial_last_round;
}

// The speculation_index cell for arm_index's sample at ordinal.
static inline int *bai_speculation_index_cell(BAISyncData *bai_sync_data,
                                              const int arm_index,
                                              const uint64_t ordinal) {
  return &bai_sync_data
              ->speculation_index[(size_t)arm_index * BAI_SPECULATION_SLOTS +
                                  (size_t)(ordinal % BAI_SPECULATION_SLOTS)];
}

// Assumes the caller has locked the bai sync data mutex. Returns the cache
// entry holding arm_index's sample at ordinal, or -1. A cell can point at an
// entry since emptied, reused, or overwritten for a later ordinal, so the
// entry itself must match.
static inline int bai_speculation_find(BAISyncData *bai_sync_data,
                                       const int arm_index,
                                       const uint64_t ordinal) {
  const int spec_idx =
      *bai_speculation_index_cell(bai_sync_data, arm_index, ordinal);
  if (spec_idx < 0) {
    return -1;
  }
  const BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
  if (speculation->state == BAI_SPECULATION_EMPTY ||
      speculation->arm_index != arm_index || speculation->ordinal != ordinal) {
    return -1;
  }
  return spec_idx;
}

// Assumes the caller has locked the bai sync data mutex. Empties a cache
// entry that holds a sample, and clears its index cell unless a later entry
// has taken it.
static inline void bai_speculation_release(BAISyncData *bai_sync_data,
                                           const int spec_idx) {
  BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
  int *cell = bai_speculation_index_cell(bai_sync_data, speculation->arm_index,
                                         speculation->ordinal);
  if (*cell == spec_idx) {
    *cell = -1;
  }
  speculation->state = BAI_SPECULATION_EMPTY;
}

// Assumes the caller has locked the bai sync data mutex. Hands a speculative
// sample matching (arm_index, ordinal), if there is one, to slot of the round
// being laid out: a finished one fills the slot in, a running one adopts it.
// Either way no worker will claim the slot. Returns true if one matched.
static inline bool bai_speculation_match_while_locked(
    BAISyncData *bai_sync_data, BAIRound *round, const uint64_t round_number,
    const int slot, const int arm_index, const uint64_t ordinal) {
  if (bai_sync_data->speculations == NULL) {
    return false;
  }
  const int spec_idx = bai_speculation_find(bai_sync_data, arm_index, ordinal);
  if (spec_idx < 0) {
    return false;
  }
  BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
  if (speculation->seed != round->seeds[slot]) {
    log_fatal("speculative sample of arm %d ordinal %llu has a stale seed",
              arm_index, (unsigned long long)ordinal);
  }
  round->slot_state[slot] = BAI_SLOT_TAKEN;
  if (speculation->state == BAI_SPECULATION_DONE) {
    round->slot_state[slot] = BAI_SLOT_DONE;
    round->values[slot] = speculation->value;
    if (round->records != NULL) {
      memcpy(round->records + (size_t)slot * bai_sync_data->record_size,
             speculation->record, bai_sync_data->record_size);
    }
    round->num_completed++;
    bai_speculation_release(bai_sync_data, spec_idx);
#ifdef BAI_SCHED_STATS
    bai_sync_data->sched_stats.speculation_hits++;
#endif
  } else {
    speculation->adopted = true;
    speculation->round_number = round_number;
    speculation->slot = slot;
  }
  return true;
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. Lays out the next round and charges it to the budget. The
// budget is the sample limit, but never less than the initial phase, which
// always runs to completion. Rounds scheduled before the initial phase has
// folded, and every round under the round-robin rule, cycle through the arms
// in global sample order; the rest follow the top-two rule from the
// committed statistics. Returns false when it lays out nothing: the budget is
// spent, or every arm still in is at the per-arm limit, until a fold brings
// an arm back (see bai_update_elimination).
static inline bool bai_schedule_round_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
#ifdef BAI_SCHED_STATS
  const int64_t sched_start_ns = bai_sched_now();
#endif
  if (bai_sync_data->scheduling_finished) {
    return false;
  }
  const uint64_t budget = args->sample_limit > bai_sync_data->initial_limit
                              ? args->sample_limit
                              : bai_sync_data->initial_limit;
  const uint64_t requested = bai_sync_data->num_total_samples_requested;
  if (requested >= budget) {
    bai_sync_data->scheduling_finished = true;
    return false;
  }
  // A round never straddles the end of the initial phase, so the fold that
  // completes it runs before any later sample is handed out.
  uint64_t round_end = budget;
  if (requested < bai_sync_data->initial_limit &&
      bai_sync_data->initial_limit < round_end) {
    round_end = bai_sync_data->initial_limit;
  }
  int num_samples = bai_sync_data->round_size;
  if ((uint64_t)num_samples > round_end - requested) {
    num_samples = (int)(round_end - requested);
  }
  BAIRound *round =
      &bai_sync_data->rounds[bai_sync_data->next_round_to_schedule %
                             BAI_SCHEDULE_ROUNDS];
  if (bai_sync_data->initial_phase ||
      args->sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN) {
    num_samples = bai_round_robin_arms(bai_sync_data, requested, num_samples,
                                       round->arm_indices);
    if (num_samples == 0) {
      return false;
    }
    round->claim_batch = BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH;
  } else {
    bai_schedule_top_two_arms(args, num_samples, round->arm_indices);
    round->claim_batch = 1;
  }
  round->num_samples = num_samples;
  round->num_claimed = 0;
  round->num_completed = 0;
  const uint64_t round_number = bai_sync_data->next_round_to_schedule;
  int num_matched = 0;
  for (int slot = 0; slot < num_samples; slot++) {
    const int arm_index = round->arm_indices[slot];
    BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    round->seeds[slot] = rvs_next_seed(args->rvs, (uint64_t)arm_index);
    round->slot_state[slot] = BAI_SLOT_FREE;
    if (bai_speculation_match_while_locked(bai_sync_data, round, round_number,
                                           slot, arm_index,
                                           arm_datum->num_scheduled)) {
      num_matched++;
    }
    arm_datum->num_scheduled++;
  }
  bai_sync_data->num_total_samples_requested += (uint64_t)num_samples;
  bai_sync_data->num_unclaimed += (uint64_t)(num_samples - num_matched);
  bai_sync_data->next_round_to_schedule++;
  bai_sync_data->schedule_epoch++;
  if (requested < bai_sync_data->initial_limit &&
      bai_sync_data->num_total_samples_requested >=
          bai_sync_data->initial_limit) {
    bai_sync_data->initial_last_round = round_number;
  }
  if (bai_sync_data->num_total_samples_requested >= budget) {
    // This round spends the last of the budget. The sample-limit status is
    // recorded when it folds, but no worker needs to wait for that.
    bai_sync_data->scheduling_finished = true;
  }
#ifdef BAI_SCHED_STATS
  round->committed_at_schedule = bai_sync_data->num_total_samples_completed;
  bai_sync_data->sched_stats.schedule_ns += bai_sched_now() - sched_start_ns;
#endif
  return true;
}

// Assumes the caller has locked the bai sync data mutex, or is the only
// thread running. Lays out every round the folds so far allow: round j once
// round j - lag - 1 has folded, with the round-robin lag for initial-phase
// rounds and the adaptive lag for the rest. Since folds are in order, each
// round is laid out from the statistics of one fixed fold.
static inline void bai_schedule_rounds_while_locked(BAISampleArgs *args) {
  const BAISyncData *bai_sync_data = args->bai_sync_data;
  while (!bai_sync_data->scheduling_finished) {
    const uint64_t lag = bai_sync_data->num_total_samples_requested <
                                 bai_sync_data->initial_limit
                             ? BAI_SCHEDULE_ROUND_ROBIN_LAG
                             : BAI_SCHEDULE_ADAPTIVE_LAG;
    if (bai_sync_data->next_round_to_schedule >
        bai_sync_data->next_round_to_fold + lag) {
      return;
    }
    if (!bai_schedule_round_while_locked(args)) {
      return;
    }
  }
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
//
// Once the last round is scheduled, a claim takes at most half of an even
// share of what is left, so that the sim does not end with one worker still
// running a whole batch while the others have nothing to do. Which worker
// runs which slot never changes what a slot samples, so the claim size can
// depend on the thread count without affecting the results.
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
    while (round->num_claimed < round->num_samples &&
           round->slot_state[round->num_claimed] != BAI_SLOT_FREE) {
      round->num_claimed++;
    }
    if (round->num_claimed < round->num_samples) {
      int max_slots = round->claim_batch;
      if (bai_sync_data->scheduling_finished) {
        const uint64_t share = bai_sync_data->num_unclaimed /
                               (2 * (uint64_t)bai_sync_data->num_threads);
        if (share < (uint64_t)max_slots) {
          max_slots = share > 0 ? (int)share : 1;
        }
      }
      int num_slots = 0;
      while (num_slots < max_slots &&
             round->num_claimed + num_slots < round->num_samples &&
             round->slot_state[round->num_claimed + num_slots] ==
                 BAI_SLOT_FREE) {
        round->slot_state[round->num_claimed + num_slots] = BAI_SLOT_TAKEN;
        num_slots++;
      }
      claim->round_number = bai_sync_data->next_round_to_claim;
      claim->first_slot = round->num_claimed;
      claim->num_slots = num_slots;
      round->num_claimed += num_slots;
      bai_sync_data->num_unclaimed -= (uint64_t)num_slots;
      while (round->num_claimed < round->num_samples &&
             round->slot_state[round->num_claimed] != BAI_SLOT_FREE) {
        round->num_claimed++;
      }
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

// Assumes the caller has locked the bai sync data mutex. Adds one finished
// slot's sample to its arm's accumulators and applies its record to the
// random variables' results. The arm's mean and variance are left stale.
static inline void bai_fold_slot_while_locked(BAISampleArgs *args,
                                              const BAIRound *round,
                                              const int slot) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  const int arm_index = round->arm_indices[slot];
  BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
  bai_accumulate_sample(arm_datum, round->values[slot]);
  if (round->records != NULL) {
    rvs_apply_sample_record(args->rvs, (uint64_t)arm_index,
                            round->records +
                                (size_t)slot * bai_sync_data->record_size);
  }
}

// Assumes the caller has locked the bai sync data mutex. Makes astar the arm
// with the highest committed mean, the lowest index on a tie, and returns that
// mean. Arms with no committed sample are skipped, so astar stays -1 until
// some arm has one.
static inline double bai_update_astar_while_locked(BAISyncData *bai_sync_data) {
  int astar_index = -1;
  double astar_mean = 0.0;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    const BAIArmDatum *arm_datum = &bai_sync_data->arm_data[arm_index];
    if (arm_datum->num_samples == 0) {
      continue;
    }
    if (astar_index < 0 || arm_datum->mean > astar_mean) {
      astar_index = arm_index;
      astar_mean = arm_datum->mean;
    }
  }
  bai_sync_data->astar_index = astar_index;
  return astar_mean;
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
    const uint64_t ordinal = bai_sync_data->arm_data[arm_index].num_samples;
    bai_fold_slot_while_locked(args, round, slot);
    bai_paired_record_while_locked(bai_sync_data, arm_index, ordinal,
                                   round->values[slot]);
  }
  for (int slot = 0; slot < round->num_samples; slot++) {
    bai_arm_datum_recompute_mean_and_var(
        &bai_sync_data->arm_data[round->arm_indices[slot]]);
  }
  bai_sync_data->num_total_samples_completed += (uint64_t)round->num_samples;
  bai_sync_data->schedule_epoch++;

  const double astar_mean = bai_update_astar_while_locked(bai_sync_data);

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
  if (bai_sync_data->eliminated != NULL) {
    bai_update_elimination(bai_sync_data, args->rvs, args->delta,
                           args->elimination_z);
  } else {
    bai_update_threshold(bai_sync_data, args->rvs, args->threshold,
                         args->sampling_rule, args->delta);
  }
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
  round->slot_state[slot] = BAI_SLOT_DONE;
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
    bai_schedule_rounds_while_locked(args);
  }
  if (!bai_sync_data->stopped && bai_sync_data->next_round_to_fold ==
                                     bai_sync_data->next_round_to_schedule) {
    // Every scheduled round has folded and nothing more can be laid out: the
    // budget is spent, or every arm still in is at the per-arm limit.
    bai_result_set_status(bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_SAMPLE_LIMIT);
    bai_sync_data->stopped = true;
    bai_sync_data->scheduling_finished = true;
  }
  if (folded) {
    cpthread_cond_broadcast(&bai_sync_data->work_available);
  }
}

// Assumes the caller has locked the bai sync data mutex. The clock or a user
// interrupt has cut the sim short: folds every sample that has finished in
// the rounds still live, in round and slot order, and stops the sim. Where a
// clock cuts a sim is not reproducible anyway, and folding only whole rounds
// would leave a sim stopped before its first round completed with no
// statistics at all, so its chosen move would owe nothing to the samples it
// ran. No stop condition is checked; the status already says why the sim
// stopped. Does nothing if the sim has already stopped.
static inline void bai_abandon_rounds_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  if (bai_sync_data->stopped) {
    return;
  }
  bool folded = false;
  for (uint64_t round_number = bai_sync_data->next_round_to_fold;
       round_number < bai_sync_data->next_round_to_schedule; round_number++) {
    const BAIRound *round =
        &bai_sync_data->rounds[round_number % BAI_SCHEDULE_ROUNDS];
    for (int slot = 0; slot < round->num_samples; slot++) {
      if (round->slot_state[slot] != BAI_SLOT_DONE) {
        continue;
      }
      bai_fold_slot_while_locked(args, round, slot);
      bai_arm_datum_recompute_mean_and_var(
          &bai_sync_data->arm_data[round->arm_indices[slot]]);
      bai_sync_data->num_total_samples_completed++;
      folded = true;
#ifdef BAI_SCHED_STATS
      bai_sync_data->sched_stats.folded_samples++;
#endif
    }
  }
  bai_sync_data->next_round_to_fold = bai_sync_data->next_round_to_schedule;
  if (folded) {
    bai_update_astar_while_locked(bai_sync_data);
  }
  bai_sync_data->stopped = true;
  bai_sync_data->scheduling_finished = true;
}

// The samples in a round: BAI_SCHEDULE_ROUND_SIZE draws' worth, to the
// nearest sample, and at least one.
static inline int bai_round_size(const uint64_t draws_per_sample) {
  if (draws_per_sample <= 1) {
    return BAI_SCHEDULE_ROUND_SIZE;
  }
  const uint64_t round_size =
      ((uint64_t)BAI_SCHEDULE_ROUND_SIZE + draws_per_sample / 2) /
      draws_per_sample;
  return round_size > 0 ? (int)round_size : 1;
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
  const size_t round_size = (size_t)bai_sync_data->round_size;
  for (int i = 0; i < BAI_SCHEDULE_ROUNDS; i++) {
    bai_sync_data->rounds[i].arm_indices =
        malloc_or_die(sizeof(int) * round_size);
    bai_sync_data->rounds[i].seeds =
        malloc_or_die(sizeof(uint64_t) * round_size);
    bai_sync_data->rounds[i].values =
        malloc_or_die(sizeof(double) * round_size);
    bai_sync_data->rounds[i].slot_state = malloc_or_die(round_size);
    if (bai_sync_data->record_size > 0) {
      bai_sync_data->rounds[i].records =
          malloc_or_die(bai_sync_data->record_size * round_size);
    }
  }
  // Speculation needs a sample to be a function of its slot, which holds
  // only for random variables that draw from seeds a slot can reserve.
  if (bai_sync_data->num_arms > 0 &&
      rvs_peek_seed(args->rvs, 0, 0) != RVS_SEED_UNRESERVED) {
    bai_sync_data->num_speculations =
        BAI_SPECULATIONS_PER_THREAD * bai_sync_data->num_threads;
    if (bai_sync_data->num_speculations > BAI_SPECULATIONS_MAX) {
      bai_sync_data->num_speculations = BAI_SPECULATIONS_MAX;
    }
    bai_sync_data->speculations = malloc_or_die(
        sizeof(BAISpeculation) * (size_t)bai_sync_data->num_speculations);
    const size_t num_index_cells =
        (size_t)bai_sync_data->num_arms * BAI_SPECULATION_SLOTS;
    bai_sync_data->speculation_index =
        malloc_or_die(sizeof(int) * num_index_cells);
    for (size_t cell_idx = 0; cell_idx < num_index_cells; cell_idx++) {
      bai_sync_data->speculation_index[cell_idx] = -1;
    }
    bai_sync_data->speculation_arm_count =
        malloc_or_die(sizeof(uint64_t) * (size_t)bai_sync_data->num_arms);
    for (int spec_idx = 0; spec_idx < bai_sync_data->num_speculations;
         spec_idx++) {
      BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
      speculation->state = BAI_SPECULATION_EMPTY;
      speculation->record = bai_sync_data->record_size > 0
                                ? malloc_or_die(bai_sync_data->record_size)
                                : NULL;
    }
  }
  bai_schedule_rounds_while_locked(args);
}

// Assumes the caller has locked the bai sync data mutex. Lays out the
// BAI_SPECULATION_SLOTS slots the next rounds are expected to hold: exactly,
// when they will be round-robin, or as the top-two rule would lay them out
// from the statistics committed so far, round after round. Returns how many
// slots it laid out: fewer when every arm still in reaches the per-arm limit,
// and 0 when there is nothing sound to predict from yet.
static inline int bai_speculation_expected_arms(BAISampleArgs *args,
                                                int *arm_indices) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  if (bai_next_round_is_round_robin(args)) {
    return bai_round_robin_arms(bai_sync_data,
                                bai_sync_data->num_total_samples_requested,
                                BAI_SPECULATION_SLOTS, arm_indices);
  }
  // The top-two rule reads every arm's mean, so every arm needs a committed
  // sample. The initial phase is iteration-major, so once num_arms samples
  // have folded each arm has one.
  if (bai_sync_data->astar_index < 0 ||
      bai_sync_data->num_total_samples_completed <
          (uint64_t)bai_sync_data->num_arms) {
    return 0;
  }
  // Each predicted round counts the ones predicted before it, as scheduling
  // will; the counts are put back afterwards.
  for (int first_slot = 0; first_slot < BAI_SPECULATION_SLOTS;
       first_slot += bai_sync_data->round_size) {
    int num_slots = BAI_SPECULATION_SLOTS - first_slot;
    if (num_slots > bai_sync_data->round_size) {
      num_slots = bai_sync_data->round_size;
    }
    int *round_arms = arm_indices + first_slot;
    bai_schedule_top_two_arms(args, num_slots, round_arms);
    for (int slot = 0; slot < num_slots; slot++) {
      bai_sync_data->arm_data[round_arms[slot]].num_scheduled++;
    }
  }
  for (int slot = 0; slot < BAI_SPECULATION_SLOTS; slot++) {
    bai_sync_data->arm_data[arm_indices[slot]].num_scheduled--;
  }
  return BAI_SPECULATION_SLOTS;
}

// Assumes the caller has locked the bai sync data mutex. Recomputes the
// expected slots and which of them speculations already hold.
static inline void bai_speculation_refresh_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  bai_sync_data->speculation_epoch = bai_sync_data->schedule_epoch;
  bai_sync_data->speculation_cursor = 0;
  const int num_expected =
      bai_speculation_expected_arms(args, bai_sync_data->speculation_arms);
  bai_sync_data->speculation_valid = num_expected > 0;
  // Slots past the budget, or past the per-arm limits, will never be laid
  // out, so they count as covered and are never speculated on.
  const uint64_t budget = args->sample_limit > bai_sync_data->initial_limit
                              ? args->sample_limit
                              : bai_sync_data->initial_limit;
  const uint64_t remaining =
      budget - bai_sync_data->num_total_samples_requested;
  for (int slot = 0; slot < BAI_SPECULATION_SLOTS; slot++) {
    bai_sync_data->speculation_covered[slot] =
        (uint64_t)slot >= remaining || slot >= num_expected;
  }
  for (int spec_idx = 0; spec_idx < bai_sync_data->num_speculations;
       spec_idx++) {
    bai_sync_data->speculations[spec_idx].expected = false;
  }
  if (!bai_sync_data->speculation_valid) {
    return;
  }
  uint64_t *arm_count = bai_sync_data->speculation_arm_count;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    arm_count[arm_index] = 0;
  }
  // The k-th expected slot of arm a will hold a's sample at ordinal
  // num_scheduled + k. An entry already holding that sample covers the slot.
  for (int slot = 0; slot < num_expected; slot++) {
    const int arm_index = bai_sync_data->speculation_arms[slot];
    const uint64_t ordinal = bai_sync_data->arm_data[arm_index].num_scheduled +
                             arm_count[arm_index]++;
    bai_sync_data->speculation_ordinals[slot] = ordinal;
    const int spec_idx =
        bai_speculation_find(bai_sync_data, arm_index, ordinal);
    if (spec_idx >= 0) {
      bai_sync_data->speculations[spec_idx].expected = true;
      bai_sync_data->speculation_covered[slot] = true;
    }
  }
}

// Assumes the caller has locked the bai sync data mutex. Reserves a cache
// entry for the first expected slot nobody has speculated on yet and returns
// its index, or -1 if there is none or no entry to put it in. A finished
// entry that is no longer expected is recycled.
static inline int bai_speculation_claim_while_locked(BAISampleArgs *args) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  if (bai_sync_data->speculations == NULL || bai_sync_data->stopped ||
      bai_sync_data->scheduling_finished) {
    return -1;
  }
  if (bai_sync_data->speculation_epoch != bai_sync_data->schedule_epoch) {
    bai_speculation_refresh_while_locked(args);
  }
  if (!bai_sync_data->speculation_valid) {
    return -1;
  }
  int slot = bai_sync_data->speculation_cursor;
  while (slot < BAI_SPECULATION_SLOTS &&
         bai_sync_data->speculation_covered[slot]) {
    slot++;
  }
  bai_sync_data->speculation_cursor = slot;
  if (slot == BAI_SPECULATION_SLOTS) {
    return -1;
  }
  int entry_idx = -1;
  for (int spec_idx = 0; spec_idx < bai_sync_data->num_speculations;
       spec_idx++) {
    const BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
    if (speculation->state == BAI_SPECULATION_EMPTY) {
      entry_idx = spec_idx;
      break;
    }
    if (speculation->state == BAI_SPECULATION_DONE && !speculation->expected &&
        entry_idx < 0) {
      entry_idx = spec_idx;
    }
  }
  if (entry_idx < 0) {
    return -1;
  }
  const int arm_index = bai_sync_data->speculation_arms[slot];
  const uint64_t ordinal = bai_sync_data->speculation_ordinals[slot];
  BAISpeculation *speculation = &bai_sync_data->speculations[entry_idx];
  if (speculation->state != BAI_SPECULATION_EMPTY) {
    // Recycling a finished sample no round expects any more.
    bai_speculation_release(bai_sync_data, entry_idx);
  }
  speculation->state = BAI_SPECULATION_RUNNING;
  speculation->arm_index = arm_index;
  speculation->ordinal = ordinal;
  speculation->seed =
      rvs_peek_seed(args->rvs, (uint64_t)arm_index,
                    ordinal - bai_sync_data->arm_data[arm_index].num_scheduled);
  speculation->adopted = false;
  speculation->expected = true;
  *bai_speculation_index_cell(bai_sync_data, arm_index, ordinal) = entry_idx;
  bai_sync_data->speculation_covered[slot] = true;
  bai_sync_data->speculation_cursor = slot + 1;
#ifdef BAI_SCHED_STATS
  bai_sync_data->sched_stats.speculations++;
#endif
  return entry_idx;
}

// Assumes the caller has locked the bai sync data mutex. Stores a finished
// speculative sample, or, if a round adopted it meanwhile, completes that
// round's slot with it. Discards it if the sim has stopped.
static inline void bai_speculation_finish_while_locked(BAISampleArgs *args,
                                                       const int spec_idx,
                                                       const double value) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  BAISpeculation *speculation = &bai_sync_data->speculations[spec_idx];
  if (bai_sync_data->stopped) {
    bai_speculation_release(bai_sync_data, spec_idx);
    return;
  }
  if (!speculation->adopted) {
    speculation->value = value;
    speculation->state = BAI_SPECULATION_DONE;
    return;
  }
  BAIRound *round =
      &bai_sync_data->rounds[speculation->round_number % BAI_SCHEDULE_ROUNDS];
  if (round->records != NULL) {
    memcpy(round->records +
               (size_t)speculation->slot * bai_sync_data->record_size,
           speculation->record, bai_sync_data->record_size);
  }
  bai_speculation_release(bai_sync_data, spec_idx);
#ifdef BAI_SCHED_STATS
  bai_sync_data->sched_stats.speculation_hits++;
#endif
  bai_schedule_complete_while_locked(args, speculation->round_number,
                                     speculation->slot, value);
}

typedef struct BAIWorkerArgs {
  BAISyncData *sync_data;
  RandomVariables *rvs;
  const BAIOptions *bai_options;
  BAILogger *bai_logger;
  Checkpoint *avoid_prune_checkpoint;
  // The move's budget, from the thread that started the simulation: each
  // worker charges it (gen_set_movegen_budget).
  MovegenBudget *movegen_budget;
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
      .elimination_z = bai_options->elimination_z,
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
  // A spent movegen budget ends the simulation as its clock would: the best
  // arm so far is the move.
  if (gen_movegen_budget_spent()) {
    bai_result_set_status_if_none(bai_result, BAI_RESULT_STATUS_MOVEGEN_BUDGET);
  }
  const bai_result_status_t status = bai_result_set_and_get_status(
      bai_result, thread_control_get_status(thread_control) ==
                      THREAD_CONTROL_STATUS_USER_INTERRUPT);
  return status == BAI_RESULT_STATUS_USER_INTERRUPT ||
         status == BAI_RESULT_STATUS_TIMEOUT ||
         status == BAI_RESULT_STATUS_MOVEGEN_BUDGET;
}

// Samples the claimed slots in order into sample_values, without the mutex.
// A round-robin claim holds up to a batch of slots, so the clock is checked
// before each one after the first: a timeout or interrupt stops this worker
// within one sample rather than a whole batch, and the slots it leaves
// unsampled are abandoned with the rest of the sim. Returns how many slots it
// sampled and sets *abandoned when it stopped early. worker_stats is written
// only in BAI_SCHED_STATS builds.
// cppcheck-suppress constParameterPointer
static inline int bai_worker_sample_claim(BAIWorkerSchedStats *worker_stats,
                                          const BAIWorkerArgs *bai_worker_args,
                                          const BAIClaim *claim,
                                          double *sample_values,
                                          bool *abandoned) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  const int rvs_thread_index = bai_worker_rvs_thread_index(bai_worker_args);
  // The claimed slots were laid out before they were handed out, under the
  // mutex, and nothing rewrites them until the round folds, which needs
  // these very samples; they are safe to read without the lock.
  const BAIRound *round =
      &sync_data->rounds[claim->round_number % BAI_SCHEDULE_ROUNDS];
  int num_sampled = 0;
  while (num_sampled < claim->num_slots) {
    if (num_sampled > 0 &&
        bai_should_abandon_rounds(sync_data->bai_result,
                                  sync_data->thread_control)) {
      *abandoned = true;
      break;
    }
    const int slot = claim->first_slot + num_sampled;
    unsigned char *record =
        round->records != NULL
            ? round->records + (size_t)slot * sync_data->record_size
            : NULL;
    const int64_t sample_start_ns = bai_sched_now();
    sample_values[num_sampled] = rvs_sample_with_seed(
        bai_worker_args->rvs, (uint64_t)round->arm_indices[slot],
        round->seeds[slot], rvs_thread_index, NULL, record);
    bai_sched_sample_done(worker_stats, sample_start_ns);
    num_sampled++;
  }
  return num_sampled;
}

// The worker holds the mutex at the top of each pass: it hands in the samples
// it finished and claims its next ones under one lock, so a sample costs one
// acquisition rather than two. The clock and the interrupt are checked
// outside the lock, after each sample and after each wait, so the worker
// never claims once the sim has been cut short.
static inline void bai_worker_round_loop(BAIWorkerArgs *bai_worker_args,
                                         BAIWorkerSchedStats *worker_stats) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = sync_data->thread_control;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int rvs_thread_index = bai_worker_rvs_thread_index(bai_worker_args);

  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, bai_worker_args->bai_options);

  double sample_values[BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH];
  bool abandoned =
      bai_should_abandon_rounds(sync_data->bai_result, thread_control);
  bai_sync_lock(sync_data, worker_stats);
  while (!abandoned) {
    BAIClaim claim;
    if (!bai_schedule_claim_while_locked(sync_data, &claim)) {
      if (sync_data->scheduling_finished) {
        break;
      }
      // Every scheduled slot is taken but a round is still in flight, so
      // nothing can be scheduled until it folds. Run a sample the next round
      // is likely to hold instead of waiting.
      const int spec_idx = bai_speculation_claim_while_locked(&sample_args);
      if (spec_idx >= 0) {
        BAISpeculation *speculation = &sync_data->speculations[spec_idx];
        const int arm_index = speculation->arm_index;
        const uint64_t seed = speculation->seed;
        unsigned char *record = speculation->record;
        cpthread_mutex_unlock(&sync_data->mutex);
        bai_sched_idle_end(worker_stats);
        const int64_t sample_start_ns = bai_sched_now();
        const double sample = rvs_sample_with_seed(
            rvs, (uint64_t)arm_index, seed, rvs_thread_index, NULL, record);
        bai_sched_sample_done(worker_stats, sample_start_ns);
        abandoned =
            bai_should_abandon_rounds(sync_data->bai_result, thread_control);
        bai_sync_lock(sync_data, worker_stats);
        bai_speculation_finish_while_locked(&sample_args, spec_idx, sample);
        continue;
      }
      // Nothing worth speculating on either. Sleep until a fold (or an
      // abandoning worker) signals, rather than spinning on the mutex.
      bai_sched_idle_begin(worker_stats);
      cpthread_cond_wait(&sync_data->work_available, &sync_data->mutex);
      abandoned =
          bai_should_abandon_rounds(sync_data->bai_result, thread_control);
      continue;
    }
    cpthread_mutex_unlock(&sync_data->mutex);
    bai_sched_idle_end(worker_stats);
    const int num_sampled = bai_worker_sample_claim(
        worker_stats, bai_worker_args, &claim, sample_values, &abandoned);
    if (!abandoned) {
      abandoned =
          bai_should_abandon_rounds(sync_data->bai_result, thread_control);
    }
    bai_sync_lock(sync_data, worker_stats);
    for (int claim_idx = 0; claim_idx < num_sampled; claim_idx++) {
      bai_schedule_complete_while_locked(&sample_args, claim.round_number,
                                         claim.first_slot + claim_idx,
                                         sample_values[claim_idx]);
    }
  }
  bai_sched_idle_end(worker_stats);
  if (abandoned) {
    bai_abandon_rounds_while_locked(&sample_args);
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
  if (stats->first_claim_ns != 0) {
    sim_stats->startup_ns += stats->first_claim_ns - sync_data->sched_start_ns;
  }
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
  sim_stats->lag_rounds = BAI_SCHEDULE_ADAPTIVE_LAG;
  sim_stats->sample_limit = bai_options->sample_limit;
  sim_stats->bai_samples =
      sync_data->sched_completions > sim_stats->initial_samples
          ? sync_data->sched_completions - sim_stats->initial_samples
          : 0;
  sim_stats->abandoned_samples =
      sync_data->sched_completions > sim_stats->folded_samples
          ? sync_data->sched_completions - sim_stats->folded_samples
          : 0;
  const int64_t end_ns = bai_sched_now();
  sim_stats->wall_ns = end_ns - start_ns;
  sim_stats->end_ns = (end_ns - sync_data->sched_last_exit_ns) *
                      (int64_t)sync_data->sched_num_exits;
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
  (void)gen_set_movegen_budget(bai_worker_args->movegen_budget);
  BAIWorkerSchedStats worker_stats;
  memset(&worker_stats, 0, sizeof(worker_stats));
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
  (void)gen_set_movegen_budget(NULL);
  return NULL;
}

// Assumes rvs are normally distributed.
static inline void bai(const BAIOptions *bai_options, RandomVariables *rvs,
                       ThreadControl *thread_control, BAILogger *bai_logger,
                       BAIResult *bai_result) {
#ifdef BAI_SCHED_STATS
  const int64_t sched_start_ns = bai_sched_now();
#endif
  bai_result_reset(bai_result, bai_options->time_limit_seconds);

  BAISyncData *sync_data = bai_sync_data_create(bai_result, thread_control,
                                                (int)rvs_get_num_rvs(rvs));
  sync_data->record_size = rvs_get_sample_record_size(rvs);
  sync_data->num_threads = bai_options->num_threads;
  sync_data->round_size = bai_round_size(bai_options->draws_per_sample);
  const bool eliminate = bai_options->eliminate && sync_data->num_arms > 1 &&
                         (bai_options->threshold != BAI_THRESHOLD_NONE ||
                          bai_options->elimination_z > 0);
  // Only a stopping test reads the paired statistics, and they only mean
  // something when the arms share seeds.
  if ((bai_options->threshold != BAI_THRESHOLD_NONE || eliminate) &&
      rvs_arms_share_seeds(rvs) && sync_data->num_arms > 1) {
    bai_sync_data_enable_paired(sync_data);
  }
  if (eliminate) {
    sync_data->eliminated =
        calloc_or_die((size_t)sync_data->num_arms, sizeof(bool));
  }
  // The per-arm limit never cuts the initial phase short.
  if (bai_options->sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN &&
      bai_options->arm_sample_limit > 0) {
    sync_data->arm_sample_limit =
        bai_options->arm_sample_limit > bai_options->sample_minimum
            ? bai_options->arm_sample_limit
            : bai_options->sample_minimum;
  }
#ifdef BAI_SCHED_STATS
  sync_data->sched_start_ns = sched_start_ns;
#endif

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
      .movegen_budget = gen_get_movegen_budget(),
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

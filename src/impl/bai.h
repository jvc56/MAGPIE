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
#include "../def/bai_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/analysis_progress.h"
#include "../ent/bai_result.h"
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

#define MINIMUM_VARIANCE 1e-10

// Internal BAI structs

typedef struct BAIArmDatum {
  uint64_t num_samples;
  double samples_sum;
  double samples_squared_sum;
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
  int challenger_index;
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
  const AnalysisProgressListener *progress_listener;
  uint64_t next_progress_sample;
  double regret_stop_target;
  bool regret_stop_use_joint;
  double regret_cross_arm_correlation;
  double regret_calibration;
  uint64_t regret_check_interval;
  uint64_t regret_min_samples_per_arm;
  uint64_t next_regret_check_sample;
} BAISyncData;

static inline BAISyncData *
bai_sync_data_create(BAIResult *bai_result, ThreadControl *thread_control,
                     const int num_initial_arms, RandomVariables *rng,
                     const AnalysisProgressListener *progress_listener) {
  BAISyncData *bai_sync_data = malloc_or_die(sizeof(BAISyncData));
  bai_sync_data->num_arms = num_initial_arms;
  bai_sync_data->num_arms_reached_threshold = 0;
  bai_sync_data->num_total_samples_completed = 0;
  bai_sync_data->num_total_samples_requested = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->challenger_index = -1;
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
  bai_sync_data->thread_control = thread_control;
  bai_sync_data->bai_result = bai_result;
  bai_sync_data->avoid_prune_arms = NULL;
  bai_sync_data->avoid_prune_count = 0;
  bai_sync_data->avoid_prune_next_idx = 0;
  bai_sync_data->progress_listener = progress_listener;
  bai_sync_data->next_progress_sample =
      analysis_progress_is_enabled(progress_listener)
          ? progress_listener->checkpoint_interval
          : 0;
  bai_sync_data->regret_stop_target = 0.0;
  bai_sync_data->regret_stop_use_joint = false;
  bai_sync_data->regret_cross_arm_correlation = 0.0;
  bai_sync_data->regret_calibration = 1.0;
  bai_sync_data->regret_check_interval = 0;
  bai_sync_data->regret_min_samples_per_arm = 0;
  bai_sync_data->next_regret_check_sample = 0;
  return bai_sync_data;
}

static inline void bai_sync_data_destroy(BAISyncData *bai_sync_data) {
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    free(bai_sync_data->arm_data[i].Zs);
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

static inline double bai_get_arm_z(BAISyncData *bai_sync_data,
                                   const int astar_index,
                                   const int challenger_index) {
  const BAIArmDatum *astar_arm_data = &bai_sync_data->arm_data[astar_index];
  const BAIArmDatum *challenger_arm_data =
      &bai_sync_data->arm_data[challenger_index];
  const double alt_lambda = bai_alt_lambda(
      astar_arm_data->mean, astar_arm_data->var,
      (double)astar_arm_data->num_samples, challenger_arm_data->mean,
      challenger_arm_data->var, (double)challenger_arm_data->num_samples);
  const double d_astar =
      bai_d(astar_arm_data->mean, astar_arm_data->var, alt_lambda);
  const double d_a =
      bai_d(challenger_arm_data->mean, challenger_arm_data->var, alt_lambda);
  return (double)astar_arm_data->num_samples * d_astar +
         (double)challenger_arm_data->num_samples * d_a;
}

static inline double bai_standard_normal_pdf(const double value) {
  static const double inv_sqrt_two_pi = 0.39894228040143267794;
  return inv_sqrt_two_pi * exp(-0.5 * value * value);
}

static inline double bai_standard_normal_cdf(const double value) {
  static const double inv_sqrt_two = 0.70710678118654752440;
  return 0.5 * erfc(-value * inv_sqrt_two);
}

static inline bool
bai_regret_has_minimum_evidence(const BAISyncData *bai_sync_data) {
  if (bai_sync_data->astar_index < 0 || bai_sync_data->num_arms <= 0) {
    return false;
  }
  const uint64_t minimum_samples =
      bai_sync_data->regret_min_samples_per_arm > 2
          ? bai_sync_data->regret_min_samples_per_arm
          : 2;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (bai_sync_data->arm_data[arm_index].num_samples < minimum_samples) {
      return false;
    }
  }
  return true;
}

static inline double bai_arm_sample_variance(const BAIArmDatum *arm) {
  // BAIArmDatum stores the population-form empirical variance (M2 / n).
  return arm->var * (double)arm->num_samples / (double)(arm->num_samples - 1);
}

// Simulation arms consume identical scenario-seed prefixes. With the current
// scalar correlation model, Cov(mean_i, mean_j) is the paired sample
// covariance divided by the larger sample count. The common-scenario audit
// will determine whether this approximation is adequate before it controls
// stopping.
static inline double bai_mean_covariance(const BAISyncData *bai_sync_data,
                                         int first_index, int second_index) {
  const BAIArmDatum *first = &bai_sync_data->arm_data[first_index];
  const double first_sample_variance = bai_arm_sample_variance(first);
  if (first_index == second_index) {
    return first_sample_variance / (double)first->num_samples;
  }
  const BAIArmDatum *second = &bai_sync_data->arm_data[second_index];
  const double second_sample_variance = bai_arm_sample_variance(second);
  const double correlation =
      fmax(-0.99, fmin(0.99, bai_sync_data->regret_cross_arm_correlation));
  const uint64_t denominator = first->num_samples > second->num_samples
                                   ? first->num_samples
                                   : second->num_samples;
  return correlation * sqrt(first_sample_variance * second_sample_variance) /
         (double)denominator;
}

static inline double
bai_difference_standard_deviation(const BAISyncData *bai_sync_data,
                                  int first_index, int second_index) {
  double variance =
      bai_mean_covariance(bai_sync_data, first_index, first_index) +
      bai_mean_covariance(bai_sync_data, second_index, second_index) -
      2.0 * bai_mean_covariance(bai_sync_data, first_index, second_index);
  if (variance < MINIMUM_VARIANCE * MINIMUM_VARIANCE) {
    variance = MINIMUM_VARIANCE * MINIMUM_VARIANCE;
  }
  return sqrt(variance);
}

static inline double bai_regret_calibration(const BAISyncData *bai_sync_data) {
  return bai_sync_data->regret_calibration > 0.0
             ? bai_sync_data->regret_calibration
             : 1.0;
}

// Legacy expected-loss signal. Each challenger contributes
// E[(U_i - U_astar)+], and the maximum pairwise contribution is returned.
// This is a lower bound on E[(max_i U_i - U_astar)+] and remains the live
// stopping signal only until the joint estimator below is calibrated.
//
// Assumes the caller has locked bai_sync_data or all workers have joined.
static inline double
bai_estimate_expected_regret(const BAISyncData *bai_sync_data,
                             RandomVariables *rvs) {
  (void)rvs;
  const int best_index = bai_sync_data->astar_index;
  if (!bai_regret_has_minimum_evidence(bai_sync_data)) {
    return INFINITY;
  }
  const BAIArmDatum *best = &bai_sync_data->arm_data[best_index];
  // A one-sample arm has empirical variance zero. Treating the variance floor
  // as evidence in that state can make a plausible arm look certain and drive
  // estimated regret to zero. Regret is therefore unknown until every arm has
  // the same minimum evidence required by the stopping rule. This remains a
  // separate requirement from BAI's sampling policy: callers that choose a
  // smaller initial phase may still select a move, but they may not claim a
  // finite residual-regret estimate.
  double expected_regret = 0.0;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == best_index) {
      continue;
    }
    const BAIArmDatum *arm = &bai_sync_data->arm_data[arm_index];
    const double difference_sd =
        bai_difference_standard_deviation(bai_sync_data, best_index, arm_index);
    const double difference_mean = arm->mean - best->mean;
    const double z = difference_mean / difference_sd;
    double arm_regret = difference_sd * bai_standard_normal_pdf(z) +
                        difference_mean * bai_standard_normal_cdf(z);
    if (arm_regret < 0.0) {
      arm_regret = 0.0;
    }
    if (arm_regret > expected_regret) {
      expected_regret = arm_regret;
    }
  }
  return bai_regret_calibration(bai_sync_data) * expected_regret;
}

// Clark's recursive moment approximation for E[max_i U_i]. Unlike the legacy
// maximum of pairwise expectations, this accounts for the simultaneous chance
// that any of several near-tied challengers is better. Arms are combined in
// descending empirical-mean order to reduce the approximation's order
// dependence. The legacy lower bound is enforced numerically.
//
// This is emitted in shadow mode only. It must pass the common-scenario
// covariance and optional-stopping audits before it may stop a live search.
// Assumes the caller has locked bai_sync_data or all workers have joined.
static inline double
bai_estimate_joint_expected_regret(const BAISyncData *bai_sync_data,
                                   RandomVariables *rvs) {
  if (!bai_regret_has_minimum_evidence(bai_sync_data)) {
    return INFINITY;
  }

  const int num_arms = bai_sync_data->num_arms;
  int order[num_arms];
  for (int index = 0; index < num_arms; index++) {
    order[index] = index;
  }
  for (int index = 1; index < num_arms; index++) {
    const int arm_index = order[index];
    int insertion_index = index;
    while (insertion_index > 0 &&
           bai_sync_data->arm_data[order[insertion_index - 1]].mean <
               bai_sync_data->arm_data[arm_index].mean) {
      order[insertion_index] = order[insertion_index - 1];
      insertion_index--;
    }
    order[insertion_index] = arm_index;
  }

  const int first_index = order[0];
  double maximum_mean = bai_sync_data->arm_data[first_index].mean;
  double maximum_variance =
      bai_mean_covariance(bai_sync_data, first_index, first_index);
  double covariance_with_maximum[num_arms];
  for (int arm_index = 0; arm_index < num_arms; arm_index++) {
    covariance_with_maximum[arm_index] =
        bai_mean_covariance(bai_sync_data, first_index, arm_index);
  }

  for (int order_index = 1; order_index < num_arms; order_index++) {
    const int arm_index = order[order_index];
    const double arm_mean = bai_sync_data->arm_data[arm_index].mean;
    const double arm_variance =
        bai_mean_covariance(bai_sync_data, arm_index, arm_index);
    const double covariance = covariance_with_maximum[arm_index];
    double difference_variance =
        maximum_variance + arm_variance - 2.0 * covariance;
    double weight_maximum;
    double next_mean;
    double next_variance;
    if (difference_variance <= MINIMUM_VARIANCE * MINIMUM_VARIANCE) {
      if (maximum_mean == arm_mean) {
        weight_maximum = 0.5;
      } else {
        weight_maximum = maximum_mean > arm_mean ? 1.0 : 0.0;
      }
      next_mean = weight_maximum > 0.0 ? maximum_mean : arm_mean;
      next_variance = weight_maximum > 0.0 ? maximum_variance : arm_variance;
    } else {
      const double difference_sd = sqrt(difference_variance);
      const double alpha = (maximum_mean - arm_mean) / difference_sd;
      weight_maximum = bai_standard_normal_cdf(alpha);
      const double weight_arm = 1.0 - weight_maximum;
      const double density = bai_standard_normal_pdf(alpha);
      next_mean = maximum_mean * weight_maximum + arm_mean * weight_arm +
                  difference_sd * density;
      const double second_moment =
          (maximum_variance + maximum_mean * maximum_mean) * weight_maximum +
          (arm_variance + arm_mean * arm_mean) * weight_arm +
          (maximum_mean + arm_mean) * difference_sd * density;
      next_variance = second_moment - next_mean * next_mean;
      if (next_variance < MINIMUM_VARIANCE * MINIMUM_VARIANCE) {
        next_variance = MINIMUM_VARIANCE * MINIMUM_VARIANCE;
      }
    }

    for (int future_order_index = order_index + 1;
         future_order_index < num_arms; future_order_index++) {
      const int future_index = order[future_order_index];
      covariance_with_maximum[future_index] =
          weight_maximum * covariance_with_maximum[future_index] +
          (1.0 - weight_maximum) *
              bai_mean_covariance(bai_sync_data, arm_index, future_index);
    }
    maximum_mean = next_mean;
    maximum_variance = next_variance;
  }

  const double joint_regret =
      bai_regret_calibration(bai_sync_data) *
      fmax(0.0, maximum_mean -
                    bai_sync_data->arm_data[bai_sync_data->astar_index].mean);
  return fmax(joint_regret, bai_estimate_expected_regret(bai_sync_data, rvs));
}

// Number of challengers whose two-sided 99% Gaussian upper confidence bound
// on U_i - U_astar reaches zero. This is a diagnostic for the dimension of the
// live near-tie set, not a stopping rule.
static inline int
bai_count_near_tie_challengers(const BAISyncData *bai_sync_data) {
  static const double z_99_two_sided = 2.5758293035489004;
  if (!bai_regret_has_minimum_evidence(bai_sync_data)) {
    return -1;
  }
  const int best_index = bai_sync_data->astar_index;
  const double best_mean = bai_sync_data->arm_data[best_index].mean;
  int count = 0;
  for (int arm_index = 0; arm_index < bai_sync_data->num_arms; arm_index++) {
    if (arm_index == best_index) {
      continue;
    }
    const double difference_mean =
        bai_sync_data->arm_data[arm_index].mean - best_mean;
    const double difference_sd =
        bai_difference_standard_deviation(bai_sync_data, best_index, arm_index);
    if (difference_mean + z_99_two_sided * difference_sd >= 0.0) {
      count++;
    }
  }
  return count;
}

static inline double bai_regret_stop_estimate(const BAISyncData *bai_sync_data,
                                              double legacy_estimate,
                                              double joint_estimate) {
  return bai_sync_data->regret_stop_use_joint ? joint_estimate
                                              : legacy_estimate;
}

static inline int
bai_sync_data_sample_limit_reached(const BAISyncData *bai_sync_data,
                                   uint64_t sample_limit) {
  return bai_sync_data->num_total_samples_requested >= sample_limit;
}

// Assumes the caller has locked the bai sync data mutex
static inline int
bai_sync_data_get_next_bai_sample_index_while_locked(BAISampleArgs *args) {
  if (bai_sync_data_sample_limit_reached(args->bai_sync_data,
                                         args->sample_limit)) {
    bai_result_set_status(args->bai_sync_data->bai_result,
                          BAI_RESULT_STATUS_SAMPLE_LIMIT);
    return -1;
  }
  int arm_index;
  switch (args->sampling_rule) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    arm_index = args->bai_sync_data->num_total_samples_requested %
                (uint64_t)args->bai_sync_data->num_arms;
    break;
  case BAI_SAMPLING_RULE_TOP_TWO_IDS:;
    const int it = args->bai_sync_data->astar_index;
    const int jt = args->bai_sync_data->challenger_index;
    const double psi_it = (double)args->bai_sync_data->arm_data[it].num_samples;
    const double psi_jt = (double)args->bai_sync_data->arm_data[jt].num_samples;
    const double emp_mean_it = args->bai_sync_data->arm_data[it].mean;
    const double emp_mean_jt = args->bai_sync_data->arm_data[jt].mean;
    const double emp_var_it = args->bai_sync_data->arm_data[it].var;
    const double emp_var_jt = args->bai_sync_data->arm_data[jt].var;
    const double theta_bar =
        (psi_it * emp_mean_it + psi_jt * emp_mean_jt) / (psi_it + psi_jt);
    const double numerator = psi_it * bai_d(emp_mean_it, emp_var_it, theta_bar);
    const double denominator =
        numerator + psi_jt * bai_d(emp_mean_jt, emp_var_jt, theta_bar);
    const double coin = numerator / denominator;
    // Thread index can be anything here since this sample doesn't use
    // multithreading
    if (rvs_sample(args->bai_sync_data->rng, 0, 0, NULL) < coin) {
      arm_index = it;
    } else {
      arm_index = jt;
    }
    break;
  }
  args->bai_sync_data->num_total_samples_requested++;
  return arm_index;
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

__attribute__((always_inline)) static inline int
bai_sync_data_get_next_sample_index_while_locked(BAISampleArgs *args) {
  if (args->bai_sync_data->initial_phase) {
    return bai_sync_data_get_next_initial_sample_index_while_locked(args);
  }
  return bai_sync_data_get_next_bai_sample_index_while_locked(args);
}

__attribute__((always_inline)) static inline int
bai_sync_data_get_next_sample_index(BAISampleArgs *args) {
  int arm_index;
  cpthread_mutex_lock(&args->bai_sync_data->mutex);
  arm_index = bai_sync_data_get_next_sample_index_while_locked(args);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
  return arm_index;
}

// Assumes the caller has locked bai_sync_data or is the only thread running
static inline void bai_update_threshold_and_challenger(
    BAISyncData *bai_sync_data, RandomVariables *rvs, bai_threshold_t threshold,
    bai_sampling_rule_t sampling_rule, const double delta, const int arm_index,
    const bool update_all) {
  if (threshold == BAI_THRESHOLD_NONE &&
      sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN) {
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

  double challenger_value;
  switch (sampling_rule) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    break;
  case BAI_SAMPLING_RULE_TOP_TWO_IDS:
    bai_sync_data->challenger_index = -1;
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
    switch (sampling_rule) {
    case BAI_SAMPLING_RULE_ROUND_ROBIN:
      break;
    case BAI_SAMPLING_RULE_TOP_TWO_IDS:;
      double arm_challenger_value =
          arm_Z + log((double)bai_sync_data->arm_data[i].num_samples);
      if (bai_sync_data->challenger_index < 0 ||
          arm_challenger_value < challenger_value) {
        bai_sync_data->challenger_index = i;
        challenger_value = arm_challenger_value;
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
__attribute__((always_inline)) static inline void
bai_sync_data_add_sample_while_locked(BAISampleArgs *args, const int arm_index,
                                      const double sample_value) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  BAIArmDatum *arm_data = bai_sync_data->arm_data;
  bai_sync_data->num_total_samples_completed++;
  BAIArmDatum *sample_arm_datum = &arm_data[arm_index];
  sample_arm_datum->num_samples++;
  sample_arm_datum->samples_sum += sample_value;
  sample_arm_datum->samples_squared_sum += sample_value * sample_value;
  sample_arm_datum->mean =
      sample_arm_datum->samples_sum / sample_arm_datum->num_samples;
  sample_arm_datum->var =
      sample_arm_datum->samples_squared_sum / sample_arm_datum->num_samples -
      sample_arm_datum->mean * sample_arm_datum->mean;
  if (sample_arm_datum->var < MINIMUM_VARIANCE) {
    sample_arm_datum->var = MINIMUM_VARIANCE;
  }

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

    bai_update_threshold_and_challenger(bai_sync_data, args->rvs,
                                        args->threshold, args->sampling_rule,
                                        args->delta, arm_index, update_all);
  }
}

// Assumes the caller has locked bai_sync_data.
static inline void bai_maybe_stop_for_regret_while_locked(BAISampleArgs *args) {
  BAISyncData *sync_data = args->bai_sync_data;
  if (sync_data->regret_stop_target <= 0.0 || sync_data->initial_phase ||
      sync_data->next_regret_check_sample == 0 ||
      sync_data->num_total_samples_completed <
          sync_data->next_regret_check_sample) {
    return;
  }

  const uint64_t interval = sync_data->regret_check_interval;
  do {
    const uint64_t old_next = sync_data->next_regret_check_sample;
    sync_data->next_regret_check_sample += interval;
    if (sync_data->next_regret_check_sample < old_next) {
      sync_data->next_regret_check_sample = 0;
      break;
    }
  } while (sync_data->next_regret_check_sample > 0 &&
           sync_data->next_regret_check_sample <=
               sync_data->num_total_samples_completed);

  for (int arm_index = 0; arm_index < sync_data->num_arms; arm_index++) {
    if (sync_data->arm_data[arm_index].num_samples <
        sync_data->regret_min_samples_per_arm) {
      return;
    }
  }
  const double estimated_regret =
      bai_estimate_expected_regret(sync_data, args->rvs);
  const double joint_estimated_regret =
      bai_estimate_joint_expected_regret(sync_data, args->rvs);
  bai_result_set_estimated_regret(sync_data->bai_result, estimated_regret);
  bai_result_set_joint_estimated_regret(sync_data->bai_result,
                                        joint_estimated_regret);
  const double stopping_estimate = bai_regret_stop_estimate(
      sync_data, estimated_regret, joint_estimated_regret);
  if (stopping_estimate <= sync_data->regret_stop_target) {
    const int near_tie_challengers = bai_count_near_tie_challengers(sync_data);
    bai_result_set_regret_at_stop(sync_data->bai_result, estimated_regret);
    bai_result_set_joint_regret_at_stop(sync_data->bai_result,
                                        joint_estimated_regret);
    bai_result_set_near_tie_challengers_at_stop(sync_data->bai_result,
                                                near_tie_challengers);
    bai_result_set_status(sync_data->bai_result,
                          BAI_RESULT_STATUS_REGRET_LIMIT);
  }
}

static inline void bai_sync_data_add_sample(BAISampleArgs *args,
                                            const int arm_index,
                                            const double sample_value) {
  cpthread_mutex_lock(&args->bai_sync_data->mutex);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
}

static inline void bai_sync_data_add_sample_with_regret_stop(
    BAISampleArgs *args, const int arm_index, const double sample_value) {
  cpthread_mutex_lock(&args->bai_sync_data->mutex);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  bai_maybe_stop_for_regret_while_locked(args);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
}

// Keep the large copy-safe progress event out of the untraced worker's stack
// frame. Inlining this rare path makes every simulation worker reserve the
// event even when progress is disabled.
__attribute__((noinline)) static void
bai_sync_data_add_sample_with_progress(BAISampleArgs *args, const int arm_index,
                                       const double sample_value) {
  bool emit_progress = false;
  AnalysisProgressEvent progress;
  cpthread_mutex_lock(&args->bai_sync_data->mutex);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  bai_maybe_stop_for_regret_while_locked(args);
  BAISyncData *sync_data = args->bai_sync_data;
  if (sync_data->next_progress_sample > 0 &&
      sync_data->num_total_samples_completed >=
          sync_data->next_progress_sample) {
    progress = analysis_progress_event_create(ANALYSIS_MODE_SIM,
                                              ANALYSIS_EVENT_CHECKPOINT);
    const int best_index = sync_data->astar_index;
    const int challenger_index = sync_data->challenger_index;
    progress.elapsed_ns =
        sync_data->progress_listener->start_ns > 0
            ? ctimer_monotonic_ns() - sync_data->progress_listener->start_ns
            : 0;
    progress.work_units = sync_data->num_total_samples_completed;
    progress.iterations = sync_data->num_total_samples_completed;
    progress.candidate_index = arm_index;
    progress.candidates_total = sync_data->num_arms;
    progress.best_index = best_index;
    progress.challenger_index = challenger_index;
    progress.item_id = best_index >= 0 ? (uint64_t)best_index : 0;
    if (best_index >= 0) {
      progress.best_value = sync_data->arm_data[best_index].mean;
    }
    if (challenger_index >= 0) {
      progress.challenger_value = sync_data->arm_data[challenger_index].mean;
    }
    progress.value = bai_estimate_expected_regret(sync_data, args->rvs);
    progress.secondary_value =
        bai_estimate_joint_expected_regret(sync_data, args->rvs);
    const uint64_t interval = sync_data->progress_listener->checkpoint_interval;
    do {
      const uint64_t old_next = sync_data->next_progress_sample;
      sync_data->next_progress_sample += interval;
      if (sync_data->next_progress_sample < old_next) {
        sync_data->next_progress_sample = 0;
        break;
      }
    } while (sync_data->next_progress_sample > 0 &&
             sync_data->next_progress_sample <=
                 sync_data->num_total_samples_completed);
    emit_progress = true;
  }
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
  if (emit_progress) {
    analysis_progress_emit(sync_data->progress_listener, &progress);
  }
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
  bai_update_threshold_and_challenger(
      bai_sync_data, bai_worker_args->rvs,
      bai_worker_args->bai_options->threshold,
      bai_worker_args->bai_options->sampling_rule,
      bai_worker_args->bai_options->delta, bai_sync_data->astar_index, true);
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

// The traced loop is deliberately out of line so its additional control flow
// and stack use cannot perturb the untraced simulation inner loop.
__attribute__((noinline)) static void
bai_worker_sample_loop_with_progress(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = bai_worker_args->sync_data->thread_control;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
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
  const int rvs_thread_index =
      bai_options->parent_worker_thread_index + bai_thread_index;

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

  while (!bai_should_stop(sync_data->bai_result, thread_control)) {
    const int arm_index = bai_sync_data_get_next_sample_index(&sample_args);
    if (arm_index < 0) {
      break;
    }
    double sample = rvs_sample(rvs, arm_index, rvs_thread_index, NULL);
    bai_sync_data_add_sample_with_progress(&sample_args, arm_index, sample);
  }
}

// Separate from the original worker loop so disabled value-of-computation
// stopping adds no branch to the simulation inner loop.
__attribute__((noinline)) static void
bai_worker_sample_loop_with_regret_stop(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = sync_data->thread_control;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int bai_thread_index = bai_worker_args->thread_index;

  if (bai_thread_index > 0 && bai_options->parent_worker_thread_index > 0) {
    log_fatal("Both BAI worker thread index (%d) and parent worker "
              "thread index (%d) are greater than 0.",
              bai_thread_index, bai_options->parent_worker_thread_index);
  }
  const int rvs_thread_index =
      bai_options->parent_worker_thread_index + bai_thread_index;
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

  while (!bai_should_stop(sync_data->bai_result, thread_control)) {
    const int arm_index = bai_sync_data_get_next_sample_index(&sample_args);
    if (arm_index < 0) {
      break;
    }
    const double sample = rvs_sample(rvs, arm_index, rvs_thread_index, NULL);
    bai_sync_data_add_sample_with_regret_stop(&sample_args, arm_index, sample);
  }
}

static inline void bai_worker_sample_loop(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = bai_worker_args->sync_data->thread_control;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
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
  const int rvs_thread_index =
      bai_options->parent_worker_thread_index + bai_thread_index;

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

  // This is the original untraced sample-update loop. bai() selects the worker
  // entry point once, so progress never adds a branch inside an iteration.
  while (!bai_should_stop(sync_data->bai_result, thread_control)) {
    const int arm_index = bai_sync_data_get_next_sample_index(&sample_args);
    if (arm_index < 0) {
      break;
    }
    double sample = rvs_sample(rvs, arm_index, rvs_thread_index, NULL);
    bai_sync_data_add_sample(&sample_args, arm_index, sample);
  }
}

static inline void *bai_worker(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  bai_worker_sample_loop(bai_worker_args);
  checkpoint_wait(bai_worker_args->checkpoint, bai_worker_args);
  bai_worker_sample_loop(bai_worker_args);
  if (bai_worker_args->sync_data->avoid_prune_arms) {
    checkpoint_wait(bai_worker_args->avoid_prune_checkpoint, bai_worker_args);
    sim_unpruned_to_winner(bai_worker_args);
  }
  return NULL;
}

static void *bai_worker_with_progress(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  bai_worker_sample_loop_with_progress(bai_worker_args);
  checkpoint_wait(bai_worker_args->checkpoint, bai_worker_args);
  bai_worker_sample_loop_with_progress(bai_worker_args);
  if (bai_worker_args->sync_data->avoid_prune_arms) {
    checkpoint_wait(bai_worker_args->avoid_prune_checkpoint, bai_worker_args);
    sim_unpruned_to_winner(bai_worker_args);
  }
  return NULL;
}

static void *bai_worker_with_regret_stop(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  bai_worker_sample_loop_with_regret_stop(bai_worker_args);
  checkpoint_wait(bai_worker_args->checkpoint, bai_worker_args);
  bai_worker_sample_loop_with_regret_stop(bai_worker_args);
  if (bai_worker_args->sync_data->avoid_prune_arms) {
    checkpoint_wait(bai_worker_args->avoid_prune_checkpoint, bai_worker_args);
    sim_unpruned_to_winner(bai_worker_args);
  }
  return NULL;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
static inline void bai(const BAIOptions *bai_options, RandomVariables *rvs,
                       RandomVariables *rng, ThreadControl *thread_control,
                       BAILogger *bai_logger,
                       const AnalysisProgressListener *progress_listener,
                       BAIResult *bai_result) {
  bai_result_reset(bai_result, bai_options->time_limit_seconds);

  Checkpoint *checkpoint =
      checkpoint_create(bai_options->num_threads, bai_finish_initial_phase);

  BAISyncData *sync_data =
      bai_sync_data_create(bai_result, thread_control,
                           (int)rvs_get_num_rvs(rvs), rng, progress_listener);
  sync_data->regret_stop_target = bai_options->regret_stop_target;
  sync_data->regret_stop_use_joint = bai_options->regret_stop_use_joint;
  sync_data->regret_cross_arm_correlation =
      bai_options->regret_cross_arm_correlation;
  sync_data->regret_calibration = bai_options->regret_calibration;
  sync_data->regret_check_interval = bai_options->regret_check_interval > 0
                                         ? bai_options->regret_check_interval
                                         : 256;
  sync_data->regret_min_samples_per_arm =
      bai_options->regret_min_samples_per_arm >
              BAI_MINIMUM_REGRET_SAMPLES_PER_ARM
          ? bai_options->regret_min_samples_per_arm
          : BAI_MINIMUM_REGRET_SAMPLES_PER_ARM;
  if (sync_data->regret_stop_target > 0.0) {
    const uint64_t minimum_total =
        (uint64_t)sync_data->num_arms * sync_data->regret_min_samples_per_arm;
    sync_data->next_regret_check_sample =
        minimum_total > sync_data->regret_check_interval
            ? minimum_total
            : sync_data->regret_check_interval;
  }

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
  void *(*worker_start)(void *) = bai_worker;
  if (sync_data->next_progress_sample > 0) {
    worker_start = bai_worker_with_progress;
  } else if (sync_data->regret_stop_target > 0.0) {
    worker_start = bai_worker_with_regret_stop;
  }
  for (int thread_index = 0; thread_index < bai_options->num_threads;
       thread_index++) {
    bai_worker_args_array[thread_index] = bai_worker_args;
    bai_worker_args_array[thread_index].thread_index = thread_index;
    cpthread_create(&worker_ids[thread_index], worker_start,
                    &bai_worker_args_array[thread_index]);
  }
  for (int thread_index = 0; thread_index < bai_options->num_threads;
       thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }
  bai_result_set_estimated_regret(bai_result,
                                  bai_estimate_expected_regret(sync_data, rvs));
  bai_result_set_joint_estimated_regret(
      bai_result, bai_estimate_joint_expected_regret(sync_data, rvs));
  bai_result_set_near_tie_challengers(
      bai_result, bai_count_near_tie_challengers(sync_data));
  bai_result_set_best_arm(bai_result, sync_data->astar_index);
  bai_result_stop_timer(bai_result);
  if (analysis_progress_is_enabled(progress_listener)) {
    AnalysisProgressEvent progress = analysis_progress_event_create(
        ANALYSIS_MODE_SIM, ANALYSIS_EVENT_FINISH);
    progress.work_units = sync_data->num_total_samples_completed;
    progress.iterations = sync_data->num_total_samples_completed;
    progress.candidates_total = sync_data->num_arms;
    progress.best_index = sync_data->astar_index;
    progress.challenger_index = sync_data->challenger_index;
    if (sync_data->astar_index >= 0) {
      progress.item_id = (uint64_t)sync_data->astar_index;
      progress.best_value = sync_data->arm_data[sync_data->astar_index].mean;
    }
    if (sync_data->challenger_index >= 0) {
      progress.challenger_value =
          sync_data->arm_data[sync_data->challenger_index].mean;
    }
    progress.value = bai_result_get_estimated_regret(bai_result);
    progress.secondary_value =
        bai_result_get_joint_estimated_regret(bai_result);
    switch (bai_result_get_status(bai_result)) {
    case BAI_RESULT_STATUS_TIMEOUT:
      progress.status = ANALYSIS_STATUS_TIME_LIMIT;
      break;
    case BAI_RESULT_STATUS_USER_INTERRUPT:
      progress.status = ANALYSIS_STATUS_INTERRUPTED;
      break;
    case BAI_RESULT_STATUS_NONE:
      progress.status = ANALYSIS_STATUS_NO_RESULT;
      break;
    default:
      progress.status = ANALYSIS_STATUS_COMPLETED;
      break;
    }
    analysis_progress_emit(progress_listener, &progress);
  }
  free(bai_worker_args_array);
  free(worker_ids);
  bai_sync_data_destroy(sync_data);
  checkpoint_destroy(avoid_prune_checkpoint);
  checkpoint_destroy(checkpoint);
}

#endif

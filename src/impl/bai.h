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
#include "../ent/bai_result.h"
#include "../ent/checkpoint.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "bai_logger.h"
#include "random_variable.h"
#include <assert.h>
#include <limits.h>
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
} BAISyncData;

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
  bai_sync_data->challenger_index = -1;
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
  if (args->bai_sync_data->num_total_samples_requested >=
      args->bai_sync_data->num_arms * args->sample_minimum) {
    return -1;
  }
  return args->bai_sync_data->num_total_samples_requested++ /
         args->sample_minimum;
}

static inline int
bai_sync_data_get_next_sample_index_while_locked(BAISampleArgs *args) {
  if (args->bai_sync_data->initial_phase) {
    return bai_sync_data_get_next_initial_sample_index_while_locked(args);
  }
  return bai_sync_data_get_next_bai_sample_index_while_locked(args);
}

static inline int bai_sync_data_get_next_sample_index(BAISampleArgs *args) {
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
static inline void
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
  const bool update_all = old_astar_index == -1 ||
                          old_astar_index != bai_sync_data->astar_index ||
                          bai_sync_data->astar_index == arm_index;

  bai_update_threshold_and_challenger(bai_sync_data, args->rvs, args->threshold,
                                      args->sampling_rule, args->delta,
                                      arm_index, update_all);
}

static inline void bai_sync_data_add_sample(BAISampleArgs *args,
                                            const int arm_index,
                                            const double sample_value) {
  cpthread_mutex_lock(&args->bai_sync_data->mutex);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  cpthread_mutex_unlock(&args->bai_sync_data->mutex);
}

typedef struct BAIWorkerArgs {
  BAISyncData *sync_data;
  RandomVariables *rvs;
  const BAIOptions *bai_options;
  BAILogger *bai_logger;
  Checkpoint *checkpoint;
  int thread_index;
} BAIWorkerArgs;

static inline bool bai_should_stop(BAIResult *bai_result,
                                   ThreadControl *thread_control) {
  return bai_result_set_and_get_status(
             bai_result, thread_control_get_status(thread_control) ==
                             THREAD_CONTROL_STATUS_USER_INTERRUPT) !=
         BAI_RESULT_STATUS_NONE;
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
  bai_update_threshold_and_challenger(
      bai_sync_data, bai_worker_args->rvs,
      bai_worker_args->bai_options->threshold,
      bai_worker_args->bai_options->sampling_rule,
      bai_worker_args->bai_options->delta, bai_sync_data->astar_index, true);
}

static inline void bai_worker_sample_loop(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = bai_worker_args->sync_data->thread_control;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int thread_index = bai_worker_args->thread_index;

  BAISampleArgs sample_args = {
      .bai_sync_data = sync_data,
      .rvs = rvs,
      .delta = bai_options->delta,
      .sample_limit = bai_options->sample_limit,
      .sample_minimum = bai_options->sample_minimum,
      .sampling_rule = bai_options->sampling_rule,
      .threshold = bai_options->threshold,
  };

  while (!bai_should_stop(sync_data->bai_result, thread_control)) {
    const int arm_index = bai_sync_data_get_next_sample_index(&sample_args);
    if (arm_index < 0) {
      break;
    }
    double sample = rvs_sample(rvs, arm_index, thread_index, NULL);
    bai_sync_data_add_sample(&sample_args, arm_index, sample);
  }
}

static inline void *bai_worker(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  bai_worker_sample_loop(bai_worker_args);
  checkpoint_wait(bai_worker_args->checkpoint, bai_worker_args);
  bai_worker_sample_loop(bai_worker_args);
  return NULL;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
static inline void bai(const BAIOptions *bai_options, RandomVariables *rvs,
                       RandomVariables *rng, ThreadControl *thread_control,
                       BAILogger *bai_logger, BAIResult *bai_result) {
  // FIXME: rvs_reset was formerly called here, ensure that the reset happens at
  // some point
  bai_result_reset(bai_result, bai_options->time_limit_seconds);

  Checkpoint *checkpoint =
      checkpoint_create(bai_options->num_threads, bai_finish_initial_phase);

  BAISyncData *sync_data = bai_sync_data_create(bai_result, thread_control,
                                                (int)rvs_get_num_rvs(rvs), rng);

  BAIWorkerArgs bai_worker_args = {
      .sync_data = sync_data,
      .rvs = rvs,
      .bai_options = bai_options,
      .bai_logger = bai_logger,
      .checkpoint = checkpoint,
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
  free(bai_worker_args_array);
  free(worker_ids);
  bai_sync_data_destroy(sync_data);
  checkpoint_destroy(checkpoint);
}

#endif
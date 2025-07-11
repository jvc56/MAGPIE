#ifndef BAI_H
#define BAI_H

/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code (https://marcjourdan.netlify.app/publication/baiuv/)
 * kindly provided by Marc Jourdan.
 */

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>

#include "../def/bai_defs.h"
#include "../def/thread_control_defs.h"

#include "../ent/bai_result.h"
#include "../ent/checkpoint.h"
#include "../ent/thread_control.h"

#include "../util/io_util.h"

#include "bai_logger.h"
#include "random_variable.h"

#define MINIMUM_VARIANCE 1e-10

// Internal BAI structs

// FIXME: just make this whole thing a .h when done
typedef struct BAIArmDatum {
  int num_samples;
  double samples_sum;
  double samples_squared_sum;
  double mean;
  double var;
  bool is_epigon;
  double *Zs;
} BAIArmDatum;

typedef struct BAISyncData {
  int num_arms;
  int num_arms_reached_threshold;
  int num_total_samples_completed;
  int num_total_samples_requested;
  int astar_index;
  int challenger_index;
  bool initial_phase;
  exit_status_t exit_status;
  BAIArmDatum *arm_data;
  RandomVariables *rng;
  pthread_mutex_t mutex;
} BAISyncData;

static inline BAISyncData *bai_sync_data_create(const int num_initial_arms,
                                                RandomVariables *rng) {
  BAISyncData *bai_sync_data = malloc_or_die(sizeof(BAISyncData));
  bai_sync_data->num_arms = num_initial_arms;
  bai_sync_data->num_arms_reached_threshold = 0;
  bai_sync_data->num_total_samples_completed = 0;
  bai_sync_data->num_total_samples_requested = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->challenger_index = -1;
  bai_sync_data->initial_phase = true;
  bai_sync_data->exit_status = EXIT_STATUS_NONE;
  bai_sync_data->arm_data =
      calloc_or_die(num_initial_arms, sizeof(BAIArmDatum));
  for (int i = 0; i < num_initial_arms; i++) {
    bai_sync_data->arm_data[i].Zs =
        malloc_or_die(num_initial_arms * sizeof(double));
  }
  bai_sync_data->rng = rng;
  pthread_mutex_init(&bai_sync_data->mutex, NULL);
  return bai_sync_data;
}

static inline void bai_sync_data_destroy(BAISyncData *bai_sync_data) {
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    free(bai_sync_data->arm_data[i].Zs);
  }
  free(bai_sync_data->arm_data);
  free(bai_sync_data);
}

static inline void
bai_sync_data_set_exit_status_if_unset(BAISyncData *bai_sync_data,
                                       exit_status_t exit_status) {
  pthread_mutex_lock(&bai_sync_data->mutex);
  if (bai_sync_data->exit_status == EXIT_STATUS_NONE) {
    bai_sync_data->exit_status = exit_status;
  }
  pthread_mutex_unlock(&bai_sync_data->mutex);
}

typedef struct BAISampleArgs {
  BAISyncData *bai_sync_data;
  double delta;
  int sample_limit;
  int sample_minimum;
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
} BAISampleArgs;

static inline double bai_alt_lambda(const double μ1, const double σ21,
                                    const double w1, const double μa,
                                    const double σ2a, const double wa) {
  if (w1 == 0) {
    return μa;
  }
  if (wa == 0 || μ1 == μa) {
    return μ1;
  }
  const double x = wa / w1;
  const double result = (σ2a * μ1 + x * σ21 * μa) / (σ2a + x * σ21);
  return result;
}

static inline double bai_d(const double μ, const double σ2, const double λ) {
  const double diff = μ - λ;
  return 0.5 * (diff * diff) / σ2;
}

static inline double bai_get_arm_z(BAISyncData *bai_sync_data,
                                   const int astar_index,
                                   const int challenger_index) {
  const BAIArmDatum *astar_arm_data = &bai_sync_data->arm_data[astar_index];
  BAIArmDatum *challenger_arm_data = &bai_sync_data->arm_data[challenger_index];
  const double alt_lambda = bai_alt_lambda(
      astar_arm_data->mean, astar_arm_data->var, astar_arm_data->num_samples,
      challenger_arm_data->mean, challenger_arm_data->var,
      challenger_arm_data->num_samples);
  const double d_astar =
      bai_d(astar_arm_data->mean, astar_arm_data->var, alt_lambda);
  const double d_a =
      bai_d(challenger_arm_data->mean, challenger_arm_data->var, alt_lambda);
  return astar_arm_data->num_samples * d_astar +
         challenger_arm_data->num_samples * d_a;
}

static inline int bai_sync_data_sample_limit_reached(BAISyncData *bai_sync_data,
                                                     int sample_limit) {
  return bai_sync_data->num_total_samples_requested >= sample_limit;
}

// Assumes the caller has locked the bai sync data mutex
static inline int
bai_sync_data_get_next_bai_sample_index_while_locked(BAISampleArgs *args) {
  if (bai_sync_data_sample_limit_reached(args->bai_sync_data,
                                         args->sample_limit)) {
    args->bai_sync_data->exit_status = EXIT_STATUS_SAMPLE_LIMIT;
    return -1;
  }
  int arm_index;
  switch (args->sampling_rule) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    arm_index = args->bai_sync_data->num_total_samples_requested %
                args->bai_sync_data->num_arms;
    break;
  case BAI_SAMPLING_RULE_TOP_TWO:
    arm_index = args->bai_sync_data->astar_index;
    if (rvs_sample(args->bai_sync_data->rng, 0, 0, NULL) > 0.5) {
      arm_index = args->bai_sync_data->challenger_index;
    }
    break;
  case BAI_SAMPLING_RULE_TOP_FEW:
    arm_index = args->bai_sync_data->astar_index;
    if (args->bai_sync_data->arm_data[args->bai_sync_data->challenger_index]
            .num_samples <
        args->bai_sync_data->arm_data[args->bai_sync_data->astar_index]
            .num_samples) {
      arm_index = args->bai_sync_data->challenger_index;
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

static inline int bai_sync_data_get_next_sample_index_while_locked(
    BAISampleArgs *args, const bool timeout, const bool user_interrupt) {
  if (args->bai_sync_data->exit_status != EXIT_STATUS_NONE) {
    return -1;
  }
  if (timeout) {
    args->bai_sync_data->exit_status = EXIT_STATUS_TIMEOUT;
    return -1;
  }
  if (user_interrupt) {
    args->bai_sync_data->exit_status = EXIT_STATUS_USER_INTERRUPT;
    return -1;
  }
  if (args->bai_sync_data->initial_phase) {
    return bai_sync_data_get_next_initial_sample_index_while_locked(args);
  }
  return bai_sync_data_get_next_bai_sample_index_while_locked(args);
}

static inline int
bai_sync_data_get_next_sample_index(BAISampleArgs *args, const bool timeout,
                                    const bool user_interrupt) {
  int arm_index;
  pthread_mutex_lock(&args->bai_sync_data->mutex);
  arm_index = bai_sync_data_get_next_sample_index_while_locked(args, timeout,
                                                               user_interrupt);
  pthread_mutex_unlock(&args->bai_sync_data->mutex);
  return arm_index;
}

// Returns true if all arms have reached the threshold
// Assumes the caller has locked bai_sync_data or is the only thread running
static inline void bai_update_threshold_and_challenger(
    BAISyncData *bai_sync_data, bai_threshold_t threshold,
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
  case BAI_SAMPLING_RULE_TOP_TWO:
  case BAI_SAMPLING_RULE_TOP_FEW:
    bai_sync_data->challenger_index = -1;
    break;
  }
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    if (i == bai_sync_data->astar_index ||
        bai_sync_data->arm_data[i].is_epigon) {
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
    case BAI_SAMPLING_RULE_TOP_TWO:
    case BAI_SAMPLING_RULE_TOP_FEW:;
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
    bai_sync_data->exit_status = EXIT_STATUS_THRESHOLD;
  }
}

// Assumes the caller has locked the bai sync data mutex
static inline void
bai_sync_data_add_sample_while_locked(BAISampleArgs *args, const int arm_index,
                                      const double sample_value) {
  BAISyncData *bai_sync_data = args->bai_sync_data;
  BAIArmDatum *arm_data = bai_sync_data->arm_data;
  if (arm_data[arm_index].is_epigon) {
    return;
  }
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

  bai_update_threshold_and_challenger(bai_sync_data, args->threshold,
                                      args->sampling_rule, args->delta,
                                      arm_index, update_all);
}

static inline void bai_sync_data_add_sample(BAISampleArgs *args,
                                            const int arm_index,
                                            const double sample_value) {
  pthread_mutex_lock(&args->bai_sync_data->mutex);
  bai_sync_data_add_sample_while_locked(args, arm_index, sample_value);
  pthread_mutex_unlock(&args->bai_sync_data->mutex);
}

typedef struct BAIWorkerArgs {
  ThreadControl *thread_control;
  BAISyncData *sync_data;
  RandomVariables *rvs;
  const BAIOptions *bai_options;
  BAILogger *bai_logger;
  Checkpoint *checkpoint;
  int thread_index;
} BAIWorkerArgs;

static inline void bai_cull_epigons(void *uncasted_bai_worker_args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)uncasted_bai_worker_args;
  BAISyncData *bai_sync_data = bai_worker_args->sync_data;
  bai_sync_data->initial_phase = false;
  if (bai_sync_data_sample_limit_reached(
          bai_sync_data, bai_worker_args->bai_options->sample_limit)) {
    bai_sync_data->exit_status = EXIT_STATUS_SAMPLE_LIMIT;
    return;
  }
  RandomVariables *rvs = bai_worker_args->rvs;
  int num_epigons = 0;
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    for (int j = i + 1; j < bai_sync_data->num_arms; j++) {
      if (bai_sync_data->arm_data[i].is_epigon ||
          bai_sync_data->arm_data[j].is_epigon) {
        continue;
      }
      int better_arm = i;
      int worse_arm = j;
      if (bai_sync_data->arm_data[j].mean > bai_sync_data->arm_data[i].mean) {
        better_arm = j;
        worse_arm = i;
      }
      if (rvs_mark_as_epigon_if_similar(rvs, better_arm, worse_arm)) {
        bai_sync_data->arm_data[worse_arm].is_epigon = true;
        num_epigons++;
        if (num_epigons == bai_sync_data->num_arms - 1) {
          break;
        }
      }
    }
  }
  if (num_epigons == bai_sync_data->num_arms - 1) {
    bai_sync_data->exit_status = EXIT_STATUS_ONE_ARM_REMAINING;
    return;
  }
  assert(!bai_sync_data->arm_data[bai_sync_data->astar_index].is_epigon);
  bai_update_threshold_and_challenger(
      bai_sync_data, bai_worker_args->bai_options->threshold,
      bai_worker_args->bai_options->sampling_rule,
      bai_worker_args->bai_options->delta, bai_sync_data->astar_index, true);
}

static inline void bai_worker_sample_loop(BAIWorkerArgs *bai_worker_args) {
  BAISyncData *sync_data = bai_worker_args->sync_data;
  ThreadControl *thread_control = bai_worker_args->thread_control;
  const BAIOptions *bai_options = bai_worker_args->bai_options;
  RandomVariables *rvs = bai_worker_args->rvs;
  const int thread_index = bai_worker_args->thread_index;

  BAISampleArgs sample_args = {
      .bai_sync_data = sync_data,
      .delta = bai_options->delta,
      .sample_limit = bai_options->sample_limit,
      .sample_minimum = bai_options->sample_minimum,
      .sampling_rule = bai_options->sampling_rule,
      .threshold = bai_options->threshold,
  };

  while (true) {
    const bool timeout = bai_options->time_limit_seconds > 0 &&
                         thread_control_get_seconds_elapsed(thread_control) >=
                             bai_options->time_limit_seconds;
    const bool user_interrupt = thread_control_get_is_exited(thread_control);
    const int arm_index = bai_sync_data_get_next_sample_index(
        &sample_args, timeout, user_interrupt);
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
  thread_control_reset(thread_control, 0);
  rvs_reset(rvs);
  bai_result_reset(bai_result);

  const int number_of_threads = thread_control_get_threads(thread_control);

  Checkpoint *checkpoint =
      checkpoint_create(number_of_threads, bai_cull_epigons);

  BAISyncData *sync_data = bai_sync_data_create(rvs_get_num_rvs(rvs), rng);

  BAIWorkerArgs bai_worker_args = {
      .thread_control = thread_control,
      .sync_data = sync_data,
      .rvs = rvs,
      .bai_options = bai_options,
      .bai_logger = bai_logger,
      .checkpoint = checkpoint,
  };

  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * number_of_threads);
  BAIWorkerArgs *bai_worker_args_array =
      malloc_or_die((sizeof(BAIWorkerArgs)) * number_of_threads);
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    bai_worker_args_array[thread_index] = bai_worker_args;
    bai_worker_args_array[thread_index].thread_index = thread_index;
    pthread_create(&worker_ids[thread_index], NULL, bai_worker,
                   &bai_worker_args_array[thread_index]);
  }
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
  }
  thread_control_exit(thread_control, sync_data->exit_status);
  bai_result_set_all(bai_result, sync_data->exit_status, sync_data->astar_index,
                     thread_control_get_seconds_elapsed(thread_control));
  free(bai_worker_args_array);
  free(worker_ids);
  bai_sync_data_destroy(sync_data);
  checkpoint_destroy(checkpoint);
}

#endif
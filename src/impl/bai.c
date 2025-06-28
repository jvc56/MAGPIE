/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code kindly provided by Marc Jourdan.
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
#define BAI_ARM_SAMPLE_MINIMUM 50

// Internal BAI structs

// FIXME: just make this whole thing a .h when done

typedef struct BAIArmDatum {
  int rvs_index;
  int num_samples;
  double samples_sum;
  double samples_squared_sum;
  double mean;
  double var;
  int challenger_index;
  double challenger_value;
  bool reached_threshold;
} BAIArmDatum;

typedef struct BAISyncData {
  int num_initial_arms;
  int num_arms;
  int num_arms_reached_threshold;
  int num_total_samples;
  int astar_index;
  double astar_mean;
  double threshold_value;
  exit_status_t exit_status;
  BAIArmDatum *arm_data;
  RandomVariables *rng;
  pthread_mutex_t mutex;
} BAISyncData;

BAISyncData *bai_sync_data_create(const int num_initial_arms) {
  BAISyncData *bai_sync_data = malloc_or_die(sizeof(BAISyncData));
  bai_sync_data->num_initial_arms = num_initial_arms;
  bai_sync_data->num_arms = num_initial_arms;
  bai_sync_data->num_arms_reached_threshold = 0;
  bai_sync_data->num_total_samples = 0;
  bai_sync_data->astar_index = -1;
  bai_sync_data->exit_status = EXIT_STATUS_NONE;
  bai_sync_data->arm_data =
      calloc_or_die(bai_sync_data->num_initial_arms, sizeof(BAIArmDatum));
  pthread_mutex_init(&bai_sync_data->mutex, NULL);
  return bai_sync_data;
}

void bai_sync_data_destroy(BAISyncData *bai_sync_data) {
  free(bai_sync_data->arm_data);
  free(bai_sync_data);
}

double alt_λ(const double μ1, const double σ21, const double w1,
             const double μa, const double σ2a, const double wa,
             BAILogger __attribute__((unused)) * bai_logger) {
  // bai_logger_log_title(bai_logger, "ALT_KV");
  // bai_logger_log_double(bai_logger, "u1", μ1);
  // bai_logger_log_double(bai_logger, "sigma21", σ21);
  // bai_logger_log_double(bai_logger, "w1", w1);
  // bai_logger_log_double(bai_logger, "ua", μa);
  // bai_logger_log_double(bai_logger, "sigma2a", σ2a);
  // bai_logger_log_double(bai_logger, "wa", wa);
  // bai_logger_flush(bai_logger);
  if (w1 == 0) {
    // bai_logger_log_double(bai_logger, "W1_ZERO", μa);
    // bai_logger_flush(bai_logger);
    return μa;
  }
  if (wa == 0 || μ1 == μa) {
    // bai_logger_log_double(bai_logger, "WA_ZERO_OR_U1_EQ_UA", μ1);
    // bai_logger_flush(bai_logger);
    return μ1;
  }
  const double x = wa / w1;
  const double result = (σ2a * μ1 + x * σ21 * μa) / (σ2a + x * σ21);
  // bai_logger_log_double(bai_logger, "x", x);
  // bai_logger_log_double(bai_logger, "result", result);
  // bai_logger_flush(bai_logger);
  return result;
}

double bai_d(const double μ, const double σ2, const double λ) {
  const double diff = μ - λ;
  return 0.5 * (diff * diff) / σ2;
}

double bai_get_tt_k_val_and_update_thres_status(BAISyncData *bai_sync_data,
                                                const int astar_index,
                                                const int challenger_index) {
  const BAIArmDatum *astar_arm_data = &bai_sync_data->arm_data[astar_index];
  BAIArmDatum *challenger_arm_data = &bai_sync_data->arm_data[challenger_index];
  const double alt_lambda =
      alt_λ(astar_arm_data->mean, astar_arm_data->var,
            astar_arm_data->num_samples, challenger_arm_data->mean,
            challenger_arm_data->var, challenger_arm_data->num_samples, NULL);
  const double d_astar =
      bai_d(astar_arm_data->mean, astar_arm_data->var, alt_lambda);
  const double d_a =
      bai_d(challenger_arm_data->mean, challenger_arm_data->var, alt_lambda);
  const double Z = astar_arm_data->num_samples * d_astar +
                   challenger_arm_data->num_samples * d_a;

  const bool old_reached_threshold =
      bai_sync_data->arm_data[challenger_index].reached_threshold;
  const bool new_reached_threshold = Z < bai_sync_data->threshold_value;
  bai_sync_data->arm_data[challenger_index].reached_threshold =
      new_reached_threshold;
  if (!old_reached_threshold && new_reached_threshold) {
    bai_sync_data->num_arms_reached_threshold++;
  } else if (old_reached_threshold && !new_reached_threshold) {
    bai_sync_data->num_arms_reached_threshold--;
  }

  return Z + log((double)challenger_arm_data->num_samples);
}

int bai_sync_data_get_next_sample_index(BAISyncData *bai_sync_data,
                                        BAILogger *bai_logger) {
  int arm_index;
  pthread_mutex_lock(&bai_sync_data->mutex);
  arm_index = bai_sync_data->astar_index;
  if (rvs_sample(bai_sync_data->rng, 0, 0, bai_logger) < 0.5) {
    arm_index =
        bai_sync_data->arm_data[bai_sync_data->astar_index].challenger_index;
  }
  pthread_mutex_unlock(&bai_sync_data->mutex);
  return arm_index;
}

// Returns true if the BAI should exit
bool bai_sync_data_add_sample(BAISyncData *bai_sync_data,
                              const int sample_index, const double sample_value,
                              const double delta,
                              const bai_sampling_rule_t sampling_rule) {
  pthread_mutex_lock(&bai_sync_data->mutex);
  if (bai_sync_data->exit_status != EXIT_STATUS_NONE) {
    pthread_mutex_unlock(&bai_sync_data->mutex);
    return true;
  }
  bai_sync_data->num_total_samples++;
  bai_sync_data->threshold_value =
      log((log((double)bai_sync_data->num_total_samples) + 1) / delta);
  BAIArmDatum *sample_arm_datum = &bai_sync_data->arm_data[sample_index];
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
  const int previous_astar_index = bai_sync_data->astar_index;
  if (bai_sync_data->astar_index < 0 ||
      sample_arm_datum->mean > bai_sync_data->astar_mean) {
    bai_sync_data->astar_index = sample_index;
    bai_sync_data->astar_mean = sample_arm_datum->mean;
  }
  const int current_astar_index = bai_sync_data->astar_index;
  BAIArmDatum *astar_arm_datum = &bai_sync_data->arm_data[current_astar_index];
  if (previous_astar_index != current_astar_index) {
    // Reset the challenger index
    astar_arm_datum->challenger_index = -1;
    for (int i = 0; i < bai_sync_data->num_arms; i++) {
      if (i == current_astar_index) {
        continue;
      }
      const double challenger_value = bai_get_tt_k_val_and_update_thres_status(
          bai_sync_data, current_astar_index, i);
      if (astar_arm_datum->challenger_index < 0 ||
          challenger_value < astar_arm_datum->challenger_value) {
        astar_arm_datum->challenger_index = i;
        astar_arm_datum->challenger_value = challenger_value;
      }
    }
  } else if (sample_index != current_astar_index) {
    const double potential_challenger_value =
        bai_get_tt_k_val_and_update_thres_status(
            bai_sync_data, current_astar_index, sample_index);
    if (potential_challenger_value < astar_arm_datum->challenger_value) {
      astar_arm_datum->challenger_index = sample_index;
      astar_arm_datum->challenger_value = potential_challenger_value;
    }
  }

  bool should_exit = false;
  if (bai_sync_data->num_arms_reached_threshold == bai_sync_data->num_arms &&
      (sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN ||
       bai_sync_data->num_total_samples % bai_sync_data->num_arms == 0)) {
    bai_sync_data->exit_status = EXIT_STATUS_THRESHOLD;
    should_exit = true;
  }
  pthread_mutex_unlock(&bai_sync_data->mutex);
  return should_exit;
}

int bai_arm_datum_compare(const void *a, const void *b) {
  const BAIArmDatum *a_arm_datum = (const BAIArmDatum *)a;
  const BAIArmDatum *b_arm_datum = (const BAIArmDatum *)b;
  if (a_arm_datum->mean < b_arm_datum->mean) {
    return 1;
  } else if (a_arm_datum->mean > b_arm_datum->mean) {
    return -1;
  } else {
    return 0;
  }
}

typedef struct BAI {
  int num_initial_arms;
  BAISyncData *sync_data;
  // These fields are not owned by the BAI struct and are
  // added for convenience.
  ThreadControl *thread_control;
  RandomVariables *rvs;
  BAILogger *bai_logger;
} BAI;

void bai_cull_epigons(void *uncasted_bai) {
  BAI *bai = (BAI *)uncasted_bai;
  BAISyncData *bai_sync_data = bai->sync_data;
  RandomVariables *rvs = bai->rvs;
  qsort(bai_sync_data->arm_data, bai_sync_data->num_initial_arms,
        sizeof(BAIArmDatum), bai_arm_datum_compare);
  for (int i = 0; i < bai_sync_data->num_arms; i++) {
    for (int j = bai_sync_data->num_arms - 1; j > i; j--) {
      rvs_mark_as_epigon_if_similar(rvs, bai_sync_data->arm_data[i].rvs_index,
                                    bai_sync_data->arm_data[j].rvs_index);
      bai_sync_data->num_arms--;
      bai_sync_data->num_total_samples -=
          bai_sync_data->arm_data[j].num_samples;
      bai_sync_data->arm_data[j] =
          bai_sync_data->arm_data[bai_sync_data->num_arms - 1];
    }
  }
  if (bai_sync_data->num_arms == 1) {
    bai_sync_data->exit_status = EXIT_STATUS_ONE_ARM_REMAINING;
  } else {
    bai_sync_data->astar_index = 0;
    bai_sync_data->astar_mean = bai_sync_data->arm_data[0].mean;
  }
}

BAI *bai_create(ThreadControl *thread_control, RandomVariables *rvs,
                BAILogger *bai_logger) {
  BAI *bai = malloc_or_die(sizeof(BAI));
  bai->num_initial_arms = rvs_get_num_rvs(rvs);
  bai->sync_data = bai_sync_data_create(bai->num_initial_arms);
  bai->thread_control = thread_control;
  bai->rvs = rvs;
  bai->bai_logger = bai_logger;
  return bai;
}

void bai_destroy(BAI *bai) {
  bai_sync_data_destroy(bai->sync_data);
  free(bai);
}

typedef struct BAIWorkerArgs {
  BAI *bai;
  const BAIOptions *bai_options;
  RandomVariables *rng;
  ThreadControl *thread_control;
  BAILogger *bai_logger;
  BAIResult *bai_result;
  pthread_mutex_t *thread_index_counter_mutex;
  pthread_mutex_t *initial_sample_counter_mutex;
  int *thread_index_counter;
  int *initial_sample_counter;
  Checkpoint *checkpoint;
} BAIWorkerArgs;

void *bai_worker(void *args) {
  BAIWorkerArgs *bai_worker_args = (BAIWorkerArgs *)args;
  BAI *bai = bai_worker_args->bai;
  BAILogger *bai_logger = bai_worker_args->bai_logger;
  pthread_mutex_t *thread_index_counter_mutex =
      bai_worker_args->thread_index_counter_mutex;
  int *thread_index_counter = bai_worker_args->thread_index_counter;
  pthread_mutex_t *initial_sample_counter_mutex =
      bai_worker_args->initial_sample_counter_mutex;
  int *initial_sample_counter = bai_worker_args->initial_sample_counter;
  const int time_limit_seconds =
      bai_worker_args->bai_options->time_limit_seconds;

  // Assign a thread index
  int thread_index;
  pthread_mutex_lock(thread_index_counter_mutex);
  thread_index = *thread_index_counter;
  (*thread_index_counter)++;
  pthread_mutex_unlock(thread_index_counter_mutex);

  const int number_of_arms = bai->num_initial_arms;
  const int number_of_initial_samples = number_of_arms * BAI_ARM_SAMPLE_MINIMUM;

  int thread_initial_sample_counter = 0;
  while (true) {
    if (thread_control_get_is_exited(bai->thread_control)) {
      return NULL;
    }
    bool finished_initial_samples = false;
    bool should_sample = false;
    pthread_mutex_lock(initial_sample_counter_mutex);
    if (*initial_sample_counter >= number_of_initial_samples) {
      finished_initial_samples = true;
    } else if (thread_initial_sample_counter == *initial_sample_counter) {
      should_sample = true;
      (*initial_sample_counter)++;
    }
    pthread_mutex_unlock(initial_sample_counter_mutex);
    if (finished_initial_samples) {
      break;
    }
    if (should_sample) {
      const int arm_index =
          thread_initial_sample_counter / BAI_ARM_SAMPLE_MINIMUM;
      double sample =
          rvs_sample(bai->rvs, bai->sync_data->arm_data[arm_index].rvs_index,
                     thread_index, bai_logger);
      bai_sync_data_add_sample(bai->sync_data, arm_index, sample,
                               bai_worker_args->bai_options->delta,
                               bai_worker_args->bai_options->sampling_rule);
    }
  }

  checkpoint_wait(bai_worker_args->checkpoint, bai);

  if (bai->sync_data->num_arms == 1) {
    return NULL;
  }

  while ((time_limit_seconds == 0 ||
          thread_control_get_seconds_elapsed(bai->thread_control) <
              time_limit_seconds) &&
         !thread_control_get_is_exited(bai->thread_control)) {
    const int arm_index =
        bai_sync_data_get_next_sample_index(bai->sync_data, bai_logger);
    double sample =
        rvs_sample(bai->rvs, bai->sync_data->arm_data[arm_index].rvs_index,
                   thread_index, bai_logger);
    if (bai_sync_data_add_sample(bai->sync_data, arm_index, sample,
                                 bai_worker_args->bai_options->delta,
                                 bai_worker_args->bai_options->sampling_rule)) {
      break;
    }
  }
  return NULL;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
void bai(const BAIOptions *bai_options, RandomVariables *rvs,
         RandomVariables *rng, ThreadControl *thread_control,
         BAILogger *bai_logger, BAIResult *bai_result) {
  thread_control_reset(thread_control, 0);
  rvs_reset(rvs);
  bai_result_reset(bai_result);

  BAI *bai = bai_create(thread_control, rvs, bai_logger);

  const int number_of_threads = thread_control_get_threads(thread_control);

  pthread_t *worker_ids = NULL;
  int thread_index_counter = 0;
  int initial_sample_counter = 0;
  pthread_mutex_t thread_index_counter_mutex;
  pthread_mutex_init(&thread_index_counter_mutex, NULL);
  pthread_mutex_t initial_sample_counter_mutex;
  pthread_mutex_init(&initial_sample_counter_mutex, NULL);
  Checkpoint *checkpoint =
      checkpoint_create(number_of_threads, bai_cull_epigons);

  BAIWorkerArgs bai_worker_args = {
      .bai = bai,
      .rng = rng,
      .bai_options = bai_options,
      .thread_index_counter_mutex = &thread_index_counter_mutex,
      .thread_index_counter = &thread_index_counter,
      .initial_sample_counter_mutex = &initial_sample_counter_mutex,
      .initial_sample_counter = &initial_sample_counter,
      .checkpoint = checkpoint,
      .bai_logger = bai_logger,
      .bai_result = bai_result,
  };

  worker_ids = malloc_or_die((sizeof(pthread_t)) * number_of_threads);
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_create(&worker_ids[thread_index], NULL, bai_worker,
                   &bai_worker_args);
  }
  for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
  }

  bai_result_set_exit_status(bai_result, bai->sync_data->exit_status);
  bai_result_set_best_arm(
      bai_result,
      bai->sync_data->arm_data[bai->sync_data->astar_index].rvs_index);
  bai_result_set_total_samples(bai_result, bai->sync_data->num_total_samples);

  free(worker_ids);
  checkpoint_destroy(checkpoint);
  bai_destroy(bai);
}
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
#include "../ent/thread_control.h"

#include "../util/io_util.h"

#include "bai_helper.h"
#include "bai_logger.h"
#include "bai_peps.h"
#include "bai_sampling_rule.h"
#include "random_variable.h"

#define MINIMUM_VARIANCE 1e-10
#define BAI_ARM_SAMPLE_MINIMUM 50

// Internal BAI structs

typedef struct BAIArmDatum {
  int bai_index;
  int rvs_index;
  bool is_similarity_evaluated;
  bool is_epigon;
} BAIArmDatum;

typedef struct ProdConQueueMessage {
  const BAIArmDatum *arm_datum;
  double sample;
  bool queue_closed;
} ProdConQueueMessage;

typedef struct ProdConQueue {
  ProdConQueueMessage *queue;
  int newest;
  int oldest;
  int count;
  int size;
  bool closed;
  pthread_mutex_t mutex;
  pthread_cond_t empty;
} ProdConQueue;

ProdConQueue *prod_con_queue_create(int size) {
  ProdConQueue *pcq = malloc_or_die(sizeof(ProdConQueue));
  pcq->newest = 0;
  pcq->oldest = 0;
  pcq->count = 0;
  pcq->size = size;
  pcq->closed = false;
  pcq->queue = malloc_or_die(sizeof(ProdConQueueMessage) * pcq->size);
  pthread_mutex_init(&pcq->mutex, NULL);
  pthread_cond_init(&pcq->empty, NULL);
  return pcq;
}

void prod_con_queue_destroy(ProdConQueue *pcq) {
  if (!pcq) {
    return;
  }
  free(pcq->queue);
  free(pcq);
}

int prod_con_queue_get_count(ProdConQueue *pcq) {
  int count;
  pthread_mutex_lock(&pcq->mutex);
  count = pcq->count;
  pthread_mutex_unlock(&pcq->mutex);
  return count;
}

void prod_con_queue_close(ProdConQueue *pcq) {
  pthread_mutex_lock(&pcq->mutex);
  pcq->closed = true;
  pthread_mutex_unlock(&pcq->mutex);
  pthread_cond_broadcast(&pcq->empty);
}

// Exits fatally if the queue is full. The caller is expected
// to prevent the queue from being full.
void prod_con_queue_produce(ProdConQueue *pcq, ProdConQueueMessage msg) {
  pthread_mutex_lock(&pcq->mutex);
  if (pcq->closed) {
    log_fatal("attempted to produce to a closed queue");
  }
  if (pcq->count == pcq->size) {
    log_fatal("queue is unexpectedly full with %d messages", pcq->count);
  }
  pcq->queue[pcq->newest] = msg;
  pcq->newest = (pcq->newest + 1) % pcq->size;
  pcq->count++;
  pthread_mutex_unlock(&pcq->mutex);
  pthread_cond_signal(&pcq->empty);
}

ProdConQueueMessage prod_con_queue_consume(ProdConQueue *pcq) {
  pthread_mutex_lock(&pcq->mutex);
  while (pcq->count == 0 && !pcq->closed) {
    pthread_cond_wait(&pcq->empty, &pcq->mutex);
  }
  if (pcq->count == 0 && pcq->closed) {
    pthread_mutex_unlock(&pcq->mutex);
    return (ProdConQueueMessage){.queue_closed = true};
  }
  ProdConQueueMessage msg = pcq->queue[pcq->oldest];
  msg.queue_closed = false;
  // Setting these is not strictly necessary, but if there
  // is some accidental double consume, this will make it
  // easier to detect.
  pcq->queue[pcq->oldest].arm_datum = NULL;
  pcq->queue[pcq->oldest].sample = INFINITY;
  pcq->oldest = (pcq->oldest + 1) % pcq->size;
  pcq->count--;
  pthread_mutex_unlock(&pcq->mutex);
  return msg;
}

typedef struct BAI {
  int initial_K;
  int K;
  int total_samples_received;
  int total_samples_requested;
  int epigon_cutoff;
  bool threshold_reached;
  bool is_multithreaded;
  int *N_received;
  int *N_requested;
  double *S;
  double *S2;
  double *hμ;
  double *hσ2;
  BAISamplingRule *bai_sampling_rule;
  BAIArmDatum **arm_data;
  ProdConQueue *request_queue;
  ProdConQueue *response_queue;
  Timer *wait_timer;
  // These fields are not owned by the BAI struct and are
  // added for convenience.
  ThreadControl *thread_control;
  RandomVariables *rvs;
  BAILogger *bai_logger;
} BAI;

BAI *bai_create(ThreadControl *thread_control, RandomVariables *rvs,
                const int epigon_cutoff, BAILogger *bai_logger) {
  BAI *bai = malloc_or_die(sizeof(BAI));
  bai->initial_K = rvs_get_num_rvs(rvs);
  bai->K = bai->initial_K;
  bai->total_samples_received = 0;
  bai->total_samples_requested = 0;
  bai->epigon_cutoff = epigon_cutoff;
  bai->threshold_reached = false;
  bai->N_received = calloc_or_die(bai->initial_K, sizeof(int));
  bai->N_requested = calloc_or_die(bai->initial_K, sizeof(int));
  bai->S = calloc_or_die(bai->initial_K, sizeof(double));
  bai->S2 = calloc_or_die(bai->initial_K, sizeof(double));
  bai->hμ = calloc_or_die(bai->initial_K, sizeof(double));
  bai->hσ2 = calloc_or_die(bai->initial_K, sizeof(double));
  // The bai_sampling_rule field is initialized in the main BAI algorithm
  // after the initial samples are collected and is destroyed normally in the
  // destroy function below.
  bai->arm_data = malloc_or_die((sizeof(BAIArmDatum *)) * (bai->initial_K));
  for (int i = 0; i < bai->initial_K; i++) {
    bai->arm_data[i] = malloc_or_die(sizeof(BAIArmDatum));
    bai->arm_data[i]->bai_index = i;
    bai->arm_data[i]->rvs_index = i;
    bai->arm_data[i]->is_similarity_evaluated = false;
    bai->arm_data[i]->is_epigon = false;
  }
  bai->request_queue = NULL;
  bai->response_queue = NULL;
  bai->is_multithreaded = false;
  const int number_of_threads = thread_control_get_threads(thread_control);
  if (number_of_threads > 1) {
    bai->is_multithreaded = true;
    bai->request_queue = prod_con_queue_create(number_of_threads);
    bai->response_queue = prod_con_queue_create(number_of_threads);
  }
  bai->wait_timer = mtimer_create(CLOCK_MONOTONIC);
  bai->thread_control = thread_control;
  bai->rvs = rvs;
  bai->bai_logger = bai_logger;
  return bai;
}

void bai_destroy(BAI *bai) {
  free(bai->N_received);
  free(bai->N_requested);
  free(bai->S);
  free(bai->S2);
  free(bai->hμ);
  free(bai->hσ2);
  bai_sampling_rule_destroy(bai->bai_sampling_rule);
  for (int i = 0; i < bai->initial_K; i++) {
    free(bai->arm_data[i]);
  }
  free(bai->arm_data);
  prod_con_queue_destroy(bai->request_queue);
  prod_con_queue_destroy(bai->response_queue);
  mtimer_destroy(bai->wait_timer);
  free(bai);
}

// BAI to RandomVariables interface

int bai_get_rvs_index(const BAI *bai, const int k) {
  const int rvs_index = bai->arm_data[k]->rvs_index;
  if (rvs_is_epigon(bai->rvs, rvs_index)) {
    log_fatal("bai selected an arm (%d) that was marked as an epigon (%d)", k,
              rvs_index);
  }
  return rvs_index;
}

bool bai_rvs_mark_as_epigon_if_similar(BAI *bai, const int leader,
                                       const int i) {
  const bool is_epigon = rvs_mark_as_epigon_if_similar(
      bai->rvs, bai_get_rvs_index(bai, leader), bai_get_rvs_index(bai, i));
  bai->arm_data[i]->is_epigon = is_epigon;
  return is_epigon;
}

// BAI functions

typedef struct SampleWorkerArgs {
  ProdConQueue *request_queue;
  ProdConQueue *response_queue;
  RandomVariables *rvs;
  pthread_mutex_t *thread_index_counter_mutex;
  int *thread_index_counter;
  BAILogger *bai_logger;
  BAIResult *bai_result;
} SampleWorkerArgs;

void *bai_sample_worker(void *args) {
  SampleWorkerArgs *bai_worker_args = (SampleWorkerArgs *)args;
  ProdConQueue *request_queue = bai_worker_args->request_queue;
  ProdConQueue *response_queue = bai_worker_args->response_queue;
  RandomVariables *rvs = bai_worker_args->rvs;
  BAILogger *bai_logger = bai_worker_args->bai_logger;
  pthread_mutex_t *thread_index_counter_mutex =
      bai_worker_args->thread_index_counter_mutex;
  int *thread_index_counter = bai_worker_args->thread_index_counter;
  int thread_index;
  pthread_mutex_lock(thread_index_counter_mutex);
  thread_index = *thread_index_counter;
  (*thread_index_counter)++;
  pthread_mutex_unlock(thread_index_counter_mutex);
  Timer *timer = mtimer_create(CLOCK_MONOTONIC);
  ProdConQueueMessage req;
  ProdConQueueMessage resp;
  while (true) {
    mtimer_start(timer);
    req = prod_con_queue_consume(request_queue);
    mtimer_stop(timer);
    bai_result_increment_sample_wait_time(bai_worker_args->bai_result,
                                          mtimer_elapsed_seconds(timer));
    if (req.queue_closed) {
      break;
    }
    mtimer_start(timer);
    resp.sample =
        rvs_sample(rvs, req.arm_datum->rvs_index, thread_index, bai_logger);
    mtimer_stop(timer);
    bai_result_increment_sample_time(bai_worker_args->bai_result,
                                     mtimer_elapsed_seconds(timer));
    resp.arm_datum = req.arm_datum;
    prod_con_queue_produce(response_queue, resp);
  }
  mtimer_destroy(timer);
  return NULL;
}

void bai_update_arm_data_with_sample(BAI *bai, const int k,
                                     const double sample) {
  bai->S[k] += sample;
  bai->S2[k] += sample * sample;
  bai->N_received[k] += 1;
  bai->hμ[k] = bai->S[k] / bai->N_received[k];
  bai->hσ2[k] = bai->S2[k] / bai->N_received[k] - bai->hμ[k] * bai->hμ[k];
  if (bai->hσ2[k] < MINIMUM_VARIANCE) {
    bai->hσ2[k] = MINIMUM_VARIANCE;
  }
  bai->total_samples_received++;
}

void bai_sample_request(BAI *bai, const int k) {
  prod_con_queue_produce(bai->request_queue, (ProdConQueueMessage){
                                                 .arm_datum = bai->arm_data[k],
                                             });
  bai->N_requested[k]++;
  bai->total_samples_requested++;
}

void bai_sample_receive(BAI *bai, BAIResult *bai_result) {
  mtimer_start(bai->wait_timer);
  const ProdConQueueMessage resp = prod_con_queue_consume(bai->response_queue);
  mtimer_stop(bai->wait_timer);
  bai_result_increment_bai_wait_time(bai_result,
                                     mtimer_elapsed_seconds(bai->wait_timer));
  if (resp.arm_datum->is_epigon) {
    return;
  }
  bai_update_arm_data_with_sample(bai, resp.arm_datum->bai_index, resp.sample);
}

void bai_sample_singlethreaded(BAI *bai, const int k, BAILogger *bai_logger) {
  bai_logger_log_int(bai_logger, "sample arm", k);
  const double sample =
      rvs_sample(bai->rvs, bai_get_rvs_index(bai, k), 0, bai->bai_logger);
  bai->N_requested[k]++;
  bai->total_samples_requested++;
  bai_update_arm_data_with_sample(bai, k, sample);
  bai_logger_log_int(bai_logger, "total samples", bai->total_samples_requested);
  for (int i = 0; i < bai->K; i++) {
    bai_logger_log_int(bai_logger, "arm", i);
    bai_logger_log_int(bai_logger, "N", bai->N_requested[i]);
    bai_logger_log_double(bai_logger, "S", bai->S[i]);
    bai_logger_log_double(bai_logger, "S2", bai->S2[i]);
    bai_logger_log_double(bai_logger, "hμ", bai->hμ[i]);
    bai_logger_log_double(bai_logger, "hσ2", bai->hσ2[i]);
  }
  bai_logger_flush(bai_logger);
}

void swap_indexes_bool(bool *a, const int i, const int j) {
  const bool tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void swap_indexes_int(int *a, const int i, const int j) {
  const int tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void swap_indexes_double(double *a, const int i, const int j) {
  const double tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void bai_swap(BAI *bai, const int i, const int j) {
  swap_indexes_int(bai->N_received, i, j);
  swap_indexes_int(bai->N_requested, i, j);
  swap_indexes_double(bai->S, i, j);
  swap_indexes_double(bai->S2, i, j);
  swap_indexes_double(bai->hμ, i, j);
  swap_indexes_double(bai->hσ2, i, j);
  bai_sampling_rule_swap_indexes(bai->bai_sampling_rule, i, j, bai->bai_logger);
  BAIArmDatum *tmp = bai->arm_data[i];
  bai->arm_data[i] = bai->arm_data[j];
  bai->arm_data[j] = tmp;
  bai->arm_data[i]->bai_index = i;
  bai->arm_data[j]->bai_index = j;
}

// Marks arms as epigons if they are similar to astar and
// returns the value of astar.
// If epigons are evaluated, astar will be 0
// Otherwise, astar will remained unchanged.
int bai_potentially_mark_epigons(BAI *bai, const int astar) {
  if (bai->epigon_cutoff == 0 ||
      bai->total_samples_received < bai->epigon_cutoff ||
      bai->arm_data[astar]->is_similarity_evaluated) {
    return astar;
  }
  bai_logger_log_title(bai->bai_logger, "EVAL_EPIGON");
  // Always make astar the first arm.
  bai_swap(bai, astar, 0);
  for (int i = bai->K - 1; i > 0; i--) {
    if (!bai_rvs_mark_as_epigon_if_similar(bai, 0, i)) {
      continue;
    }
    bai_swap(bai, i, bai->K - 1);
    bai->K--;
    if (bai->K == 1) {
      break;
    }
  }
  bai->arm_data[0]->is_similarity_evaluated = true;
  return 0;
}

// BAI helper functions

bool stopping_criterion(const int K, const double *Zs, const BAIThreshold *Sβ,
                        const int *N, const double *hμ, const double *hσ2,
                        const int astar, BAILogger *bai_logger) {
  if (!Sβ) {
    return false;
  }
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    const double thres =
        bai_invoke_threshold(Sβ, N, K, hμ, hσ2, astar, a, bai_logger);
    const bool cdt = Zs[a] > thres;
    bai_logger_log_title(bai_logger, "STOPPING_CRITERION");
    bai_logger_log_int(bai_logger, "a", a + 1);
    bai_logger_log_double(bai_logger, "val", Zs[a]);
    bai_logger_log_double(bai_logger, "thres", thres);
    bai_logger_flush(bai_logger);
    if (!cdt) {
      return false;
    }
  }
  return true;
}

bool bai_sample_limit_reached(const BAIOptions *bai_options, const BAI *bai) {
  if (bai_options->sample_limit == 0) {
    return false;
  }
  if (bai_options->sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN) {
    return bai->total_samples_requested >= bai_options->sample_limit;
  }
  for (int i = 0; i < bai->K; i++) {
    if (bai->N_requested[i] < bai_options->sample_limit) {
      return false;
    }
  }
  return true;
}

bool bai_round_robin_is_complete(const BAI *bai) {
  const int num_arm_requested = bai->N_requested[0];
  for (int i = 1; i < bai->K; i++) {
    if (bai->N_requested[i] != num_arm_requested) {
      return false;
    }
  }
  return true;
}

void bai_set_result(const BAI *bai, const exit_status_t exit_status,
                    const int astar, double total_time, BAIResult *bai_result) {
  bai_result_set_exit_status(bai_result, exit_status);
  bai_result_set_best_arm(bai_result, bai_get_rvs_index(bai, astar));
  bai_result_set_total_samples(bai_result, bai->total_samples_requested);
  bai_result_set_total_time(bai_result, total_time);
}

typedef struct BAIIsFinishedArgs {
  const BAIOptions *bai_options;
  const ThreadControl *thread_control;
  BAI *bai;
  const double *Zs;
  const BAIThreshold *Sβ;
  BAIResult *bai_result;
} BAIIsFinishedArgs;

// Returns true and sets the bai_result if finished.
// Returns false otherwise.
bool bai_is_finished(BAIIsFinishedArgs *args, const exit_status_t exit_status,
                     const int astar) {
  bool finished = false;
  switch (exit_status) {
  case EXIT_STATUS_NONE:
  case EXIT_STATUS_MAX_ITERATIONS:
    log_fatal("invalid BAI finished exit condition: ", exit_status);
    break;
  case EXIT_STATUS_THRESHOLD:
    args->bai->threshold_reached =
        args->bai->threshold_reached ||
        stopping_criterion(args->bai->K, args->Zs, args->Sβ,
                           args->bai->N_received, args->bai->hμ, args->bai->hσ2,
                           astar, NULL);
    finished =
        args->bai->threshold_reached &&
        (args->bai_options->sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN ||
         bai_round_robin_is_complete(args->bai));
    break;
  case EXIT_STATUS_SAMPLE_LIMIT:
    finished = bai_sample_limit_reached(args->bai_options, args->bai);
    break;
  case EXIT_STATUS_TIME_LIMIT:
    finished = args->bai_options->time_limit_seconds > 0 &&
               thread_control_get_seconds_elapsed(args->bai->thread_control) >=
                   args->bai_options->time_limit_seconds;
    break;
  case EXIT_STATUS_USER_INTERRUPT:
    finished = thread_control_get_is_exited(args->bai->thread_control);
    break;
  case EXIT_STATUS_ONE_ARM_REMAINING:
    finished = args->bai->K == 1;
    break;
  }
  if (finished) {
    bai_set_result(args->bai, exit_status, astar,
                   thread_control_get_seconds_elapsed(args->thread_control),
                   args->bai_result);
    thread_control_exit(args->bai->thread_control, exit_status);
  }
  return finished;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
void bai(const BAIOptions *bai_options, RandomVariables *rvs,
         RandomVariables *rng, ThreadControl *thread_control,
         BAILogger *bai_logger, BAIResult *bai_result) {
  thread_control_reset(thread_control, 0);
  rvs_reset(rvs);
  bai_result_reset(bai_result);

  BAI *bai =
      bai_create(thread_control, rvs, bai_options->epigon_cutoff, bai_logger);

  BAIThreshold *Sβ = bai_create_threshold(bai_options->threshold,
                                          bai_options->delta, 2, 2, 1.2);
  BAIGLRTResults *glrt_results = bai_glrt_results_create(bai->initial_K);

  const int number_of_threads = thread_control_get_threads(thread_control);

  pthread_t *worker_ids = NULL;
  int thread_index_counter = 0;
  pthread_mutex_t thread_index_counter_mutex;
  pthread_mutex_init(&thread_index_counter_mutex, NULL);

  SampleWorkerArgs bai_worker_args = {
      .request_queue = bai->request_queue,
      .response_queue = bai->response_queue,
      .rvs = rvs,
      .thread_index_counter_mutex = &thread_index_counter_mutex,
      .thread_index_counter = &thread_index_counter,
      .bai_logger = bai_logger,
      .bai_result = bai_result,
  };

  if (bai->is_multithreaded) {
    worker_ids = malloc_or_die((sizeof(pthread_t)) * number_of_threads);
    for (int thread_index = 0; thread_index < number_of_threads;
         thread_index++) {
      pthread_create(&worker_ids[thread_index], NULL, bai_sample_worker,
                     &bai_worker_args);
    }
    // Give each thread a sample to work on, then when all threads
    // have a sample to compute, start collecting samples and requesting
    // new samples on a 1-for-1 basis so the request queue does not
    // overflow.
    for (int k = 0; k < bai->initial_K; k++) {
      for (int i = 0; i < (BAI_ARM_SAMPLE_MINIMUM); i++) {
        if (bai->total_samples_requested >= number_of_threads) {
          bai_sample_receive(bai, bai_result);
        }
        bai_sample_request(bai, k);
      }
    }
    int num_to_receive = number_of_threads;
    if (num_to_receive > bai->total_samples_requested) {
      num_to_receive = bai->total_samples_requested;
    }
    for (int i = 0; i < num_to_receive; i++) {
      // At this point we have requested some X samples and have only
      // received X - number_of_threads samples, so we need to receive
      // the remaining number_of_threads samples.
      bai_sample_receive(bai, bai_result);
    }
  } else {
    for (int k = 0; k < bai->initial_K; k++) {
      for (int i = 0; i < (BAI_ARM_SAMPLE_MINIMUM); i++) {
        bai_sample_singlethreaded(bai, k, bai_logger);
      }
    }
  }

  assert(bai->total_samples_requested == bai->total_samples_received);

  // The sampling rule must be initialized after the initial sampling.
  bai->bai_sampling_rule = bai_sampling_rule_create(
      bai_options->sampling_rule, bai->N_received, bai->initial_K);

  if (bai->is_multithreaded) {
    // Ensure all threads are saturated by starting number_of_threads requests
    // before the main algorithm starts. Once started, the main algorithm will
    // request and receive samples on a 1-for-1 basis to keep all of the threads
    // saturated. Here, we just request a sample for the current best arm
    // number_of_threads times since it's likely we will need to sample the
    // the best arm number_of_threads times anyway.
    int non_rr_sample_index = -1;
    if (bai_options->sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN) {
      non_rr_sample_index = 0;
      for (int i = 1; i < bai->initial_K; i++) {
        if (bai->hμ[i] > bai->hμ[non_rr_sample_index]) {
          non_rr_sample_index = i;
        }
      }
    }
    for (int i = 0;
         i < number_of_threads && !bai_sample_limit_reached(bai_options, bai);
         i++) {
      int next_sample = non_rr_sample_index;
      if (next_sample == -1) {
        next_sample = i % bai->initial_K;
      }
      bai_sample_request(bai, next_sample);
    }
  }

  BAIIsFinishedArgs is_finished_args = {
      .bai_options = bai_options,
      .thread_control = thread_control,
      .bai = bai,
      .Zs = glrt_results->vals,
      .Sβ = Sβ,
      .bai_result = bai_result,
  };
  int astar = 0;
  bai_logger_log_title(bai_logger, "finished initlal sampling");
  bai_logger_flush(bai_logger);
  while (
      !bai_is_finished(&is_finished_args, EXIT_STATUS_SAMPLE_LIMIT, astar) &&
      !bai_is_finished(&is_finished_args, EXIT_STATUS_TIME_LIMIT, astar) &&
      !bai_is_finished(&is_finished_args, EXIT_STATUS_USER_INTERRUPT, astar) &&
      !bai_is_finished(&is_finished_args, EXIT_STATUS_ONE_ARM_REMAINING,
                       astar)) {
    bai_glrt(bai->K, bai->N_received, bai->hμ, bai->hσ2, glrt_results,
             NULL);
    const double *Zs = glrt_results->vals;
    const int aalt = glrt_results->k;
    astar = glrt_results->astar;
    const double *ξ = bai->hμ;
    const double *ϕ2 = bai->hσ2;

    if (bai_is_finished(&is_finished_args, EXIT_STATUS_THRESHOLD, astar)) {
      break;
    }

    const int *N;
    if (bai_options->sampling_rule == BAI_SAMPLING_RULE_ROUND_ROBIN) {
      N = bai->N_requested;
    } else {
      N = bai->N_received;
    }

    bai_logger_log_int(bai_logger, "astar", astar);
    bai_logger_log_int(bai_logger, "challenger", aalt);
    // bai_logger_log_double_array(bai_logger, "Zs", Zs, bai->K);
    bai_logger_flush(bai_logger);
    
    const int k = bai_sampling_rule_next_sample(bai->bai_sampling_rule, astar,
                                                aalt, ξ, ϕ2, N, bai->S, Zs,
                                                bai->K, rng, NULL);

    if (bai->is_multithreaded) {
      bai_sample_receive(bai, bai_result);
      bai_sample_request(bai, k);
    } else {
      bai_sample_singlethreaded(bai, k, bai_logger);
    }
    astar = bai_potentially_mark_epigons(bai, astar);
  }
  if (bai->is_multithreaded) {
    prod_con_queue_close(bai->request_queue);
    for (int thread_index = 0; thread_index < number_of_threads;
         thread_index++) {
      pthread_join(worker_ids[thread_index], NULL);
    }
    free(worker_ids);
  }
  bai_glrt_results_destroy(glrt_results);
  bai_destroy_threshold(Sβ);
  bai_destroy(bai);
}
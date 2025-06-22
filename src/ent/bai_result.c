#include "bai_result.h"

#include <pthread.h>

#include "stats.h"

#include "../util/io_util.h"

struct BAIResult {
  exit_status_t exit_status;
  int best_arm;
  int total_samples;
  int thread_waits_for_consume;
  int thread_waits_for_produce;
  double total_time;
  Stat *requests_created;
  pthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result) {
  bai_result->exit_status = EXIT_STATUS_NONE;
  bai_result->best_arm = 0;
  bai_result->total_samples = 0;
  bai_result->thread_waits_for_consume = 0;
  bai_result->thread_waits_for_produce = 0;
  bai_result->total_time = 0;
  stat_reset(bai_result->requests_created);
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  pthread_mutex_init(&bai_result->mutex, NULL);
  bai_result->requests_created = stat_create(true);
  bai_result_reset(bai_result);
  return bai_result;
}

void bai_result_destroy(BAIResult *bai_result) {
  stat_destroy(bai_result->requests_created);
  free(bai_result);
}

void bai_result_set_exit_status(BAIResult *bai_result,
                                exit_status_t exit_status) {
  bai_result->exit_status = exit_status;
}

exit_status_t bai_result_get_exit_status(BAIResult *bai_result) {
  return bai_result->exit_status;
}

void bai_result_set_best_arm(BAIResult *bai_result, int best_arm) {
  bai_result->best_arm = best_arm;
}

int bai_result_get_best_arm(BAIResult *bai_result) {
  return bai_result->best_arm;
}

void bai_result_set_total_samples(BAIResult *bai_result, int total_samples) {
  bai_result->total_samples = total_samples;
}

int bai_result_get_total_samples(BAIResult *bai_result) {
  return bai_result->total_samples;
}

void bai_result_increment_thread_waits_for_consume(BAIResult *bai_result) {
  pthread_mutex_lock(&bai_result->mutex);
  bai_result->thread_waits_for_consume++;
  pthread_mutex_unlock(&bai_result->mutex);
}

int bai_result_get_thread_waits_for_consume(BAIResult *bai_result) {
  return bai_result->thread_waits_for_consume;
}

void bai_result_increment_thread_waits_for_produce(BAIResult *bai_result) {
  pthread_mutex_lock(&bai_result->mutex);
  bai_result->thread_waits_for_produce++;
  pthread_mutex_unlock(&bai_result->mutex);
}

int bai_result_get_thread_waits_for_produce(BAIResult *bai_result) {
  return bai_result->thread_waits_for_produce;
}

void bai_result_add_requests_created(BAIResult *bai_result, int num_requests) {
  pthread_mutex_lock(&bai_result->mutex);
  stat_push(bai_result->requests_created, num_requests, 1);
  pthread_mutex_unlock(&bai_result->mutex);
}

Stat *bai_result_get_requests_created(BAIResult *bai_result) {
  return bai_result->requests_created;
}

void bai_result_set_total_time(BAIResult *bai_result, const double total_time) {
  bai_result->total_time = total_time;
}

double bai_result_get_total_time(BAIResult *bai_result) {
  return bai_result->total_time;
}

#include "bai_result.h"

#include <pthread.h>

#include "../util/util.h"

struct BAIResult {
  exit_status_t exit_status;
  int best_arm;
  int total_samples;
  double sample_time;
  double bai_wait_time;
  double sample_wait_time;
  double total_time;
  pthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result) {
  bai_result->exit_status = EXIT_STATUS_NONE;
  bai_result->best_arm = 0;
  bai_result->total_samples = 0;
  bai_result->sample_time = 0;
  bai_result->bai_wait_time = 0;
  bai_result->sample_wait_time = 0;
  bai_result->total_time = 0;
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  pthread_mutex_init(&bai_result->mutex, NULL);
  bai_result_reset(bai_result);
  return bai_result;
}

void bai_result_destroy(BAIResult *bai_result) { free(bai_result); }

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

void bai_result_increment_sample_time(BAIResult *bai_result,
                                      const double sample_time) {
  pthread_mutex_lock(&bai_result->mutex);
  bai_result->sample_time += sample_time;
  pthread_mutex_unlock(&bai_result->mutex);
}

double bai_result_get_sample_time(BAIResult *bai_result) {
  return bai_result->sample_time;
}

void bai_result_increment_sample_wait_time(BAIResult *bai_result,
                                           const double sample_wait_time) {
  pthread_mutex_lock(&bai_result->mutex);
  bai_result->sample_wait_time += sample_wait_time;
  pthread_mutex_unlock(&bai_result->mutex);
}

double bai_result_get_sample_wait_time(BAIResult *bai_result) {
  return bai_result->sample_wait_time;
}

// Note: This is not thread safe since the BAI algorithm is single threaded
void bai_result_increment_bai_wait_time(BAIResult *bai_result,
                                        const double bai_wait_time) {
  bai_result->bai_wait_time += bai_wait_time;
}

double bai_result_get_bai_wait_time(BAIResult *bai_result) {
  return bai_result->bai_wait_time;
}

void bai_result_set_total_time(BAIResult *bai_result, const double total_time) {
  bai_result->total_time = total_time;
}

double bai_result_get_total_time(BAIResult *bai_result) {
  return bai_result->total_time;
}

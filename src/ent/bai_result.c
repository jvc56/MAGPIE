#include "bai_result.h"

#include <pthread.h>

#include "../util/util.h"

struct BAIResult {
  exit_status_t exit_status;
  int best_arm;
  int total_samples;
  double sample_time;
  double bai_time;
  double total_time;
  pthread_mutex_t mutex;
};

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  bai_result->exit_status = EXIT_STATUS_NONE;
  bai_result->best_arm = -1;
  bai_result->total_samples = 0;
  bai_result->sample_time = 0;
  bai_result->bai_time = 0;
  bai_result->total_time = 0;
  pthread_mutex_init(&bai_result->mutex, NULL);
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

void bai_result_set_total_time(BAIResult *bai_result, const double total_time) {
  bai_result->total_time = total_time;
}

double bai_result_get_total_time(BAIResult *bai_result) {
  return bai_result->total_time;
}

void bai_result_set_bai_time(BAIResult *bai_result, const double bai_time) {
  bai_result->bai_time = bai_time;
}

double bai_result_get_bai_time(BAIResult *bai_result) {
  return bai_result->bai_time;
}
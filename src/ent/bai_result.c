#include "bai_result.h"

#include <pthread.h>

#include "../util/io_util.h"

struct BAIResult {
  exit_status_t exit_status;
  int best_arm;
  double total_time;
  pthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result) {
  bai_result->exit_status = EXIT_STATUS_NONE;
  bai_result->best_arm = -1;
  bai_result->total_time = 0;
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  pthread_mutex_init(&bai_result->mutex, NULL);
  bai_result_reset(bai_result);
  return bai_result;
}

void bai_result_destroy(BAIResult *bai_result) { free(bai_result); }

void bai_result_set_all(BAIResult *bai_result, exit_status_t exit_status,
                        int best_arm, double total_time) {
  bai_result->exit_status = exit_status;
  bai_result->best_arm = best_arm;
  bai_result->total_time = total_time;
}

exit_status_t bai_result_get_exit_status(BAIResult *bai_result) {
  return bai_result->exit_status;
}

int bai_result_get_best_arm(BAIResult *bai_result) {
  return bai_result->best_arm;
}

double bai_result_get_total_time(BAIResult *bai_result) {
  return bai_result->total_time;
}

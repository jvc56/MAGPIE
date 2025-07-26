#include "bai_result.h"

#include "../compat/cpthread.h"
#include "../util/io_util.h"
#include <stdlib.h>

struct BAIResult {
  int best_arm;
  double total_time;
  cpthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result) {
  bai_result->best_arm = -1;
  bai_result->total_time = 0;
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  cpthread_mutex_init(&bai_result->mutex);
  bai_result_reset(bai_result);
  return bai_result;
}

void bai_result_destroy(BAIResult *bai_result) { free(bai_result); }

void bai_result_set_all(BAIResult *bai_result, int best_arm,
                        double total_time) {
  bai_result->best_arm = best_arm;
  bai_result->total_time = total_time;
}

int bai_result_get_best_arm(const BAIResult *bai_result) {
  return bai_result->best_arm;
}

double bai_result_get_total_time(const BAIResult *bai_result) {
  return bai_result->total_time;
}

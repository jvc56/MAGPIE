#include "bai_result.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct BAIResult {
  // A bai_result_status_t. Atomic so that BAI's workers can check it after
  // every sample without the mutex, which orders the writes.
  _Atomic int status;
  int best_arm;
  uint64_t num_samples;
  Timer timer;
  double time_limit_seconds;
  cpthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result, double time_limit_seconds) {
  atomic_store(&bai_result->status, BAI_RESULT_STATUS_NONE);
  bai_result->best_arm = -1;
  bai_result->num_samples = 0;
  bai_result->time_limit_seconds = time_limit_seconds;
  ctimer_start(&bai_result->timer);
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  cpthread_mutex_init(&bai_result->mutex);
  bai_result_reset(bai_result, 0);
  return bai_result;
}

// Timer is a plain value type (no owned resources), so a struct copy of
// everything but the mutex is a correct full duplicate.
BAIResult *bai_result_duplicate(const BAIResult *bai_result) {
  if (bai_result == NULL) {
    return NULL;
  }
  BAIResult *new_bai_result = malloc_or_die(sizeof(BAIResult));
  *new_bai_result = *bai_result;
  atomic_init(&new_bai_result->status, atomic_load(&bai_result->status));
  cpthread_mutex_init(&new_bai_result->mutex);
  return new_bai_result;
}

void bai_result_destroy(BAIResult *bai_result) { free(bai_result); }

// Not thread safe, the BAI algorithm handles the threading logic
// for the best arm index, this function is just to record the result.
void bai_result_set_best_arm(BAIResult *bai_result, int best_arm) {
  bai_result->best_arm = best_arm;
}

int bai_result_get_best_arm(const BAIResult *bai_result) {
  return bai_result->best_arm;
}

// Not thread safe, set once bai() has joined its workers.
void bai_result_set_num_samples(BAIResult *bai_result,
                                const uint64_t num_samples) {
  bai_result->num_samples = num_samples;
}

uint64_t bai_result_get_num_samples(const BAIResult *bai_result) {
  return bai_result->num_samples;
}

double bai_result_get_elapsed_seconds(const BAIResult *bai_result) {
  return ctimer_elapsed_seconds(&bai_result->timer);
}

void bai_result_stop_timer(BAIResult *bai_result) {
  ctimer_stop(&bai_result->timer);
}

double bai_result_get_time_limit_seconds(const BAIResult *bai_result) {
  return bai_result->time_limit_seconds;
}

bai_result_status_t bai_result_get_status(BAIResult *bai_result) {
  return (bai_result_status_t)atomic_load(&bai_result->status);
}

// Sets user interrupt or timeout status if the conditions for either are met.
// BAI's workers call this after every sample, so the usual case, a sim still
// running with no interrupt and time to spare, takes no lock.
bai_result_status_t bai_result_set_and_get_status(BAIResult *bai_result,
                                                  const bool user_interrupt) {
  bai_result_status_t status = bai_result_get_status(bai_result);
  if (status != BAI_RESULT_STATUS_NONE) {
    return status;
  }
  const bool timed_out = bai_result->time_limit_seconds > 0 &&
                         bai_result_get_elapsed_seconds(bai_result) >=
                             bai_result->time_limit_seconds;
  if (!user_interrupt && !timed_out) {
    return BAI_RESULT_STATUS_NONE;
  }
  cpthread_mutex_lock(&bai_result->mutex);
  status = bai_result_get_status(bai_result);
  if (status == BAI_RESULT_STATUS_NONE) {
    status = user_interrupt ? BAI_RESULT_STATUS_USER_INTERRUPT
                            : BAI_RESULT_STATUS_TIMEOUT;
    atomic_store(&bai_result->status, status);
  }
  cpthread_mutex_unlock(&bai_result->mutex);
  return status;
}

void bai_result_set_status(BAIResult *bai_result,
                           const bai_result_status_t status) {
  cpthread_mutex_lock(&bai_result->mutex);
  atomic_store(&bai_result->status, status);
  cpthread_mutex_unlock(&bai_result->mutex);
}

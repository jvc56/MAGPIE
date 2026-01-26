#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BAI_RESULT_STATUS_NONE,
  BAI_RESULT_STATUS_THRESHOLD,
  BAI_RESULT_STATUS_WIN_PCT_CUTOFF,
  BAI_RESULT_STATUS_SAMPLE_LIMIT,
  BAI_RESULT_STATUS_TIMEOUT,
  BAI_RESULT_STATUS_USER_INTERRUPT,
} bai_result_status_t;

typedef struct BAIResult BAIResult;

void bai_result_reset(BAIResult *bai_result, uint64_t time_limit_seconds);
BAIResult *bai_result_create(void);
void bai_result_destroy(BAIResult *bai_result);
void bai_result_set_best_arm(BAIResult *bai_result, int best_arm);
int bai_result_get_best_arm(const BAIResult *bai_result);
bai_result_status_t bai_result_get_status(BAIResult *bai_result);
void bai_result_set_status(BAIResult *bai_result,
                           const bai_result_status_t status);
bai_result_status_t bai_result_set_and_get_status(BAIResult *bai_result,
                                                  bool user_interrupt);
double bai_result_get_elapsed_seconds(const BAIResult *bai_result);
void bai_result_stop_timer(BAIResult *bai_result);
uint64_t bai_result_get_time_limit_seconds(const BAIResult *bai_result);

#endif

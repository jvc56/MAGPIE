#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include <stdint.h>

typedef enum {
  BAI_RESULT_STATUS_NONE,
  BAI_RESULT_STATUS_MAX_ITERATIONS,
  BAI_RESULT_STATUS_THRESHOLD,
  BAI_RESULT_STATUS_SAMPLE_LIMIT,
  BAI_RESULT_STATUS_TIMEOUT,
  BAI_RESULT_STATUS_ONE_ARM_REMAINING,
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
double bai_result_get_elapsed_seconds(const BAIResult *bai_result);

#endif

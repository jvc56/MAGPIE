#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include "../def/thread_control_defs.h"

typedef struct BAIResult BAIResult;

BAIResult *bai_result_create(void);
void bai_result_destroy(BAIResult *bai_result);
void bai_result_set_exit_status(BAIResult *bai_result,
                                exit_status_t exit_status);
exit_status_t bai_result_get_exit_status(BAIResult *bai_result);
void bai_result_set_best_arm(BAIResult *bai_result, int best_arm);
int bai_result_get_best_arm(BAIResult *bai_result);
void bai_result_set_total_samples(BAIResult *bai_result, int total_samples);
int bai_result_get_total_samples(BAIResult *bai_result);
void bai_result_increment_sample_time(BAIResult *bai_result,
                                      const double sample_time);
double bai_result_get_sample_time(BAIResult *bai_result);
void bai_result_set_total_time(BAIResult *bai_result, const double total_time);
double bai_result_get_total_time(BAIResult *bai_result);
#endif

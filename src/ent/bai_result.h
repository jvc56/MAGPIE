#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include "../def/thread_control_defs.h"

typedef struct BAIResult BAIResult;

void bai_result_reset(BAIResult *bai_result);
BAIResult *bai_result_create(void);
void bai_result_destroy(BAIResult *bai_result);
void bai_result_set_all(BAIResult *bai_result, exit_status_t exit_status,
                        int best_arm, double total_time);
exit_status_t bai_result_get_exit_status(BAIResult *bai_result);
int bai_result_get_best_arm(BAIResult *bai_result);
double bai_result_get_total_time(BAIResult *bai_result);

#endif

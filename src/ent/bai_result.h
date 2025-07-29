#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include "../def/thread_control_defs.h"

typedef struct BAIResult BAIResult;

void bai_result_reset(BAIResult *bai_result);
BAIResult *bai_result_create(void);
void bai_result_destroy(BAIResult *bai_result);
void bai_result_set_all(BAIResult *bai_result, int best_arm, double total_time);
int bai_result_get_best_arm(const BAIResult *bai_result);
double bai_result_get_total_time(const BAIResult *bai_result);

#endif

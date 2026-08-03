#ifndef BAI_RESULT_H
#define BAI_RESULT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BAI_RESULT_STATUS_NONE,
  BAI_RESULT_STATUS_THRESHOLD,
  BAI_RESULT_STATUS_WIN_PCT_CUTOFF,
  BAI_RESULT_STATUS_SAMPLE_LIMIT,
  BAI_RESULT_STATUS_REGRET_LIMIT,
  BAI_RESULT_STATUS_RULE_ZERO_LIMIT,
  BAI_RESULT_STATUS_TIMEOUT,
  BAI_RESULT_STATUS_USER_INTERRUPT,
} bai_result_status_t;

typedef struct BAIResult BAIResult;

void bai_result_reset(BAIResult *bai_result, double time_limit_seconds);
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
double bai_result_get_time_limit_seconds(const BAIResult *bai_result);
double bai_result_get_estimated_regret(BAIResult *bai_result);
void bai_result_set_estimated_regret(BAIResult *bai_result,
                                     double estimated_regret);
double bai_result_get_joint_estimated_regret(BAIResult *bai_result);
void bai_result_set_joint_estimated_regret(BAIResult *bai_result,
                                           double estimated_regret);
double bai_result_get_regret_at_stop(BAIResult *bai_result);
void bai_result_set_regret_at_stop(BAIResult *bai_result,
                                   double regret_at_stop);
double bai_result_get_joint_regret_at_stop(BAIResult *bai_result);
void bai_result_set_joint_regret_at_stop(BAIResult *bai_result,
                                         double regret_at_stop);
int bai_result_get_near_tie_challengers(BAIResult *bai_result);
void bai_result_set_near_tie_challengers(BAIResult *bai_result,
                                         int near_tie_challengers);
int bai_result_get_near_tie_challengers_at_stop(BAIResult *bai_result);
void bai_result_set_near_tie_challengers_at_stop(BAIResult *bai_result,
                                                 int near_tie_challengers);
bool bai_result_get_rule_zero_stopped(BAIResult *bai_result);
uint64_t bai_result_get_rule_zero_stop_nodes(BAIResult *bai_result);
uint64_t bai_result_get_rule_zero_stop_iterations(BAIResult *bai_result);
int bai_result_get_rule_zero_stable_checkpoints(BAIResult *bai_result);
int bai_result_get_rule_zero_selected_switches(BAIResult *bai_result);
void bai_result_set_rule_zero_stop(BAIResult *bai_result, uint64_t nodes,
                                   uint64_t iterations,
                                   int stable_checkpoints,
                                   int selected_switches);

#endif

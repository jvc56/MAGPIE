#include "bai_result.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

struct BAIResult {
  bai_result_status_t status;
  int best_arm;
  Timer timer;
  double time_limit_seconds;
  double estimated_regret;
  double joint_estimated_regret;
  double regret_at_stop;
  double joint_regret_at_stop;
  int near_tie_challengers;
  int near_tie_challengers_at_stop;
  bool rule_zero_stopped;
  bool rule_zero_would_stop;
  uint64_t rule_zero_stop_nodes;
  uint64_t rule_zero_stop_iterations;
  int rule_zero_stable_checkpoints;
  int rule_zero_selected_switches;
  cpthread_mutex_t mutex;
};

void bai_result_reset(BAIResult *bai_result, double time_limit_seconds) {
  bai_result->status = BAI_RESULT_STATUS_NONE;
  bai_result->best_arm = -1;
  bai_result->time_limit_seconds = time_limit_seconds;
  bai_result->estimated_regret = NAN;
  bai_result->joint_estimated_regret = NAN;
  bai_result->regret_at_stop = NAN;
  bai_result->joint_regret_at_stop = NAN;
  bai_result->near_tie_challengers = -1;
  bai_result->near_tie_challengers_at_stop = -1;
  bai_result->rule_zero_stopped = false;
  bai_result->rule_zero_would_stop = false;
  bai_result->rule_zero_stop_nodes = 0;
  bai_result->rule_zero_stop_iterations = 0;
  bai_result->rule_zero_stable_checkpoints = 0;
  bai_result->rule_zero_selected_switches = 0;
  ctimer_start(&bai_result->timer);
}

BAIResult *bai_result_create(void) {
  BAIResult *bai_result = malloc_or_die(sizeof(BAIResult));
  cpthread_mutex_init(&bai_result->mutex);
  bai_result_reset(bai_result, 0);
  return bai_result;
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

double bai_result_get_elapsed_seconds(const BAIResult *bai_result) {
  return ctimer_elapsed_seconds(&bai_result->timer);
}

void bai_result_stop_timer(BAIResult *bai_result) {
  ctimer_stop(&bai_result->timer);
}

double bai_result_get_time_limit_seconds(const BAIResult *bai_result) {
  return bai_result->time_limit_seconds;
}

double bai_result_get_estimated_regret(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const double estimated_regret = bai_result->estimated_regret;
  cpthread_mutex_unlock(&bai_result->mutex);
  return estimated_regret;
}

void bai_result_set_estimated_regret(BAIResult *bai_result,
                                     double estimated_regret) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->estimated_regret = estimated_regret;
  cpthread_mutex_unlock(&bai_result->mutex);
}

double bai_result_get_joint_estimated_regret(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const double estimated_regret = bai_result->joint_estimated_regret;
  cpthread_mutex_unlock(&bai_result->mutex);
  return estimated_regret;
}

void bai_result_set_joint_estimated_regret(BAIResult *bai_result,
                                           double estimated_regret) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->joint_estimated_regret = estimated_regret;
  cpthread_mutex_unlock(&bai_result->mutex);
}

double bai_result_get_regret_at_stop(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const double regret_at_stop = bai_result->regret_at_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
  return regret_at_stop;
}

void bai_result_set_regret_at_stop(BAIResult *bai_result,
                                   double regret_at_stop) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->regret_at_stop = regret_at_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
}

double bai_result_get_joint_regret_at_stop(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const double regret_at_stop = bai_result->joint_regret_at_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
  return regret_at_stop;
}

void bai_result_set_joint_regret_at_stop(BAIResult *bai_result,
                                         double regret_at_stop) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->joint_regret_at_stop = regret_at_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
}

int bai_result_get_near_tie_challengers(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const int near_tie_challengers = bai_result->near_tie_challengers;
  cpthread_mutex_unlock(&bai_result->mutex);
  return near_tie_challengers;
}

void bai_result_set_near_tie_challengers(BAIResult *bai_result,
                                         int near_tie_challengers) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->near_tie_challengers = near_tie_challengers;
  cpthread_mutex_unlock(&bai_result->mutex);
}

int bai_result_get_near_tie_challengers_at_stop(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const int near_tie_challengers = bai_result->near_tie_challengers_at_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
  return near_tie_challengers;
}

void bai_result_set_near_tie_challengers_at_stop(BAIResult *bai_result,
                                                 int near_tie_challengers) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->near_tie_challengers_at_stop = near_tie_challengers;
  cpthread_mutex_unlock(&bai_result->mutex);
}

bool bai_result_get_rule_zero_stopped(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const bool stopped = bai_result->rule_zero_stopped;
  cpthread_mutex_unlock(&bai_result->mutex);
  return stopped;
}

uint64_t bai_result_get_rule_zero_stop_nodes(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const uint64_t nodes = bai_result->rule_zero_stop_nodes;
  cpthread_mutex_unlock(&bai_result->mutex);
  return nodes;
}

uint64_t bai_result_get_rule_zero_stop_iterations(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const uint64_t iterations = bai_result->rule_zero_stop_iterations;
  cpthread_mutex_unlock(&bai_result->mutex);
  return iterations;
}

int bai_result_get_rule_zero_stable_checkpoints(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const int stable_checkpoints = bai_result->rule_zero_stable_checkpoints;
  cpthread_mutex_unlock(&bai_result->mutex);
  return stable_checkpoints;
}

int bai_result_get_rule_zero_selected_switches(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const int selected_switches = bai_result->rule_zero_selected_switches;
  cpthread_mutex_unlock(&bai_result->mutex);
  return selected_switches;
}

bool bai_result_get_rule_zero_would_stop(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  const bool would_stop = bai_result->rule_zero_would_stop;
  cpthread_mutex_unlock(&bai_result->mutex);
  return would_stop;
}

void bai_result_set_rule_zero_trigger(BAIResult *bai_result, uint64_t nodes,
                                      uint64_t iterations,
                                      int stable_checkpoints,
                                      int selected_switches, bool stopped) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->rule_zero_would_stop = true;
  bai_result->rule_zero_stopped = stopped;
  bai_result->rule_zero_stop_nodes = nodes;
  bai_result->rule_zero_stop_iterations = iterations;
  bai_result->rule_zero_stable_checkpoints = stable_checkpoints;
  bai_result->rule_zero_selected_switches = selected_switches;
  cpthread_mutex_unlock(&bai_result->mutex);
}

bool bai_result_set_status_if_none(BAIResult *bai_result,
                                   const bai_result_status_t status) {
  cpthread_mutex_lock(&bai_result->mutex);
  const bool claimed = bai_result->status == BAI_RESULT_STATUS_NONE;
  if (claimed) {
    bai_result->status = status;
  }
  cpthread_mutex_unlock(&bai_result->mutex);
  return claimed;
}

// Sets user interrupt or timeout status if the conditions for either are met
bai_result_status_t bai_result_set_and_get_status(BAIResult *bai_result,
                                                  const bool user_interrupt) {
  cpthread_mutex_lock(&bai_result->mutex);
  if (bai_result->status == BAI_RESULT_STATUS_NONE) {
    if (user_interrupt) {
      bai_result->status = BAI_RESULT_STATUS_USER_INTERRUPT;
    } else if (bai_result->time_limit_seconds > 0 &&
               bai_result_get_elapsed_seconds(bai_result) >=
                   bai_result->time_limit_seconds) {
      bai_result->status = BAI_RESULT_STATUS_TIMEOUT;
    }
  }
  bai_result_status_t status = bai_result->status;
  cpthread_mutex_unlock(&bai_result->mutex);
  return status;
}

bai_result_status_t bai_result_get_status(BAIResult *bai_result) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result_status_t status = bai_result->status;
  cpthread_mutex_unlock(&bai_result->mutex);
  return status;
}

void bai_result_set_status(BAIResult *bai_result,
                           const bai_result_status_t status) {
  cpthread_mutex_lock(&bai_result->mutex);
  bai_result->status = status;
  cpthread_mutex_unlock(&bai_result->mutex);
}

#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/thread_control_defs.h"

#include "timer.h"
#include "xoshiro.h"

typedef struct ThreadControl ThreadControl;

typedef struct ThreadControlIterOutput {
  uint64_t seed;
  uint64_t iter_count;
} ThreadControlIterOutput;

typedef struct ThreadControlIterCompletedOutput {
  uint64_t iter_count_completed;
  double time_elapsed;
  bool print_info;
} ThreadControlIterCompletedOutput;

ThreadControl *thread_control_create(void);
void thread_control_destroy(ThreadControl *thread_control);

bool thread_control_get_is_exited(ThreadControl *thread_control);
int thread_control_get_print_info_interval(const ThreadControl *thread_control);
int thread_control_get_threads(const ThreadControl *thread_control);

bool thread_control_exit(ThreadControl *thread_control,
                         exit_status_t exit_status);
exit_status_t thread_control_get_exit_status(ThreadControl *thread_control);
void thread_control_set_print_info_interval(ThreadControl *thread_control,
                                            int print_info_interval);
bool thread_control_set_mode_started(ThreadControl *thread_control);
bool thread_control_set_mode_status_ready(ThreadControl *thread_control);
bool thread_control_set_mode_finished(ThreadControl *thread_control);
mode_search_status_t thread_control_get_mode(ThreadControl *thread_control);
void thread_control_set_threads(ThreadControl *thread_control,
                                int number_of_threads);
void thread_control_print(ThreadControl *thread_control, const char *content);
void thread_control_wait_for_mode_stopped(ThreadControl *thread_control);
bool thread_control_get_next_iter_output(ThreadControl *thread_control,
                                         ThreadControlIterOutput *iter_output);
void thread_control_set_seed(ThreadControl *thread_control, uint64_t seed);
void thread_control_increment_seed(ThreadControl *thread_control);
uint64_t thread_control_get_seed(const ThreadControl *thread_control);
uint64_t thread_control_get_iter_count(const ThreadControl *thread_control);
void thread_control_reset(ThreadControl *thread_control,
                          uint64_t max_iter_count);
void thread_control_complete_iter(
    ThreadControl *thread_control,
    ThreadControlIterCompletedOutput *iter_completed_output);
double thread_control_get_seconds_elapsed(const ThreadControl *thread_control);
void thread_control_copy_to_dst_and_jump(ThreadControl *thread_control,
                                         XoshiroPRNG *dst);
#endif
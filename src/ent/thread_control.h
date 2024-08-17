#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/thread_control_defs.h"

#include "file_handler.h"
#include "timer.h"

typedef struct ThreadControl ThreadControl;

typedef struct ThreadControlIterOutput {
  uint64_t seed;
  uint64_t iter_count;
  bool print_info;
} ThreadControlIterOutput;

ThreadControl *thread_control_create(void);
void thread_control_destroy(ThreadControl *thread_control);

bool thread_control_get_is_halted(ThreadControl *thread_control);
FileHandler *thread_control_get_infile(ThreadControl *thread_control);
Timer *thread_control_get_timer(ThreadControl *thread_control);
int thread_control_get_print_info_interval(const ThreadControl *thread_control);
int thread_control_get_check_stop_interval(const ThreadControl *thread_control);
int thread_control_get_threads(const ThreadControl *thread_control);

bool thread_control_halt(ThreadControl *thread_control,
                         halt_status_t halt_status);
bool thread_control_unhalt(ThreadControl *thread_control);
halt_status_t thread_control_get_halt_status(ThreadControl *thread_control);
void thread_control_set_print_info_interval(ThreadControl *thread_control,
                                            int print_info_interval);
void thread_control_set_check_stop_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
bool thread_control_set_mode_searching(ThreadControl *thread_control);
bool thread_control_set_mode_stopped(ThreadControl *thread_control);
mode_search_status_t thread_control_get_mode(ThreadControl *thread_control);
void thread_control_set_io(ThreadControl *thread_control,
                           const char *in_filename, const char *out_filename);
bool thread_control_set_check_stop_active(ThreadControl *thread_control);
bool thread_control_set_check_stop_inactive(ThreadControl *thread_control);
void thread_control_set_threads(ThreadControl *thread_control,
                                int number_of_threads);
void thread_control_print(ThreadControl *thread_control, const char *content);
void thread_control_wait_for_mode_stopped(ThreadControl *thread_control);
bool thread_control_get_next_iter_output(ThreadControl *thread_control,
                                         ThreadControlIterOutput *iter_output);
void thread_control_prng_seed(ThreadControl *thread_control, uint64_t seed);
uint64_t thread_control_get_iter_count(const ThreadControl *thread_control);
void thread_control_reset_iter_count(ThreadControl *thread_control);
uint64_t thread_control_get_max_iter_count(const ThreadControl *thread_control);
void thread_control_set_max_iter_count(ThreadControl *thread_control,
                                       uint64_t max_iter_count);
void thread_control_increment_max_iter_count(ThreadControl *thread_control,
                                             uint64_t inc);

#endif
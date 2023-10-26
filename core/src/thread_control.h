#ifndef THREAD_CONTROL_H
#define THREAD_CONTROL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "config.h"

typedef enum {
  CHECK_STOP_INACTIVE,
  CHECK_STOP_ACTIVE,
} check_stop_status_t;

typedef enum {
  MODE_STOPPED,
  MODE_SEARCHING,
} mode_search_status_t;

typedef enum {
  HALT_STATUS_NONE,
  HALT_STATUS_PROBABILISTIC,
  HALT_STATUS_MAX_ITERATIONS,
  HALT_STATUS_USER_INTERRUPT,
} halt_status_t;

typedef struct ThreadControl {
  int number_of_threads;
  int print_info_interval;
  int check_stopping_condition_interval;
  check_stop_status_t check_stop_status;
  mode_search_status_t current_mode;
  halt_status_t halt_status;
  pthread_mutex_t print_output_mutex;
  pthread_mutex_t check_stopping_condition_mutex;
  pthread_mutex_t current_mode_mutex;
  pthread_mutex_t halt_status_mutex;
  pthread_mutex_t searching_mode_mutex;
  FILE *outfile;
  struct timespec start_time;
} ThreadControl;

ThreadControl *create_thread_control(FILE *outfile);
void destroy_thread_control(ThreadControl *thread_control);
bool halt(ThreadControl *thread_control, halt_status_t halt_status);
bool unhalt(ThreadControl *thread_control);
bool is_halted(ThreadControl *thread_control);
halt_status_t get_halt_status(ThreadControl *thread_control);
void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval);
void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
bool set_mode_searching(ThreadControl *thread_control);
bool set_mode_stopped(ThreadControl *thread_control);
mode_search_status_t get_mode(ThreadControl *thread_control);
bool set_check_stop_active(ThreadControl *thread_control);
bool set_check_stop_inactive(ThreadControl *thread_control);
void print_to_file(ThreadControl *thread_control, const char *content);
void wait_for_mode_stopped(ThreadControl *thread_control);

#endif
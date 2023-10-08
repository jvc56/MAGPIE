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
  pthread_mutex_t halt_status_mutex;
  int halt_status;
  pthread_mutex_t current_mode_mutex;
  int current_mode;
  pthread_mutex_t print_output_mutex;
  int print_info_interval;
  pthread_mutex_t check_stopping_condition_mutex;
  int check_stopping_condition_interval;
  int check_stop_status;
  FILE *outfile;
  pthread_mutex_t searching_mode_mutex;
  struct timespec start_time;
} ThreadControl;

ThreadControl *create_thread_control(FILE *outfile);
void destroy_thread_control(ThreadControl *thread_control);
int halt(ThreadControl *thread_control, int halt_status);
int unhalt(ThreadControl *thread_control);
int is_halted(ThreadControl *thread_control);
int get_halt_status(ThreadControl *thread_control);
void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval);
void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval);
int set_mode_searching(ThreadControl *thread_control);
int set_mode_stopped(ThreadControl *thread_control);
int get_mode(ThreadControl *thread_control);
int set_check_stop_active(ThreadControl *thread_control);
int set_check_stop_inactive(ThreadControl *thread_control);
void print_to_file(ThreadControl *thread_control, const char *content);
void wait_for_mode_stopped(ThreadControl *thread_control);

#endif
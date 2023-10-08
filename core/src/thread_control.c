#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "thread_control.h"
#include "util.h"

ThreadControl *create_thread_control(FILE *outfile) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->number_of_threads = 1;
  pthread_mutex_init(&thread_control->halt_status_mutex, NULL);
  thread_control->halt_status = HALT_STATUS_NONE;
  pthread_mutex_init(&thread_control->current_mode_mutex, NULL);
  thread_control->current_mode = MODE_STOPPED;
  pthread_mutex_init(&thread_control->print_output_mutex, NULL);
  thread_control->print_info_interval = 0;
  pthread_mutex_init(&thread_control->check_stopping_condition_mutex, NULL);
  thread_control->check_stopping_condition_interval = 0;
  thread_control->check_stop_status = CHECK_STOP_INACTIVE;
  if (!outfile) {
    thread_control->outfile = stdout;
  } else {
    thread_control->outfile = outfile;
  }
  pthread_mutex_init(&thread_control->searching_mode_mutex, NULL);
  return thread_control;
}

void destroy_thread_control(ThreadControl *thread_control) {
  free(thread_control);
}

void set_print_info_interval(ThreadControl *thread_control,
                             int print_info_interval) {
  thread_control->print_info_interval = print_info_interval;
}

void set_check_stopping_condition_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval) {
  thread_control->check_stopping_condition_interval =
      check_stopping_condition_interval;
}

int get_halt_status(ThreadControl *thread_control) {
  int halt_status;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  halt_status = thread_control->halt_status;
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return halt_status;
}

int is_halted(ThreadControl *thread_control) {
  return get_halt_status(thread_control) != HALT_STATUS_NONE;
}

int halt(ThreadControl *thread_control, int halt_status) {
  int success = 0;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  // Assume the first reason to halt is the only
  // reason we care about, so subsequent calls to halt
  // can be ignored.
  if (thread_control->halt_status == HALT_STATUS_NONE &&
      halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = halt_status;
    success = 1;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

int unhalt(ThreadControl *thread_control) {
  int success = 0;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  if (thread_control->halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = HALT_STATUS_NONE;
    success = 1;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

int set_mode_searching(ThreadControl *thread_control) {
  int success = 0;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  if (thread_control->current_mode == MODE_STOPPED) {
    thread_control->current_mode = MODE_SEARCHING;
    success = 1;
  }
  // Searching mode mutex should remain locked while we are searching.
  pthread_mutex_lock(&thread_control->searching_mode_mutex);
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return success;
}

int set_mode_stopped(ThreadControl *thread_control) {
  int success = 0;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  if (thread_control->current_mode == MODE_SEARCHING) {
    thread_control->current_mode = MODE_STOPPED;
    success = 1;
  }
  pthread_mutex_unlock(&thread_control->searching_mode_mutex);
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return success;
}

int get_mode(ThreadControl *thread_control) {
  int mode = 0;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  mode = thread_control->current_mode;
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return mode;
}

int set_check_stop_active(ThreadControl *thread_control) {
  int success = 0;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_INACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_ACTIVE;
    success = 1;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

int set_check_stop_inactive(ThreadControl *thread_control) {
  int success = 0;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_ACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_INACTIVE;
    success = 1;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

void print_to_file(ThreadControl *thread_control, const char *content) {
  // Lock to print unconditionally even if we might not need
  // to for simplicity. The performance cost is negligible.
  pthread_mutex_lock(&thread_control->print_output_mutex);
  fprintf(thread_control->outfile, "%s", content);
  fflush(thread_control->outfile);
  pthread_mutex_unlock(&thread_control->print_output_mutex);
}

void wait_for_mode_stopped(ThreadControl *thread_control) {
  pthread_mutex_lock(&thread_control->searching_mode_mutex);
  // We can only acquire the lock once the search has stopped.
  pthread_mutex_unlock(&thread_control->searching_mode_mutex);
}
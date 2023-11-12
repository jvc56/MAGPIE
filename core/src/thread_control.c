#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "thread_control.h"
#include "util.h"

ThreadControl *create_thread_control() {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->halt_status = HALT_STATUS_NONE;
  thread_control->current_mode = MODE_STOPPED;
  thread_control->check_stop_status = CHECK_STOP_INACTIVE;
  thread_control->number_of_threads = 1;
  thread_control->check_stopping_condition_interval = 0;
  thread_control->print_info_interval = 0;
  pthread_mutex_init(&thread_control->current_mode_mutex, NULL);
  pthread_mutex_init(&thread_control->check_stopping_condition_mutex, NULL);
  pthread_mutex_init(&thread_control->halt_status_mutex, NULL);
  pthread_mutex_init(&thread_control->searching_mode_mutex, NULL);
  thread_control->outfile = create_file_handler_from_filename(
      STDOUT_FILENAME, FILE_HANDLER_MODE_WRITE);
  thread_control->infile =
      create_file_handler_from_filename(STDIN_FILENAME, FILE_HANDLER_MODE_READ);
  return thread_control;
}

void destroy_thread_control(ThreadControl *thread_control) {
  destroy_file_handler(thread_control->outfile);
  destroy_file_handler(thread_control->infile);
  free(thread_control);
}

void set_io(ThreadControl *thread_control, const char *in_filename,
            const char *out_filename) {
  const char *nonnull_in_filename = in_filename;
  if (!nonnull_in_filename) {
    nonnull_in_filename = get_file_handler_filename(thread_control->infile);
  }

  const char *nonnull_out_filename = out_filename;
  if (!nonnull_out_filename) {
    nonnull_out_filename = get_file_handler_filename(thread_control->outfile);
  }

  if (strings_equal(nonnull_in_filename, nonnull_out_filename)) {
    log_warn("in file and out file cannot be the same\n");
    return;
  }

  set_file_handler(thread_control->infile, nonnull_in_filename,
                   FILE_HANDLER_MODE_READ);

  set_file_handler(thread_control->outfile, nonnull_out_filename,
                   FILE_HANDLER_MODE_WRITE);
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

halt_status_t get_halt_status(ThreadControl *thread_control) {
  halt_status_t halt_status;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  halt_status = thread_control->halt_status;
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return halt_status;
}

bool is_halted(ThreadControl *thread_control) {
  return get_halt_status(thread_control) != HALT_STATUS_NONE;
}

bool halt(ThreadControl *thread_control, halt_status_t halt_status) {
  bool success = false;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  // Assume the first reason to halt is the only
  // reason we care about, so subsequent calls to halt
  // can be ignored.
  if (thread_control->halt_status == HALT_STATUS_NONE &&
      halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = halt_status;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

bool unhalt(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  if (thread_control->halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = HALT_STATUS_NONE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

bool set_mode_searching(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  if (thread_control->current_mode == MODE_STOPPED) {
    thread_control->current_mode = MODE_SEARCHING;
    // Searching mode mutex should remain locked while we are searching.
    pthread_mutex_lock(&thread_control->searching_mode_mutex);
    success = true;
  }
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return success;
}

bool set_mode_stopped(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  if (thread_control->current_mode == MODE_SEARCHING) {
    thread_control->current_mode = MODE_STOPPED;
    pthread_mutex_unlock(&thread_control->searching_mode_mutex);
    success = true;
  }
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return success;
}

mode_search_status_t get_mode(ThreadControl *thread_control) {
  mode_search_status_t mode;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  mode = thread_control->current_mode;
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return mode;
}

bool set_check_stop_active(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_INACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_ACTIVE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

bool set_check_stop_inactive(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_ACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_INACTIVE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

void print_to_outfile(ThreadControl *thread_control, const char *content) {
  write_to_file(thread_control->outfile, content);
}

void wait_for_mode_stopped(ThreadControl *thread_control) {
  pthread_mutex_lock(&thread_control->searching_mode_mutex);
  // We can only acquire the lock once the search has stopped.
  pthread_mutex_unlock(&thread_control->searching_mode_mutex);
}
#include "thread_control.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../def/file_handler_defs.h"
#include "../def/thread_control_defs.h"

#include "file_handler.h"
#include "timer.h"
#include "xoshiro.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

struct ThreadControl {
  int number_of_threads;
  int print_info_interval;
  int check_stopping_condition_interval;
  uint64_t iter_count;
  XoshiroPRNG *prng;
  check_stop_status_t check_stop_status;
  mode_search_status_t current_mode;
  halt_status_t halt_status;
  pthread_mutex_t check_stopping_condition_mutex;
  pthread_mutex_t current_mode_mutex;
  pthread_mutex_t halt_status_mutex;
  pthread_mutex_t searching_mode_mutex;
  pthread_mutex_t iter_mutex;
  FileHandler *outfile;
  FileHandler *infile;
  Timer *timer;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->halt_status = HALT_STATUS_NONE;
  thread_control->current_mode = MODE_STOPPED;
  thread_control->check_stop_status = CHECK_STOP_INACTIVE;
  thread_control->number_of_threads = 1;
  thread_control->check_stopping_condition_interval = 0;
  thread_control->print_info_interval = 0;
  thread_control->iter_count = 0;
  pthread_mutex_init(&thread_control->current_mode_mutex, NULL);
  pthread_mutex_init(&thread_control->check_stopping_condition_mutex, NULL);
  pthread_mutex_init(&thread_control->halt_status_mutex, NULL);
  pthread_mutex_init(&thread_control->searching_mode_mutex, NULL);
  pthread_mutex_init(&thread_control->iter_mutex, NULL);
  thread_control->outfile = file_handler_create_from_filename(
      STDOUT_FILENAME, FILE_HANDLER_MODE_WRITE);
  thread_control->infile =
      file_handler_create_from_filename(STDIN_FILENAME, FILE_HANDLER_MODE_READ);
  thread_control->timer = mtimer_create();
  thread_control->prng = prng_create(time(NULL));
  return thread_control;
}

void thread_control_destroy(ThreadControl *thread_control) {
  if (!thread_control) {
    return;
  }
  file_handler_destroy(thread_control->outfile);
  file_handler_destroy(thread_control->infile);
  mtimer_destroy(thread_control->timer);
  prng_destroy(thread_control->prng);
  free(thread_control);
}

void thread_control_set_io(ThreadControl *thread_control,
                           const char *in_filename, const char *out_filename) {
  const char *nonnull_in_filename = in_filename;
  if (!nonnull_in_filename) {
    nonnull_in_filename = file_handler_get_filename(thread_control->infile);
  }

  const char *nonnull_out_filename = out_filename;
  if (!nonnull_out_filename) {
    nonnull_out_filename = file_handler_get_filename(thread_control->outfile);
  }

  if (strings_equal(nonnull_in_filename, nonnull_out_filename)) {
    log_warn("in file and out file cannot be the same\n");
    return;
  }

  file_handler_set_filename(thread_control->infile, nonnull_in_filename,
                            FILE_HANDLER_MODE_READ);

  file_handler_set_filename(thread_control->outfile, nonnull_out_filename,
                            FILE_HANDLER_MODE_WRITE);
}

FileHandler *thread_control_get_infile(ThreadControl *thread_control) {
  return thread_control->infile;
}

Timer *thread_control_get_timer(ThreadControl *thread_control) {
  return thread_control->timer;
}

int thread_control_get_print_info_interval(
    const ThreadControl *thread_control) {
  return thread_control->print_info_interval;
}

void thread_control_set_print_info_interval(ThreadControl *thread_control,
                                            int print_info_interval) {
  thread_control->print_info_interval = print_info_interval;
}

int thread_control_get_check_stop_interval(
    const ThreadControl *thread_control) {
  return thread_control->check_stopping_condition_interval;
}

void thread_control_set_check_stop_interval(
    ThreadControl *thread_control, int check_stopping_condition_interval) {
  thread_control->check_stopping_condition_interval =
      check_stopping_condition_interval;
}

halt_status_t thread_control_get_halt_status(ThreadControl *thread_control) {
  halt_status_t halt_status;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  halt_status = thread_control->halt_status;
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return halt_status;
}

bool thread_control_get_is_halted(ThreadControl *thread_control) {
  return thread_control_get_halt_status(thread_control) != HALT_STATUS_NONE;
}

bool thread_control_halt(ThreadControl *thread_control,
                         halt_status_t halt_status) {
  bool success = false;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  // Assume the first reason to thread_control_halt is the only
  // reason we care about, so subsequent calls to thread_control_halt
  // can be ignored.
  if (thread_control->halt_status == HALT_STATUS_NONE &&
      halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = halt_status;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

bool thread_control_unhalt(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->halt_status_mutex);
  if (thread_control->halt_status != HALT_STATUS_NONE) {
    thread_control->halt_status = HALT_STATUS_NONE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->halt_status_mutex);
  return success;
}

bool thread_control_set_mode_searching(ThreadControl *thread_control) {
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

bool thread_control_set_mode_stopped(ThreadControl *thread_control) {
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

mode_search_status_t thread_control_get_mode(ThreadControl *thread_control) {
  mode_search_status_t mode;
  pthread_mutex_lock(&thread_control->current_mode_mutex);
  mode = thread_control->current_mode;
  pthread_mutex_unlock(&thread_control->current_mode_mutex);
  return mode;
}

bool thread_control_set_check_stop_active(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_INACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_ACTIVE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

bool thread_control_set_check_stop_inactive(ThreadControl *thread_control) {
  bool success = false;
  pthread_mutex_lock(&thread_control->check_stopping_condition_mutex);
  if (thread_control->check_stop_status == CHECK_STOP_ACTIVE) {
    thread_control->check_stop_status = CHECK_STOP_INACTIVE;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->check_stopping_condition_mutex);
  return success;
}

// NOT THREAD SAFE: This does not require locking since it is
// not called during a multithreaded commmand
int thread_control_get_threads(const ThreadControl *thread_control) {
  return thread_control->number_of_threads;
}

// NOT THREAD SAFE: This does not require locking since it is
// not called during a multithreaded commmand
void thread_control_set_threads(ThreadControl *thread_control,
                                int number_of_threads) {
  thread_control->number_of_threads = number_of_threads;
}

void thread_control_print(ThreadControl *thread_control, const char *content) {
  file_handler_write(thread_control->outfile, content);
}

void thread_control_wait_for_mode_stopped(ThreadControl *thread_control) {
  pthread_mutex_lock(&thread_control->searching_mode_mutex);
  // We can only acquire the lock once the search has stopped.
  pthread_mutex_unlock(&thread_control->searching_mode_mutex);
}

void thread_control_get_next_iter_output(ThreadControl *thread_control,
                                         ThreadControlIterOutput *iter_output) {
  pthread_mutex_lock(&thread_control->iter_mutex);
  iter_output->seed = prng_next(thread_control->prng);
  iter_output->iter_count = thread_control->iter_count++;
  pthread_mutex_unlock(&thread_control->iter_mutex);
}

// NOT THREAD SAFE: This function is meant to be called
// before a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
void thread_control_prng_seed(ThreadControl *thread_control, uint64_t seed) {
  prng_seed(thread_control->prng, seed);
}

void thread_control_reset_iter_count(ThreadControl *thread_control) {
  thread_control->iter_count = 0;
}
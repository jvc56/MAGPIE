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
  uint64_t iter_count_completed;
  uint64_t max_iter_count;
  double time_elapsed;
  uint64_t seed;
  XoshiroPRNG *prng;
  check_stop_status_t check_stop_status;
  mode_search_status_t current_mode;
  exit_status_t exit_status;
  pthread_mutex_t check_stopping_condition_mutex;
  pthread_mutex_t current_mode_mutex;
  pthread_mutex_t exit_status_mutex;
  pthread_mutex_t searching_mode_mutex;
  pthread_mutex_t iter_mutex;
  pthread_mutex_t iter_completed_mutex;
  FileHandler *outfile;
  FileHandler *infile;
  Timer *timer;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->exit_status = EXIT_STATUS_NONE;
  thread_control->current_mode = MODE_STOPPED;
  thread_control->check_stop_status = CHECK_STOP_INACTIVE;
  thread_control->number_of_threads = 1;
  thread_control->check_stopping_condition_interval = 0;
  thread_control->print_info_interval = 0;
  thread_control->iter_count = 0;
  thread_control->iter_count_completed = 0;
  thread_control->time_elapsed = 0;
  thread_control->max_iter_count = 0;
  pthread_mutex_init(&thread_control->current_mode_mutex, NULL);
  pthread_mutex_init(&thread_control->check_stopping_condition_mutex, NULL);
  pthread_mutex_init(&thread_control->exit_status_mutex, NULL);
  pthread_mutex_init(&thread_control->searching_mode_mutex, NULL);
  pthread_mutex_init(&thread_control->iter_mutex, NULL);
  pthread_mutex_init(&thread_control->iter_completed_mutex, NULL);
  thread_control->outfile = file_handler_create_from_filename(
      STDOUT_FILENAME, FILE_HANDLER_MODE_WRITE);
  thread_control->infile =
      file_handler_create_from_filename(STDIN_FILENAME, FILE_HANDLER_MODE_READ);
  thread_control->timer = mtimer_create();
  thread_control->seed = time(NULL);
  thread_control->prng = prng_create(thread_control->seed);
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

int thread_control_get_print_info_interval(
    const ThreadControl *thread_control) {
  return thread_control->print_info_interval;
}

void thread_control_set_print_info_interval(ThreadControl *thread_control,
                                            int print_info_interval) {
  thread_control->print_info_interval = print_info_interval;
}

uint64_t
thread_control_get_max_iter_count(const ThreadControl *thread_control) {
  return thread_control->max_iter_count;
}

void thread_control_increment_max_iter_count(ThreadControl *thread_control,
                                             uint64_t inc) {
  thread_control->max_iter_count += inc;
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

exit_status_t thread_control_get_exit_status(ThreadControl *thread_control) {
  exit_status_t exit_status;
  pthread_mutex_lock(&thread_control->exit_status_mutex);
  exit_status = thread_control->exit_status;
  pthread_mutex_unlock(&thread_control->exit_status_mutex);
  return exit_status;
}

bool thread_control_get_is_exited(ThreadControl *thread_control) {
  return thread_control_get_exit_status(thread_control) != EXIT_STATUS_NONE;
}

bool thread_control_exit(ThreadControl *thread_control,
                         exit_status_t exit_status) {
  bool success = false;
  pthread_mutex_lock(&thread_control->exit_status_mutex);
  // Assume the first reason to thread_control_exit is the only
  // reason we care about, so subsequent calls to thread_control_exit
  // can be ignored.
  if (thread_control->exit_status == EXIT_STATUS_NONE &&
      exit_status != EXIT_STATUS_NONE) {
    thread_control->exit_status = exit_status;
    success = true;
  }
  pthread_mutex_unlock(&thread_control->exit_status_mutex);
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

// Returns true if the iter_count is already greater than or equal to
// stop_iter_count and does nothing else.
// Returns false if the iter_count is less than the stop_iter_count
// and increments the iter_count and sets the next seed.
bool thread_control_get_next_iter_output(ThreadControl *thread_control,
                                         ThreadControlIterOutput *iter_output) {
  bool at_stop_count = false;
  pthread_mutex_lock(&thread_control->iter_mutex);
  if (thread_control->iter_count >= thread_control->max_iter_count) {
    at_stop_count = true;
  } else {
    iter_output->seed = prng_next(thread_control->prng);
    iter_output->iter_count = thread_control->iter_count++;
  }
  pthread_mutex_unlock(&thread_control->iter_mutex);
  return at_stop_count;
}

// This function should be called when a thread has completed computation
// for an iteration given by thread_control_get_next_iter_output.
// It increments the count completed and records the elapsed time.
void thread_control_complete_iter(
    ThreadControl *thread_control,
    ThreadControlIterCompletedOutput *iter_completed_output) {
  pthread_mutex_lock(&thread_control->iter_completed_mutex);
  // Update internal fields
  thread_control->iter_count_completed++;
  thread_control->time_elapsed = mtimer_elapsed_seconds(thread_control->timer);
  // Set output
  iter_completed_output->iter_count_completed =
      thread_control->iter_count_completed;
  iter_completed_output->time_elapsed = thread_control->time_elapsed;
  iter_completed_output->print_info =
      thread_control->print_info_interval > 0 &&
      thread_control->iter_count_completed %
              thread_control->print_info_interval ==
          0;
  pthread_mutex_unlock(&thread_control->iter_completed_mutex);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
void thread_control_start_timer(ThreadControl *thread_control) {
  mtimer_start(thread_control->timer);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
void thread_control_stop_timer(ThreadControl *thread_control) {
  mtimer_stop(thread_control->timer);
}

double thread_control_get_seconds_elapsed(const ThreadControl *thread_control) {
  return mtimer_elapsed_seconds(thread_control->timer);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
void thread_control_set_seed(ThreadControl *thread_control, uint64_t seed) {
  thread_control->seed = seed;
  prng_seed(thread_control->prng, thread_control->seed);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
uint64_t thread_control_get_seed(ThreadControl *thread_control) {
  return thread_control->seed;
}

// Copies the thread control PRNG to the other PRNG and performs a PRNG
// jump on the thread control PRNG.
void thread_control_copy_to_dst_and_jump(ThreadControl *thread_control,
                                         XoshiroPRNG *dst) {
  prng_copy(dst, thread_control->prng);
  prng_jump(thread_control->prng);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
uint64_t thread_control_get_iter_count(const ThreadControl *thread_control) {
  return thread_control->iter_count;
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
// Resets all iteration counts and resets then starts the timer.
void thread_control_reset(ThreadControl *thread_control,
                          uint64_t max_iter_count) {
  thread_control->iter_count = 0;
  thread_control->iter_count_completed = 0;
  thread_control->time_elapsed = 0;
  thread_control->max_iter_count = max_iter_count;
  thread_control->exit_status = EXIT_STATUS_NONE;
  mtimer_reset(thread_control->timer);
  mtimer_start(thread_control->timer);
}
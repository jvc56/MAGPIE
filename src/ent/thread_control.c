#include "thread_control.h"

#include "../compat/cpthread.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <time.h>

#include "../def/thread_control_defs.h"

#include "timer.h"
#include "xoshiro.h"

#include "../util/io_util.h"

struct ThreadControl {
  int number_of_threads;
  int print_info_interval;
  uint64_t max_iter_count;
  double time_elapsed;
  uint64_t seed;
  XoshiroPRNG *prng;
  FILE *out_fh;
  FILE *in_fh;
  Timer *timer;
  uint64_t iter_count;
  cpthread_mutex_t iter_mutex;
  uint64_t iter_count_completed;
  cpthread_mutex_t iter_completed_mutex;
  thread_control_status_t tc_status;
  cpthread_mutex_t tc_status_mutex;
  cpthread_mutex_t print_mutex;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->tc_status = THREAD_CONTROL_STATUS_UNINITIALIZED;
  thread_control->number_of_threads = 1;
  thread_control->print_info_interval = 0;
  thread_control->iter_count = 0;
  thread_control->iter_count_completed = 0;
  thread_control->time_elapsed = 0;
  thread_control->max_iter_count = 0;
  cpthread_mutex_init(&thread_control->tc_status_mutex);
  cpthread_mutex_init(&thread_control->iter_mutex);
  cpthread_mutex_init(&thread_control->iter_completed_mutex);
  cpthread_mutex_init(&thread_control->print_mutex);
  thread_control->timer = mtimer_create_monotonic();
  thread_control->seed = time(NULL);
  thread_control->prng = prng_create(thread_control->seed);
  return thread_control;
}

void thread_control_destroy(ThreadControl *thread_control) {
  if (!thread_control) {
    return;
  }
  mtimer_destroy(thread_control->timer);
  prng_destroy(thread_control->prng);
  free(thread_control);
}

int thread_control_get_print_info_interval(
    const ThreadControl *thread_control) {
  return thread_control->print_info_interval;
}

void thread_control_set_print_info_interval(ThreadControl *thread_control,
                                            int print_info_interval) {
  thread_control->print_info_interval = print_info_interval;
}

thread_control_status_t
thread_control_get_status(ThreadControl *thread_control) {
  thread_control_status_t tc_status;
  cpthread_mutex_lock(&thread_control->tc_status_mutex);
  tc_status = thread_control->tc_status;
  cpthread_mutex_unlock(&thread_control->tc_status_mutex);
  return tc_status;
}

bool thread_control_is_winding_down_status(thread_control_status_t tc_status) {
  bool is_winding_down;
  switch (tc_status) {
  case THREAD_CONTROL_STATUS_UNINITIALIZED:
  case THREAD_CONTROL_STATUS_STARTED:
  case THREAD_CONTROL_STATUS_FINISHED:
    is_winding_down = false;
    break;
  default:
    is_winding_down = true;
    break;
  }
  return is_winding_down;
}

bool thread_control_is_started_status(thread_control_status_t tc_status) {
  return tc_status == THREAD_CONTROL_STATUS_STARTED;
}

bool thread_control_is_winding_down(ThreadControl *thread_control) {
  return thread_control_is_winding_down_status(
      thread_control_get_status(thread_control));
}

bool thread_control_is_finished(ThreadControl *thread_control) {
  return thread_control_get_status(thread_control) ==
         THREAD_CONTROL_STATUS_FINISHED;
}

bool thread_control_is_started(ThreadControl *thread_control) {
  return thread_control_is_started_status(
      thread_control_get_status(thread_control));
}

bool thread_control_is_ready_for_new_command(ThreadControl *thread_control) {
  const thread_control_status_t tc_status =
      thread_control_get_status(thread_control);
  bool is_ready;
  switch (tc_status) {
  case THREAD_CONTROL_STATUS_UNINITIALIZED:
  case THREAD_CONTROL_STATUS_FINISHED:
    is_ready = true;
    break;
  default:
    is_ready = false;
    break;
  }
  return is_ready;
}

bool thread_control_is_sim_printable(ThreadControl *thread_control,
                                     const bool simmed_plays_initialized) {
  bool is_printable;
  switch (thread_control_get_status(thread_control)) {
  case THREAD_CONTROL_STATUS_UNINITIALIZED:
    is_printable = false;
    break;
  case THREAD_CONTROL_STATUS_STARTED:
    is_printable = simmed_plays_initialized;
    break;
  case THREAD_CONTROL_STATUS_MAX_ITERATIONS:
  case THREAD_CONTROL_STATUS_USER_INTERRUPT:
  case THREAD_CONTROL_STATUS_THRESHOLD:
  case THREAD_CONTROL_STATUS_SAMPLE_LIMIT:
  case THREAD_CONTROL_STATUS_TIMEOUT:
  case THREAD_CONTROL_STATUS_ONE_ARM_REMAINING:
  case THREAD_CONTROL_STATUS_FINISHED:
    is_printable = true;
    break;
  }
  return is_printable;
}

void thread_control_verify_state_change(thread_control_status_t old_status,
                                        thread_control_status_t new_status) {
  if (new_status == THREAD_CONTROL_STATUS_UNINITIALIZED ||
      (old_status == THREAD_CONTROL_STATUS_UNINITIALIZED &&
       new_status != THREAD_CONTROL_STATUS_STARTED) ||
      (old_status != THREAD_CONTROL_STATUS_UNINITIALIZED &&
       old_status != THREAD_CONTROL_STATUS_FINISHED &&
       new_status == THREAD_CONTROL_STATUS_STARTED)) {
    log_fatal("invalid thread control state transition: %d -> %d\n", old_status,
              new_status);
  }
}

bool thread_control_is_noop_state_change(thread_control_status_t old_status,
                                         thread_control_status_t new_status) {
  return thread_control_is_winding_down_status(old_status) &&
         thread_control_is_winding_down_status(new_status);
}

// Returns true if the status was set successfully.
// Returns false if the exit status remains unchanged.
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t new_status) {
  bool success = false;
  cpthread_mutex_lock(&thread_control->tc_status_mutex);
  const thread_control_status_t old_status = thread_control->tc_status;
  thread_control_verify_state_change(old_status, new_status);
  // Only set the status to some specific exit reason if it is not already set
  // to some other winding down reason.
  if (!thread_control_is_noop_state_change(old_status, new_status)) {
    thread_control->tc_status = new_status;
    success = true;
    // Reset the thread control
    if (new_status == THREAD_CONTROL_STATUS_STARTED) {
      thread_control->iter_count = 0;
      thread_control->iter_count_completed = 0;
      thread_control->time_elapsed = 0;
      mtimer_start(thread_control->timer);
    }
  }
  cpthread_mutex_unlock(&thread_control->tc_status_mutex);
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
  cpthread_mutex_lock(&thread_control->print_mutex);
  write_to_stream_out(content);
  cpthread_mutex_unlock(&thread_control->print_mutex);
}

// Returns true if the iter_count is already greater than or equal to
// stop_iter_count and does nothing else.
// Returns false if the iter_count is less than the stop_iter_count
// and increments the iter_count and sets the next seed.
bool thread_control_get_next_iter_output(ThreadControl *thread_control,
                                         ThreadControlIterOutput *iter_output) {
  bool at_stop_count = false;
  cpthread_mutex_lock(&thread_control->iter_mutex);
  if (thread_control->iter_count >= thread_control->max_iter_count) {
    at_stop_count = true;
  } else {
    iter_output->seed = prng_next(thread_control->prng);
    iter_output->iter_count = thread_control->iter_count++;
  }
  cpthread_mutex_unlock(&thread_control->iter_mutex);
  return at_stop_count;
}

// This function should be called when a thread has completed computation
// for an iteration given by thread_control_get_next_iter_output.
// It increments the count completed and records the elapsed time.
void thread_control_complete_iter(
    ThreadControl *thread_control,
    ThreadControlIterCompletedOutput *iter_completed_output) {
  cpthread_mutex_lock(&thread_control->iter_completed_mutex);
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
  cpthread_mutex_unlock(&thread_control->iter_completed_mutex);
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
void thread_control_increment_seed(ThreadControl *thread_control) {
  thread_control->seed++;
  prng_seed(thread_control->prng, thread_control->seed);
}

// NOT THREAD SAFE: This function is meant to be called
// before or after a multithreaded operation. Do not call this in a
// multithreaded context as it is intentionally not thread safe.
uint64_t thread_control_get_seed(const ThreadControl *thread_control) {
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
void thread_control_set_max_iter_count(ThreadControl *thread_control,
                                       uint64_t max_iter_count) {
  thread_control->max_iter_count = max_iter_count;
}
#include "thread_control.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct ThreadControl {
  _Atomic int status; // atomic for lock-free reads on the hot search path
  cpthread_mutex_t status_mutex; // protects cond var signaling only
  cpthread_cond_t status_cond;
  cpthread_mutex_t print_mutex;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  atomic_init(&thread_control->status, THREAD_CONTROL_STATUS_UNINITIALIZED);
  cpthread_mutex_init(&thread_control->status_mutex);
  cpthread_mutex_init(&thread_control->print_mutex);
  cpthread_cond_init(&thread_control->status_cond);
  return thread_control;
}

void thread_control_destroy(ThreadControl *thread_control) {
  if (!thread_control) {
    return;
  }
  free(thread_control);
}

thread_control_status_t
thread_control_get_status(ThreadControl *thread_control) {
  // Lock-free read: avoids mutex contention on the hot per-node search path.
  // acquire ordering ensures we see any prior store from the timer thread.
  return (thread_control_status_t)atomic_load_explicit(
      &thread_control->status, memory_order_acquire);
}

// Returns true if the status was set successfully.
// Returns false if the exit status remains unchanged.
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t new_status) {
  bool success = false;
  // Still hold the mutex here so the cond broadcast is correctly paired
  // with cond_wait in thread_control_wait_for_status_change.
  cpthread_mutex_lock(&thread_control->status_mutex);
  const thread_control_status_t old_status =
      (thread_control_status_t)atomic_load_explicit(&thread_control->status,
                                                    memory_order_relaxed);
  if (new_status != old_status) {
    // release ordering so search threads' acquire-load sees the update.
    atomic_store_explicit(&thread_control->status, new_status,
                          memory_order_release);
    success = true;
    if (new_status != THREAD_CONTROL_STATUS_STARTED) {
      cpthread_cond_broadcast(&thread_control->status_cond);
    }
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return success;
}

void thread_control_wait_for_status_change(ThreadControl *thread_control) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  if ((thread_control_status_t)atomic_load_explicit(
          &thread_control->status, memory_order_relaxed) !=
      THREAD_CONTROL_STATUS_FINISHED) {
    cpthread_cond_wait(&thread_control->status_cond,
                       &thread_control->status_mutex);
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
}

void thread_control_print(ThreadControl *thread_control, const char *content) {
  cpthread_mutex_lock(&thread_control->print_mutex);
  write_to_stream_out("%s", content);
  cpthread_mutex_unlock(&thread_control->print_mutex);
}
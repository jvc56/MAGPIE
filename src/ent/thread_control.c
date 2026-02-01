#include "thread_control.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct ThreadControl {
  thread_control_status_t status;
  cpthread_mutex_t status_mutex;
  cpthread_cond_t status_cond;
  cpthread_mutex_t print_mutex;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  thread_control->status = THREAD_CONTROL_STATUS_UNINITIALIZED;
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
#ifdef __EMSCRIPTEN__
  // WASM: Skip mutex due to Emscripten pthread issues
  return thread_control->status;
#else
  thread_control_status_t status;
  cpthread_mutex_lock(&thread_control->status_mutex);
  status = thread_control->status;
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return status;
#endif
}

// Lock-free read - no mutex, just read the status directly
// This is unsafe in general but works for WASM where we're just polling
thread_control_status_t
thread_control_get_status_unsafe(ThreadControl *thread_control) {
  return thread_control->status;
}

// Returns true if the status was set successfully.
// Returns false if the exit status remains unchanged.
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t new_status) {
#ifdef __EMSCRIPTEN__
  // WASM: Skip mutex due to Emscripten pthread issues
  // This is unsafe but works for our use case
  const thread_control_status_t old_status = thread_control->status;
  if (new_status != old_status) {
    thread_control->status = new_status;
    return true;
  }
  return false;
#else
  bool success = false;
  cpthread_mutex_lock(&thread_control->status_mutex);
  const thread_control_status_t old_status = thread_control->status;
  if (new_status != old_status) {
    thread_control->status = new_status;
    success = true;
    if (new_status != THREAD_CONTROL_STATUS_STARTED) {
      cpthread_cond_broadcast(&thread_control->status_cond);
    }
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return success;
#endif
}

void thread_control_wait_for_status_change(ThreadControl *thread_control) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  if (thread_control->status != THREAD_CONTROL_STATUS_FINISHED) {
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
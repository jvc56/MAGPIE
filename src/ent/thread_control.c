#include "thread_control.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../util/io_util.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
  thread_control_status_t status;
  cpthread_mutex_lock(&thread_control->status_mutex);
  status = thread_control->status;
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return status;
}

// Returns true if the status was set successfully.
// Returns false if the exit status remains unchanged.
bool thread_control_set_status(ThreadControl *thread_control,
                               thread_control_status_t new_status) {
  bool success = false;
  cpthread_mutex_lock(&thread_control->status_mutex);
  const thread_control_status_t old_status = thread_control->status;
  // Only set the status to some specific exit reason if it is not already set
  // to some other winding down reason.
  if (new_status != old_status) {
    thread_control->status = new_status;
    success = true;
    // Reset the thread control
    if (new_status != THREAD_CONTROL_STATUS_STARTED) {
      cpthread_cond_broadcast(&thread_control->status_cond);
    }
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return success;
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
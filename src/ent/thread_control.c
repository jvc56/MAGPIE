#include "thread_control.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../util/io_util.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct ThreadControl {
  _Atomic bool paused;
  _Atomic int status; // atomic for lock-free reads on the hot search path
  cpthread_mutex_t status_mutex; // protects cond var signaling only
  cpthread_cond_t status_cond;
  cpthread_mutex_t print_mutex;
  // See thread_control_set_parent. A parent's stop is pushed down to its
  // children when it happens, so reading a status never looks past its own
  // thread control. The children are a list through next_sibling, under the
  // parent's status_mutex; a child's mutex is only ever taken inside its
  // parent's.
  ThreadControl *parent;
  ThreadControl *first_child;
  ThreadControl *next_sibling;
  // Whether thread_control_stop was called since the status last became
  // STARTED, whether or not it changed the status. Under status_mutex.
  bool stop_requested;
};

ThreadControl *thread_control_create(void) {
  ThreadControl *thread_control = malloc_or_die(sizeof(ThreadControl));
  atomic_init(&thread_control->paused, false);
  atomic_init(&thread_control->status, THREAD_CONTROL_STATUS_UNINITIALIZED);
  cpthread_mutex_init(&thread_control->status_mutex);
  cpthread_mutex_init(&thread_control->print_mutex);
  cpthread_cond_init(&thread_control->status_cond);
  thread_control->parent = NULL;
  thread_control->first_child = NULL;
  thread_control->next_sibling = NULL;
  thread_control->stop_requested = false;
  return thread_control;
}

void thread_control_destroy(ThreadControl *thread_control) {
  if (!thread_control) {
    return;
  }
  thread_control_set_parent(thread_control, NULL);
  free(thread_control);
}

thread_control_status_t
thread_control_get_status(ThreadControl *thread_control) {
  // Lock-free read: avoids mutex contention on the hot per-node search path.
  // acquire ordering ensures we see any prior store from the timer thread.
  return (thread_control_status_t)atomic_load_explicit(&thread_control->status,
                                                       memory_order_acquire);
}

// Passes a stop on to the children. Called with the status_mutex held.
static void thread_control_stop_children(ThreadControl *thread_control) {
  for (ThreadControl *child = thread_control->first_child; child != NULL;
       child = child->next_sibling) {
    (void)thread_control_set_status(child,
                                    THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
}

void thread_control_set_parent(ThreadControl *thread_control,
                               ThreadControl *parent) {
  ThreadControl *previous = thread_control->parent;
  if (previous != NULL) {
    cpthread_mutex_lock(&previous->status_mutex);
    ThreadControl **link = &previous->first_child;
    while (*link != thread_control) {
      link = &(*link)->next_sibling;
    }
    *link = thread_control->next_sibling;
    cpthread_mutex_unlock(&previous->status_mutex);
    thread_control->next_sibling = NULL;
  }
  thread_control->parent = parent;
  if (parent == NULL) {
    return;
  }
  cpthread_mutex_lock(&parent->status_mutex);
  thread_control->next_sibling = parent->first_child;
  parent->first_child = thread_control;
  // A parent already stopped stops the child now.
  if (thread_control_get_status(parent) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    (void)thread_control_set_status(thread_control,
                                    THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  cpthread_mutex_unlock(&parent->status_mutex);
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
    atomic_store_explicit(&thread_control->paused, false, memory_order_release);
    success = true;
    if (new_status == THREAD_CONTROL_STATUS_STARTED) {
      thread_control->stop_requested = false;
    } else {
      cpthread_cond_broadcast(&thread_control->status_cond);
    }
    if (new_status == THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      thread_control_stop_children(thread_control);
    }
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return success;
}

void thread_control_stop(ThreadControl *thread_control) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  thread_control->stop_requested = true;
  int expected = THREAD_CONTROL_STATUS_STARTED;
  // Publish cancellation to search threads that acquire-load the status.
  if (atomic_compare_exchange_strong_explicit(
          &thread_control->status, &expected,
          THREAD_CONTROL_STATUS_USER_INTERRUPT, memory_order_release,
          memory_order_relaxed)) {
    atomic_store_explicit(&thread_control->paused, false, memory_order_release);
    cpthread_cond_broadcast(&thread_control->status_cond);
    thread_control_stop_children(thread_control);
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
}

bool thread_control_restore_unless_stopped(ThreadControl *thread_control,
                                           thread_control_status_t status) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  const bool restore = !thread_control->stop_requested;
  if (restore) {
    atomic_store_explicit(&thread_control->status, status,
                          memory_order_release);
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return restore;
}

bool thread_control_set_paused(ThreadControl *thread_control, bool paused) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  const bool running = thread_control_get_status(thread_control) ==
                       THREAD_CONTROL_STATUS_STARTED;
  if (running) {
    atomic_store_explicit(&thread_control->paused, paused,
                          memory_order_release);
    if (!paused) {
      cpthread_cond_broadcast(&thread_control->status_cond);
    }
  }
  cpthread_mutex_unlock(&thread_control->status_mutex);
  return running;
}

thread_control_status_t
thread_control_search_checkpoint(ThreadControl *thread_control) {
  // Only search workers may wait here. Status polling must remain nonblocking
  // so an embedding can receive Resume/Stop while all search workers sleep.
  if (atomic_load_explicit(&thread_control->paused, memory_order_acquire)) {
    cpthread_mutex_lock(&thread_control->status_mutex);
    while (
        atomic_load_explicit(&thread_control->paused, memory_order_relaxed) &&
        thread_control_get_status(thread_control) ==
            THREAD_CONTROL_STATUS_STARTED) {
      cpthread_cond_wait(&thread_control->status_cond,
                         &thread_control->status_mutex);
    }
    cpthread_mutex_unlock(&thread_control->status_mutex);
  }
  return thread_control_get_status(thread_control);
}

void thread_control_wait_for_status_change(ThreadControl *thread_control) {
  cpthread_mutex_lock(&thread_control->status_mutex);
  while ((thread_control_status_t)atomic_load_explicit(&thread_control->status,
                                                       memory_order_relaxed) !=
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

void thread_control_print_formatted(ThreadControl *thread_control,
                                    const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *content = format_string_with_va_list(fmt, &args);
  va_end(args);
  if (content) {
    thread_control_print(thread_control, content);
    free(content);
  }
}

void thread_control_print_err(ThreadControl *thread_control,
                              const char *content) {
  cpthread_mutex_lock(&thread_control->print_mutex);
  write_to_stream(get_stream_err(), "%s", content);
  cpthread_mutex_unlock(&thread_control->print_mutex);
}

void thread_control_print_formatted_err(ThreadControl *thread_control,
                                        const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char *content = format_string_with_va_list(fmt, &args);
  va_end(args);
  if (content) {
    thread_control_print_err(thread_control, content);
    free(content);
  }
}
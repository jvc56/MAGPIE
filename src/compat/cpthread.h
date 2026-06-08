#ifndef COMPAT_PTHREAD_H
#define COMPAT_PTHREAD_H

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "../def/cpthread_defs.h"
#include "../util/io_util.h"

static inline void cpthread_mutex_init(cpthread_mutex_t *mutex) {
  if (pthread_mutex_init(mutex, NULL)) {
    log_fatal("mutex init failed");
  }
}

static inline void cpthread_mutex_lock(cpthread_mutex_t *mutex) {
  if (pthread_mutex_lock(mutex)) {
    log_fatal("mutex lock failed");
  }
}

static inline void cpthread_mutex_unlock(cpthread_mutex_t *mutex) {
  if (pthread_mutex_unlock(mutex)) {
    log_fatal("mutex unlock failed");
  }
}

static inline void cpthread_cond_init(cpthread_cond_t *cond) {
  if (pthread_cond_init(cond, NULL)) {
    log_fatal("cond init failed");
  }
}

static inline void cpthread_cond_wait(cpthread_cond_t *cond,
                                      cpthread_mutex_t *mutex) {
  if (pthread_cond_wait(cond, mutex)) {
    log_fatal("cond wait failed");
  }
}

static inline void cpthread_cond_timedwait(cpthread_cond_t *cond,
                                           cpthread_mutex_t *mutex,
                                           const struct timespec *abstime) {
  int ret = pthread_cond_timedwait(cond, mutex, abstime);
  if (ret == 0) {
    return;
  }
  if (ret == ETIMEDOUT) {
    log_fatal("cond timedwait timed out");
  }
  log_fatal("cond timedwait failed");
}

static inline void cpthread_cond_timedwait_loop(cpthread_cond_t *cond,
                                                cpthread_mutex_t *mutex,
                                                const int timeout_seconds,
                                                const int *done) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_seconds;
  cpthread_mutex_lock(mutex);
  while (!done) {
    cpthread_cond_timedwait(cond, mutex, &ts);
  }
  cpthread_mutex_unlock(mutex);
}

static inline void cpthread_cond_signal(cpthread_cond_t *cond) {
  if (pthread_cond_signal(cond)) {
    log_fatal("cond signal failed");
  }
}

static inline void cpthread_cond_broadcast(cpthread_cond_t *cond) {
  if (pthread_cond_broadcast(cond)) {
    log_fatal("cond broadcast failed");
  }
}

static inline void cpthread_create(pthread_t *newthread,
                                   void *(*start_routine)(void *), void *arg) {
  if (pthread_create(newthread, NULL, start_routine, arg)) {
    log_fatal("thread create failed");
  }
}

// Like cpthread_create, but requests a specific stack size (bytes) for the
// new thread. The platform default secondary-thread stack is small (512 KB on
// macOS); callers whose work recurses deeply on the worker stack — e.g. a
// reentrant help-while-waiting thread pool — must request a larger stack to
// avoid a guard-page fault (SIGBUS) that prints no diagnostic. stack_bytes is
// rounded up to a page by the platform; pass a page-multiple to be explicit.
static inline void cpthread_create_with_stack(pthread_t *newthread,
                                              void *(*start_routine)(void *),
                                              void *arg, size_t stack_bytes) {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr)) {
    log_fatal("thread attr init failed");
  }
  if (pthread_attr_setstacksize(&attr, stack_bytes)) {
    log_fatal("thread attr setstacksize failed");
  }
  if (pthread_create(newthread, &attr, start_routine, arg)) {
    log_fatal("thread create failed");
  }
  // attr is a value type with no embedded allocations on the platforms we
  // target; destroying it releases any attr-internal state and is required by
  // POSIX (this is an attr object, not a mutex/cond — see CLAUDE.md).
  pthread_attr_destroy(&attr);
}

static inline void cpthread_join(cpthread_t th) {
  if (pthread_join(th, NULL)) {
    log_fatal("thread join failed");
  }
}

static inline void cpthread_detach(cpthread_t thread) {
  if (pthread_detach(thread)) {
    log_fatal("thread detach failed");
  }
}

static inline void cpthread_cancel(cpthread_t thread) {
  if (pthread_cancel(thread)) {
    log_fatal("thread cancel failed");
  }
}

static inline void cpthread_key_create(cpthread_key_t *key,
                                       void (*destructor)(void *)) {
  if (pthread_key_create(key, destructor)) {
    log_fatal("thread key create failed");
  }
}

static inline void cpthread_once(cpthread_once_t *once_control,
                                 void (*init_routine)(void)) {
  if (pthread_once(once_control, init_routine)) {
    log_fatal("thread once failed");
  }
}

static inline void *cpthread_getspecific(cpthread_key_t key) {
  return pthread_getspecific(key);
}

static inline void cpthread_setspecific(cpthread_key_t key,
                                        const void *value) {
  if (pthread_setspecific(key, value)) {
    log_fatal("thread setspecific failed");
  }
}

#endif

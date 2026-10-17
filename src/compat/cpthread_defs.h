#ifndef CPTHREAD_DEFS_H
#define CPTHREAD_DEFS_H

#include <pthread.h>

// The threading types, kept here so that code outside src/compat names no
// pthreads type. pthreads is native on POSIX and winpthreads (linked
// statically) on Windows.
typedef pthread_t cpthread_t;
typedef pthread_mutex_t cpthread_mutex_t;
typedef pthread_cond_t cpthread_cond_t;
typedef pthread_key_t cpthread_key_t;
typedef pthread_once_t cpthread_once_t;

#define CPTHREAD_ONCE_INIT PTHREAD_ONCE_INIT
#define CPTHREAD_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define CPTHREAD_COND_INITIALIZER PTHREAD_COND_INITIALIZER

// Locks and unlocks returning pthreads' status rather than ending the process
// as cpthread.h's do, for io_util, which those report failures through.
static inline int cpthread_mutex_lock_status(cpthread_mutex_t *mutex) {
  return pthread_mutex_lock(mutex);
}

static inline int cpthread_mutex_unlock_status(cpthread_mutex_t *mutex) {
  return pthread_mutex_unlock(mutex);
}

#endif

#ifndef THREAD_CONTROL_DEFS_H
#define THREAD_CONTROL_DEFS_H

typedef enum {
  THREAD_CONTROL_STATUS_UNINITIALIZED,
  THREAD_CONTROL_STATUS_STARTED,
  THREAD_CONTROL_STATUS_MAX_ITERATIONS,
  THREAD_CONTROL_STATUS_USER_INTERRUPT,
  THREAD_CONTROL_STATUS_THRESHOLD,
  THREAD_CONTROL_STATUS_SAMPLE_LIMIT,
  THREAD_CONTROL_STATUS_TIMEOUT,
  THREAD_CONTROL_STATUS_ONE_ARM_REMAINING,
  THREAD_CONTROL_STATUS_FINISHED,
} thread_control_status_t;

enum { MAX_THREADS = 512 };

#endif
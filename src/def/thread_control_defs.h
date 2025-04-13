#ifndef THREAD_CONTROL_DEFS_H
#define THREAD_CONTROL_DEFS_H

typedef enum {
  CHECK_STOP_INACTIVE,
  CHECK_STOP_ACTIVE,
} check_stop_status_t;

typedef enum {
  MODE_STOPPED,
  MODE_SEARCHING,
} mode_search_status_t;

typedef enum {
  HALT_STATUS_NONE,
  // FIXME: the probabilistic enum should be removed
  HALT_STATUS_PROBABILISTIC,
  HALT_STATUS_MAX_ITERATIONS,
  HALT_STATUS_USER_INTERRUPT,
} halt_status_t;

#define MAX_THREADS 512

#endif
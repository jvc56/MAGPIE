#ifndef TIMER_H
#define TIMER_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "../util/io_util.h"

typedef struct timespec TimeSpec;

// Not thread safe
typedef struct Timer {
  TimeSpec start_time;
  TimeSpec end_time;
  bool is_running;
} Timer;

static inline void ctimer_reset(Timer *timer) { timer->is_running = false; }

static inline void ctimer_start(Timer *timer) {
  clock_gettime(CLOCK_MONOTONIC, &timer->start_time);
  timer->is_running = true;
}

static inline void ctimer_stop(Timer *timer) {
  clock_gettime(CLOCK_MONOTONIC, &timer->end_time);
  timer->is_running = false;
}

static inline double ctimer_elapsed_seconds(const Timer *timer) {
  TimeSpec finish_time;
  if (timer->is_running) {
    clock_gettime(CLOCK_MONOTONIC, &finish_time);
  } else {
    finish_time.tv_sec = timer->end_time.tv_sec;
    finish_time.tv_nsec = timer->end_time.tv_nsec;
  }
  double elapsed = (double)(finish_time.tv_sec - timer->start_time.tv_sec);
  elapsed +=
      (double)(finish_time.tv_nsec - timer->start_time.tv_nsec) / 1000000000.0;
  return elapsed;
}

static inline time_t ctime_get_current_time(void) { return time(NULL); }

static inline int ctime_nanosleep(TimeSpec *req, TimeSpec *rem) {
  return nanosleep(req, rem);
}

static inline void ctime_nap(double const seconds) {
  long const secs = (long)seconds;
  double const frac = seconds - (double)secs;
  TimeSpec req;
  TimeSpec rem;

  if (seconds <= 0.0) {
    return;
  }

  req.tv_sec = (time_t)secs;
  req.tv_nsec = (long)(1000000000.0 * frac);
  if (req.tv_nsec > 999999999L) {
    req.tv_nsec = 999999999L;
  }

  rem.tv_sec = (time_t)0;
  rem.tv_nsec = 0L;

  ctime_nanosleep(&req, &rem);
}

static inline void ctimer_clock_gettime_realtime(TimeSpec *ts) {
  clock_gettime(CLOCK_REALTIME, ts);
}

// Returns the current CLOCK_MONOTONIC time as nanoseconds since an arbitrary
// epoch. Suitable for computing absolute deadlines and durations.
static inline int64_t ctimer_monotonic_ns(void) {
  TimeSpec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

#endif
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

// Sleeps at least `seconds` by CLOCK_MONOTONIC. On Windows, nanosleep sleeps
// in whole milliseconds, can wake up to a clock tick (about 15.6 ms) early,
// and returns at once for less than a millisecond; elsewhere a signal can cut
// it short. So it sleeps again for what is left, rounded up to a whole
// millisecond, which blocks every time rather than spinning.
static inline void ctime_nap(double const seconds) {
  if (seconds <= 0.0) {
    return;
  }
  const int64_t deadline_ns =
      ctimer_monotonic_ns() + (int64_t)(seconds * 1000000000.0);
  for (;;) {
    const int64_t left_ns = deadline_ns - ctimer_monotonic_ns();
    if (left_ns <= 0) {
      return;
    }
    const int64_t left_ms = (left_ns + 999999) / 1000000;
    TimeSpec req = {.tv_sec = (time_t)(left_ms / 1000),
                    .tv_nsec = (long)((left_ms % 1000) * 1000000)};
    (void)ctime_nanosleep(&req, NULL);
  }
}

#endif

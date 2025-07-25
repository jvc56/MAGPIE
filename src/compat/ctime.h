#ifndef TIMER_H
#define TIMER_H

#include "../compat/cpthread.h"
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
  int clock_type;
  TimeSpec start_time;
  TimeSpec end_time;
  bool is_running;
} Timer;

static inline void ctimer_reset(Timer *timer) { timer->is_running = false; }

static inline Timer *ctimer_create(int clock_type) {
  Timer *timer = (Timer *)malloc_or_die(sizeof(Timer));
  timer->clock_type = clock_type;
  ctimer_reset(timer);
  return timer;
}

static inline Timer *ctimer_create_monotonic(void) {
  return ctimer_create(CLOCK_MONOTONIC);
}

static inline void ctimer_destroy(Timer *timer) {
  if (!timer) {
    return;
  }
  free(timer);
}

static inline void ctimer_start(Timer *timer) {
  clock_gettime(timer->clock_type, &timer->start_time);
  timer->is_running = true;
}

static inline double ctimer_elapsed_seconds(const Timer *timer) {
  TimeSpec finish_time;
  if (timer->is_running) {
    clock_gettime(timer->clock_type, &finish_time);
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

static inline void ctime_write_current_time(char *time_buf) {
  time_t current_time = ctime_get_current_time();
  time_buf[strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                    localtime(&current_time))] = '\0';
}

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

#endif
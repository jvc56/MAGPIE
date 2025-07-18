#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>

#include <time.h>

#include "../util/io_util.h"

// Not thread safe
typedef struct Timer {
  int clock_type;
  struct timespec start_time;
  struct timespec end_time;
  bool is_running;
} Timer;

static inline void mtimer_reset(Timer *timer) { timer->is_running = false; }

static inline Timer *mtimer_create(int clock_type) {
  Timer *timer = (Timer *)malloc_or_die(sizeof(Timer));
  timer->clock_type = clock_type;
  mtimer_reset(timer);
  return timer;
}

static inline void mtimer_destroy(Timer *timer) {
  if (!timer) {
    return;
  }
  free(timer);
}

static inline void mtimer_start(Timer *timer) {
  clock_gettime(timer->clock_type, &timer->start_time);
  timer->is_running = true;
}

static inline double mtimer_elapsed_seconds(const Timer *timer) {
  struct timespec finish_time;
  if (timer->is_running) {
    clock_gettime(timer->clock_type, &finish_time);
  } else {
    finish_time.tv_sec = timer->end_time.tv_sec;
    finish_time.tv_nsec = timer->end_time.tv_nsec;
  }
  double elapsed = (finish_time.tv_sec - timer->start_time.tv_sec);
  elapsed += (finish_time.tv_nsec - timer->start_time.tv_nsec) / 1000000000.0;
  return elapsed;
}

#endif
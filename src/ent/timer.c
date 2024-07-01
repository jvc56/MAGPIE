#include "timer.h"

#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "../util/log.h"
#include "../util/util.h"

// Not thread safe
struct Timer {
  clock_t start_time;
  clock_t end_time;
  bool is_running;
};

void mtimer_reset(Timer *timer) {
  timer->start_time = 0;
  timer->end_time = 0;
  timer->is_running = false;
}

Timer *mtimer_create(void) {
  Timer *timer = (Timer *)malloc_or_die(sizeof(Timer));
  mtimer_reset(timer);
  return timer;
}

void mtimer_destroy(Timer *timer) {
  if (!timer) {
    return;
  }
  free(timer);
}

void mtimer_start(Timer *timer) {
  if (timer->is_running) {
    log_fatal("Timer is already running.");
  }

  timer->start_time = clock();
  timer->is_running = true;
}

void mtimer_stop(Timer *timer) {
  if (!timer->is_running) {
    log_fatal("Timer is not running or has already stopped.");
  }

  timer->end_time = clock();
  timer->is_running = false;
}

double mtimer_elapsed_seconds(const Timer *timer) {
  if (timer->is_running) {
    clock_t current_time = clock();
    return ((double)(current_time - timer->start_time)) / CLOCKS_PER_SEC;
  } else {
    return ((double)(timer->end_time - timer->start_time)) / CLOCKS_PER_SEC;
  }
}

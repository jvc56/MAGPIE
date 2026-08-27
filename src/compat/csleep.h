#ifndef COMPAT_CSLEEP_H
#define COMPAT_CSLEEP_H

// Blocking sleep. Used by the contribution client to back off between polls
// and after a rate-limit response.

#if defined(_WIN32)

#include <windows.h>

static inline void csleep_seconds(double seconds) {
  if (seconds > 0) {
    Sleep((DWORD)(seconds * 1000.0));
  }
}

#else

#include <time.h>

static inline void csleep_seconds(double seconds) {
  if (seconds <= 0) {
    return;
  }
  struct timespec request;
  request.tv_sec = (time_t)seconds;
  request.tv_nsec = (long)((seconds - (double)request.tv_sec) * 1e9);
  // Resume after a signal rather than returning early: callers are expressing
  // a minimum wait, not a cancellation point.
  while (nanosleep(&request, &request) == -1) {
  }
}

#endif

#endif

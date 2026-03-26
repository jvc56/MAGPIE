#ifndef COMPAT_CSCHED_H
#define COMPAT_CSCHED_H

// Platform-portable sched_yield wrapper.
// sched_yield is not available in wasm builds.
#ifdef __wasm__
static inline void compat_sched_yield(void) {}
#else
#include <sched.h>
static inline void compat_sched_yield(void) { sched_yield(); }
#endif

#endif

#ifndef COMPAT_PTHREAD_TYPES_H
#define COMPAT_PTHREAD_TYPES_H

#include <pthread.h>

typedef pthread_t cpthread_t;
typedef pthread_mutex_t cpthread_mutex_t;
typedef pthread_cond_t cpthread_cond_t;

#endif

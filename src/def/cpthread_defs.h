#ifndef CPTHREAD_DEFS_H
#define CPTHREAD_DEFS_H

#include <pthread.h>

typedef pthread_t cpthread_t;
typedef pthread_mutex_t cpthread_mutex_t;
typedef pthread_cond_t cpthread_cond_t;
typedef pthread_key_t cpthread_key_t;
typedef pthread_once_t cpthread_once_t;

#define CPTHREAD_ONCE_INIT PTHREAD_ONCE_INIT

#endif

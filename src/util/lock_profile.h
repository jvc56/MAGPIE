#ifndef LOCK_PROFILE_H
#define LOCK_PROFILE_H

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"

typedef enum {
  LOCK_PROFILE_SITE_BAI_STATUS,
  LOCK_PROFILE_SITE_BAI_NEXT_ARM,
  LOCK_PROFILE_SITE_BAI_UPDATE,
  LOCK_PROFILE_SITE_SIM_SEED,
  LOCK_PROFILE_SITE_SIM_PLY_STATS,
  LOCK_PROFILE_SITE_SIM_EQUITY_STATS,
  LOCK_PROFILE_SITE_SIM_WIN_PCT_STATS,
  LOCK_PROFILE_SITE_SIM_UTILITY_STATS,
  LOCK_PROFILE_SITE_DISPLAY,
  LOCK_PROFILE_SITE_INITIAL_CHECKPOINT,
  LOCK_PROFILE_SITE_AVOID_PRUNE_CHECKPOINT,
  LOCK_PROFILE_SITE_THREAD_CONTROL,
  LOCK_PROFILE_SITE_BAI_WORKER_JOIN,
  LOCK_PROFILE_SITE_AUTOPLAY_WORKER_JOIN,
  LOCK_PROFILE_SITE_INFERENCE_WORKER_JOIN,
  LOCK_PROFILE_SITE_OTHER_CHECKPOINT,
  NUM_LOCK_PROFILE_SITES,
} lock_profile_site_t;

#ifdef MAGPIE_LOCK_PROFILE

void lock_profile_mutex_lock(cpthread_mutex_t *mutex, lock_profile_site_t site);
void lock_profile_mutex_unlock(cpthread_mutex_t *mutex,
                               lock_profile_site_t site);
void lock_profile_cond_wait(cpthread_cond_t *cond, cpthread_mutex_t *mutex,
                            lock_profile_site_t site);
void lock_profile_thread_join(cpthread_t thread, lock_profile_site_t site);
void lock_profile_report(void);

#else

static inline void lock_profile_mutex_lock(cpthread_mutex_t *mutex,
                                           lock_profile_site_t site) {
  (void)site;
  cpthread_mutex_lock(mutex);
}

static inline void lock_profile_mutex_unlock(cpthread_mutex_t *mutex,
                                             lock_profile_site_t site) {
  (void)site;
  cpthread_mutex_unlock(mutex);
}

static inline void lock_profile_cond_wait(cpthread_cond_t *cond,
                                          cpthread_mutex_t *mutex,
                                          lock_profile_site_t site) {
  (void)site;
  cpthread_cond_wait(cond, mutex);
}

static inline void lock_profile_thread_join(cpthread_t thread,
                                            lock_profile_site_t site) {
  (void)site;
  cpthread_join(thread);
}

static inline void lock_profile_report(void) {}

#endif

#endif

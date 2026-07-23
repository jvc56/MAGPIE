#include "lock_profile.h"

#include "io_util.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef MAGPIE_LOCK_PROFILE

enum {
  LOCK_PROFILE_HOLD_SAMPLE_PERIOD = 64,
  LOCK_PROFILE_MAX_THREADS = 4096,
};

typedef struct LockProfileSiteData {
  uint64_t acquisitions;
  uint64_t contended;
  uint64_t blocking_wait_ns;
  uint64_t condition_waits;
  uint64_t condition_wait_ns;
  uint64_t hold_samples;
  uint64_t sampled_hold_ns;
  uint64_t hold_start_ns;
  bool sampling_hold;
} LockProfileSiteData;

typedef struct LockProfileThreadData {
  LockProfileSiteData sites[NUM_LOCK_PROFILE_SITES];
} LockProfileThreadData;

static pthread_once_t lock_profile_once = PTHREAD_ONCE_INIT;
static pthread_key_t lock_profile_key;
static pthread_mutex_t lock_profile_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static LockProfileThreadData *lock_profile_threads[LOCK_PROFILE_MAX_THREADS];
static int lock_profile_num_threads;

static const char *const lock_profile_site_names[NUM_LOCK_PROFILE_SITES] = {
    "bai_status",         "bai_next_arm",           "bai_update",
    "sim_seed",           "sim_ply_stats",          "sim_equity_stats",
    "sim_win_pct_stats",  "sim_utility_stats",      "display",
    "initial_checkpoint", "avoid_prune_checkpoint", "thread_control",
    "bai_worker_join",    "autoplay_worker_join",   "inference_worker_join",
    "other_checkpoint",
};

static uint64_t lock_profile_now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    log_fatal("lock profile clock_gettime failed");
  }
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void lock_profile_init(void) {
  if (pthread_key_create(&lock_profile_key, NULL)) {
    log_fatal("lock profile pthread key creation failed");
  }
}

static LockProfileThreadData *lock_profile_get_thread_data(void) {
  if (pthread_once(&lock_profile_once, lock_profile_init)) {
    log_fatal("lock profile pthread_once failed");
  }
  LockProfileThreadData *thread_data = pthread_getspecific(lock_profile_key);
  if (thread_data) {
    return thread_data;
  }
  thread_data = calloc_or_die(1, sizeof(LockProfileThreadData));
  if (pthread_setspecific(lock_profile_key, thread_data)) {
    log_fatal("lock profile pthread_setspecific failed");
  }
  if (pthread_mutex_lock(&lock_profile_registry_mutex)) {
    log_fatal("lock profile registry lock failed");
  }
  if (lock_profile_num_threads >= LOCK_PROFILE_MAX_THREADS) {
    log_fatal("lock profile exceeded %d registered threads",
              LOCK_PROFILE_MAX_THREADS);
  }
  lock_profile_threads[lock_profile_num_threads++] = thread_data;
  if (pthread_mutex_unlock(&lock_profile_registry_mutex)) {
    log_fatal("lock profile registry unlock failed");
  }
  return thread_data;
}

void lock_profile_mutex_lock(cpthread_mutex_t *mutex,
                             lock_profile_site_t site) {
  LockProfileSiteData *site_data = &lock_profile_get_thread_data()->sites[site];
  site_data->acquisitions++;
  const int try_result = pthread_mutex_trylock(mutex);
  if (try_result == EBUSY) {
    site_data->contended++;
    const uint64_t wait_start_ns = lock_profile_now_ns();
    if (pthread_mutex_lock(mutex)) {
      log_fatal("profiled mutex lock failed");
    }
    site_data->blocking_wait_ns += lock_profile_now_ns() - wait_start_ns;
  } else if (try_result != 0) {
    log_fatal("profiled mutex trylock failed");
  }
  site_data->sampling_hold =
      site_data->acquisitions == 1 ||
      site_data->acquisitions % LOCK_PROFILE_HOLD_SAMPLE_PERIOD == 0;
  if (site_data->sampling_hold) {
    site_data->hold_samples++;
    site_data->hold_start_ns = lock_profile_now_ns();
  }
}

void lock_profile_mutex_unlock(cpthread_mutex_t *mutex,
                               lock_profile_site_t site) {
  LockProfileSiteData *site_data = &lock_profile_get_thread_data()->sites[site];
  if (site_data->sampling_hold) {
    site_data->sampled_hold_ns +=
        lock_profile_now_ns() - site_data->hold_start_ns;
    site_data->sampling_hold = false;
  }
  if (pthread_mutex_unlock(mutex)) {
    log_fatal("profiled mutex unlock failed");
  }
}

void lock_profile_cond_wait(cpthread_cond_t *cond, cpthread_mutex_t *mutex,
                            lock_profile_site_t site) {
  LockProfileSiteData *site_data = &lock_profile_get_thread_data()->sites[site];
  if (site_data->sampling_hold) {
    site_data->sampled_hold_ns +=
        lock_profile_now_ns() - site_data->hold_start_ns;
  }
  site_data->condition_waits++;
  const uint64_t wait_start_ns = lock_profile_now_ns();
  if (pthread_cond_wait(cond, mutex)) {
    log_fatal("profiled condition wait failed");
  }
  site_data->condition_wait_ns += lock_profile_now_ns() - wait_start_ns;
  if (site_data->sampling_hold) {
    site_data->hold_start_ns = lock_profile_now_ns();
  }
}

void lock_profile_thread_join(cpthread_t thread, lock_profile_site_t site) {
  LockProfileSiteData *site_data = &lock_profile_get_thread_data()->sites[site];
  site_data->condition_waits++;
  const uint64_t wait_start_ns = lock_profile_now_ns();
  cpthread_join(thread);
  site_data->condition_wait_ns += lock_profile_now_ns() - wait_start_ns;
}

void lock_profile_report(void) {
  LockProfileSiteData totals[NUM_LOCK_PROFILE_SITES] = {0};
  if (pthread_mutex_lock(&lock_profile_registry_mutex)) {
    log_fatal("lock profile report lock failed");
  }
  for (int thread_idx = 0; thread_idx < lock_profile_num_threads;
       thread_idx++) {
    const LockProfileThreadData *thread_data = lock_profile_threads[thread_idx];
    for (int site = 0; site < NUM_LOCK_PROFILE_SITES; site++) {
      totals[site].acquisitions += thread_data->sites[site].acquisitions;
      totals[site].contended += thread_data->sites[site].contended;
      totals[site].blocking_wait_ns +=
          thread_data->sites[site].blocking_wait_ns;
      totals[site].condition_waits += thread_data->sites[site].condition_waits;
      totals[site].condition_wait_ns +=
          thread_data->sites[site].condition_wait_ns;
      totals[site].hold_samples += thread_data->sites[site].hold_samples;
      totals[site].sampled_hold_ns += thread_data->sites[site].sampled_hold_ns;
    }
  }
  if (pthread_mutex_unlock(&lock_profile_registry_mutex)) {
    log_fatal("lock profile report unlock failed");
  }

  uint64_t all_wait_ns = 0;
  for (int site = 0; site < NUM_LOCK_PROFILE_SITES; site++) {
    all_wait_ns +=
        totals[site].blocking_wait_ns + totals[site].condition_wait_ns;
  }
  printf("LOCK_PROFILE hold_sample_period=%d registered_threads=%d\n",
         LOCK_PROFILE_HOLD_SAMPLE_PERIOD, lock_profile_num_threads);
  for (int site = 0; site < NUM_LOCK_PROFILE_SITES; site++) {
    const LockProfileSiteData *site_data = &totals[site];
    const uint64_t total_wait_ns =
        site_data->blocking_wait_ns + site_data->condition_wait_ns;
    const uint64_t wait_events =
        site_data->contended + site_data->condition_waits;
    const double mean_wait_ns =
        wait_events ? (double)total_wait_ns / (double)wait_events : 0.0;
    const double mean_hold_ns = site_data->hold_samples
                                    ? (double)site_data->sampled_hold_ns /
                                          (double)site_data->hold_samples
                                    : 0.0;
    const double estimated_hold_ns =
        mean_hold_ns * (double)site_data->acquisitions;
    const double contended_pct = site_data->acquisitions
                                     ? 100.0 * (double)site_data->contended /
                                           (double)site_data->acquisitions
                                     : 0.0;
    const double wait_share_pct =
        all_wait_ns ? 100.0 * (double)total_wait_ns / (double)all_wait_ns : 0.0;
    printf("LOCK_PROFILE site=%s acquisitions=%llu contended=%llu "
           "contended_pct=%.6f blocking_wait_ns=%llu condition_waits=%llu "
           "condition_wait_ns=%llu total_wait_ns=%llu mean_wait_ns=%.3f "
           "hold_samples=%llu sampled_hold_ns=%llu mean_hold_ns=%.3f "
           "estimated_hold_ns=%.0f wait_share_pct=%.6f\n",
           lock_profile_site_names[site],
           (unsigned long long)site_data->acquisitions,
           (unsigned long long)site_data->contended, contended_pct,
           (unsigned long long)site_data->blocking_wait_ns,
           (unsigned long long)site_data->condition_waits,
           (unsigned long long)site_data->condition_wait_ns,
           (unsigned long long)total_wait_ns, mean_wait_ns,
           (unsigned long long)site_data->hold_samples,
           (unsigned long long)site_data->sampled_hold_ns, mean_hold_ns,
           estimated_hold_ns, wait_share_pct);
  }
}

#endif

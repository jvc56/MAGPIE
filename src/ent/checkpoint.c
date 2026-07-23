#include "checkpoint.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include "../util/lock_profile.h"
#include <stdlib.h>

struct Checkpoint {
  cpthread_mutex_t mutex;
  cpthread_cond_t cond;
  prebroadcast_func_t prebroadcast_func;
  int count;
  int num_threads;
#ifdef MAGPIE_LOCK_PROFILE
  lock_profile_site_t lock_profile_site;
#endif
};

Checkpoint *checkpoint_create(int num_threads,
                              prebroadcast_func_t prebroadcast_func) {
  Checkpoint *checkpoint = malloc_or_die(sizeof(Checkpoint));
  checkpoint->count = 0;
  checkpoint->num_threads = num_threads;
  checkpoint->prebroadcast_func = prebroadcast_func;
#ifdef MAGPIE_LOCK_PROFILE
  checkpoint->lock_profile_site = LOCK_PROFILE_SITE_OTHER_CHECKPOINT;
#endif
  cpthread_mutex_init(&checkpoint->mutex);
  cpthread_cond_init(&checkpoint->cond);
  return checkpoint;
}

void checkpoint_destroy(Checkpoint *checkpoint) { free(checkpoint); }

#ifdef MAGPIE_LOCK_PROFILE
void checkpoint_set_lock_profile_site(Checkpoint *checkpoint,
                                      lock_profile_site_t site) {
  checkpoint->lock_profile_site = site;
}
#endif

void checkpoint_wait(Checkpoint *checkpoint, void *data) {
#ifdef MAGPIE_LOCK_PROFILE
  const lock_profile_site_t site = checkpoint->lock_profile_site;
#else
  const lock_profile_site_t site = LOCK_PROFILE_SITE_OTHER_CHECKPOINT;
#endif
  lock_profile_mutex_lock(&checkpoint->mutex, site);
  checkpoint->count++;

  if (checkpoint->count == checkpoint->num_threads) {
    checkpoint->prebroadcast_func(data);
    checkpoint->count = 0;
    cpthread_cond_broadcast(&checkpoint->cond);
  } else {
    lock_profile_cond_wait(&checkpoint->cond, &checkpoint->mutex, site);
  }
  lock_profile_mutex_unlock(&checkpoint->mutex, site);
}

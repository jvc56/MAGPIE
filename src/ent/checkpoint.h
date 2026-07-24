#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "../util/lock_profile.h"

typedef struct Checkpoint Checkpoint;

typedef void (*prebroadcast_func_t)(void *);

Checkpoint *checkpoint_create(int num_threads,
                              prebroadcast_func_t prebroadcast_func);
void checkpoint_destroy(Checkpoint *checkpoint);
#ifdef MAGPIE_LOCK_PROFILE
void checkpoint_set_lock_profile_site(Checkpoint *checkpoint,
                                      lock_profile_site_t site);
#else
static inline void checkpoint_set_lock_profile_site(Checkpoint *checkpoint,
                                                    lock_profile_site_t site) {
  (void)checkpoint;
  (void)site;
}
#endif
void checkpoint_wait(Checkpoint *checkpoint, void *data);

#endif

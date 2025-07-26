#include "checkpoint.h"

#include "../compat/cpthread.h"
#include "../util/io_util.h"
#include <stdlib.h>

struct Checkpoint {
  cpthread_mutex_t mutex;
  cpthread_cond_t cond;
  prebroadcast_func_t prebroadcast_func;
  int count;
  int num_threads;
};

Checkpoint *checkpoint_create(int num_threads,
                              prebroadcast_func_t prebroadcast_func) {
  Checkpoint *checkpoint = malloc_or_die(sizeof(Checkpoint));
  checkpoint->count = 0;
  checkpoint->num_threads = num_threads;
  checkpoint->prebroadcast_func = prebroadcast_func;
  cpthread_mutex_init(&checkpoint->mutex);
  cpthread_cond_init(&checkpoint->cond);
  return checkpoint;
}

void checkpoint_destroy(Checkpoint *checkpoint) { free(checkpoint); }

void checkpoint_wait(Checkpoint *checkpoint, void *data) {
  cpthread_mutex_lock(&checkpoint->mutex);
  checkpoint->count++;

  if (checkpoint->count == checkpoint->num_threads) {
    checkpoint->prebroadcast_func(data);
    checkpoint->count = 0;
    cpthread_cond_broadcast(&checkpoint->cond);
  } else {
    cpthread_cond_wait(&checkpoint->cond, &checkpoint->mutex);
  }
  cpthread_mutex_unlock(&checkpoint->mutex);
}
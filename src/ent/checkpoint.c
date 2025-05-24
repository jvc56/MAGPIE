#include "checkpoint.h"

#include <pthread.h>

#include "../util/io_util.h"

struct Checkpoint {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
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
  pthread_mutex_init(&checkpoint->mutex, NULL);
  pthread_cond_init(&checkpoint->cond, NULL);
  return checkpoint;
}

void checkpoint_destroy(Checkpoint *checkpoint) { free(checkpoint); }

void checkpoint_wait(Checkpoint *checkpoint, void *data) {
  pthread_mutex_lock(&checkpoint->mutex);
  checkpoint->count++;

  if (checkpoint->count == checkpoint->num_threads) {
    checkpoint->prebroadcast_func(data);
    checkpoint->count = 0;
    pthread_cond_broadcast(&checkpoint->cond);
  } else {
    pthread_cond_wait(&checkpoint->cond, &checkpoint->mutex);
  }
  pthread_mutex_unlock(&checkpoint->mutex);
}
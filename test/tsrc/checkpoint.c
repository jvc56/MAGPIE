#include <assert.h>
#include <pthread.h>

#include "../../src/ent/checkpoint.h"

#define NUM_THREADS 4

typedef struct {
  Checkpoint *checkpoint;
  int *shared_data;
} thread_data_t;

void *thread_func(void *arg) {
  thread_data_t *data = (thread_data_t *)arg;
  checkpoint_wait(data->checkpoint, data->shared_data);
  return NULL;
}

void prebroadcast_func(void *data) {
  int *shared_data = (int *)data;
  *shared_data += 1;
}

void test_checkpoint(void) {
  pthread_t threads[NUM_THREADS];
  Checkpoint *checkpoint;
  thread_data_t thread_data[NUM_THREADS];
  int shared_data = 0;
  int i;

  checkpoint = checkpoint_create(NUM_THREADS, prebroadcast_func);

  for (i = 0; i < NUM_THREADS; i++) {
    thread_data[i].checkpoint = checkpoint;
    thread_data[i].shared_data = &shared_data;
    pthread_create(&threads[i], NULL, thread_func, &thread_data[i]);
  }

  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  checkpoint_destroy(checkpoint);

  // Ensure prebroadcast_func was called once
  assert(shared_data == 1);
}
#include "prod_con_queue.h"

#include <pthread.h>

#include "../util/log.h"
#include "../util/util.h"

struct ProdConQueue {
  void **queue;
  int newest;
  int oldest;
  int count;
  int size;
  pthread_mutex_t mutex;
  pthread_cond_t empty;
};

ProdConQueue *prod_con_queue_create(int size) {
  ProdConQueue *pcq = malloc_or_die(sizeof(ProdConQueue));
  pcq->newest = 0;
  pcq->oldest = 0;
  pcq->count = 0;
  pcq->size = size;
  pcq->queue = malloc_or_die(sizeof(void *) * pcq->size);
  for (int i = 0; i < size; i++) {
    pcq->queue[i] = NULL;
  }
  pthread_mutex_init(&pcq->mutex, NULL);
  pthread_cond_init(&pcq->empty, NULL);
  return pcq;
}

void prod_con_queue_destroy(ProdConQueue *pcq) {
  if (!pcq) {
    return;
  }
  free(pcq->queue);
  free(pcq);
}

// Exits fatally if the queue is full. The caller is expected
// to prevent the queue from being full.
void prod_con_queue_produce(ProdConQueue *pcq, void *item) {
  pthread_mutex_lock(&pcq->mutex);
  if (pcq->count == pcq->size) {
    log_fatal("queue is unexpectedly full with %d items\n", pcq->count);
  }
  pcq->queue[pcq->newest] = item;
  pcq->newest = (pcq->newest + 1) % pcq->size;
  pcq->count++;
  pthread_mutex_unlock(&pcq->mutex);
  pthread_cond_signal(&pcq->empty);
}

void *prod_con_queue_consume(ProdConQueue *pcq) {
  pthread_mutex_lock(&pcq->mutex);
  while (pcq->count == 0) {
    pthread_cond_wait(&pcq->empty, &pcq->mutex);
  }
  void *item = pcq->queue[pcq->oldest];
  pcq->queue[pcq->oldest] = NULL;
  pcq->oldest = (pcq->oldest + 1) % pcq->size;
  pcq->count--;
  pthread_mutex_unlock(&pcq->mutex);
  return item;
}
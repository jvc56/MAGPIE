#ifndef MOVE_GEN_CACHE_H
#define MOVE_GEN_CACHE_H

#include "../util/io_util.h"
#include "move_gen.h"
#include <string.h>

enum {
  MOVE_GEN_CACHE_SIZE = 512,
};

typedef struct MoveGenCache {
  MoveGen *movegens[MOVE_GEN_CACHE_SIZE];
} MoveGenCache;

static inline void move_gen_cache_init(MoveGenCache *cache) {
  memset(cache, 0, sizeof(MoveGenCache));
}

static inline void move_gen_cache_alloc(MoveGenCache *cache, int size) {
  if (size > MOVE_GEN_CACHE_SIZE) {
    log_fatal("Requested move gen cache size %d exceeds maximum of %d", size,
              MOVE_GEN_CACHE_SIZE);
  }
  for (int i = 0; i < size; i++) {
    if (!cache->movegens[i]) {
      cache->movegens[i] = malloc_or_die(sizeof(MoveGen));
    }
  }
}

static inline void move_gen_cache_destroy(MoveGenCache *cache) {
  for (int i = 0; i < MOVE_GEN_CACHE_SIZE; i++) {
    free(cache->movegens[i]);
    cache->movegens[i] = NULL;
  }
}

static inline MoveGen *move_gen_cache_get(MoveGenCache *cache, int index) {
  return cache->movegens[index];
}

#endif

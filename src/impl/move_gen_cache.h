#ifndef MOVE_GEN_CACHE_H
#define MOVE_GEN_CACHE_H

#include "../util/io_util.h"
#include "move_gen.h"
#include <string.h>

typedef struct MoveGenCache {
  MoveGen *movegens;
  int size;
} MoveGenCache;

static inline void move_gen_cache_init(MoveGenCache *cache) {
  memset(cache, 0, sizeof(MoveGenCache));
}

static inline void move_gen_cache_alloc(MoveGenCache *cache, int size) {
  if (size <= cache->size) {
    return;
  }
  if (!cache->movegens) {
    cache->movegens = malloc_or_die(size * sizeof(MoveGen));
  } else {
    cache->movegens = realloc_or_die(cache->movegens, size * sizeof(MoveGen));
  }
  cache->size = size;
}

static inline void move_gen_cache_destroy(MoveGenCache *cache) {
  free(cache->movegens);
  cache->movegens = NULL;
  cache->size = 0;
}

static inline MoveGen *move_gen_cache_get(MoveGenCache *cache, int index) {
  return &cache->movegens[index];
}

#endif

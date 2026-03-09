#ifndef MOVE_GEN_CACHE_H
#define MOVE_GEN_CACHE_H

#include "../util/io_util.h"
#include "move_gen.h"
#include <stdint.h>
#include <string.h>

enum {
  MOVEGEN_CACHE_ALIGNMENT = 64,
  MOVEGEN_CACHE_STRIDE = ((int)sizeof(MoveGen) + MOVEGEN_CACHE_ALIGNMENT - 1)
                         / MOVEGEN_CACHE_ALIGNMENT * MOVEGEN_CACHE_ALIGNMENT,
};

typedef struct MoveGenCache {
  uint8_t *movegens;
  int size;
} MoveGenCache;

static inline void move_gen_cache_init(MoveGenCache *cache) {
  memset(cache, 0, sizeof(MoveGenCache));
}

static inline void move_gen_cache_alloc(MoveGenCache *cache, int size) {
  if (size <= cache->size) {
    return;
  }
  uint8_t *new_movegens = portable_aligned_alloc_or_die(
      MOVEGEN_CACHE_ALIGNMENT, (size_t)size * MOVEGEN_CACHE_STRIDE);
  if (cache->movegens) {
    for (int i = 0; i < cache->size; i++) {
      memcpy(new_movegens + (size_t)i * MOVEGEN_CACHE_STRIDE,
             cache->movegens + (size_t)i * MOVEGEN_CACHE_STRIDE,
             sizeof(MoveGen));
    }
    portable_aligned_free_or_die(cache->movegens);
  }
  cache->movegens = new_movegens;
  cache->size = size;
}

static inline void move_gen_cache_destroy(MoveGenCache *cache) {
  portable_aligned_free_or_die(cache->movegens);
  cache->movegens = NULL;
  cache->size = 0;
}

static inline MoveGen *move_gen_cache_get(MoveGenCache *cache, int index) {
  return (MoveGen *)(cache->movegens + (size_t)index * MOVEGEN_CACHE_STRIDE);
}

#endif

#ifndef CROSS_SET_CACHE_H
#define CROSS_SET_CACHE_H

#include "../compat/malloc.h"
#include "../util/io_util.h"
#include "equity.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A memory of classic cross sets by the words beside the empty square. An
// empty square's cross set, its cross score and the extension sets at the
// ends of the words beside it depend only on those words' letters and the
// lexicon, and an endgame's search computes the same few of them again and
// again: about 99.8% of the cross sets it reads the lexicon for are of words
// it has already seen (see game_gen_classic_cross_set in game.c).
//
// The cache is direct-mapped by the key's top bits, and an entry, one
// aligned 64-byte block, holds the whole 64-bit key. Its owner salts the
// keys whenever the lexicon may change, so that an entry from another
// lexicon never matches.

enum {
  CROSS_SET_CACHE_ENTRY_BYTES = 64,
};

// What game_gen_classic_cross_set writes for an empty square with letters
// on at least one side: the square's cross set and cross score, and the
// extension sets of the words on its left (leftside_*) and right
// (rightside_*).
typedef struct CrossSetValues {
  uint64_t cross_set;
  uint64_t leftside_leftx_set;
  uint64_t leftside_rightx_set;
  uint64_t rightside_leftx_set;
  uint64_t rightside_rightx_set;
  Equity score;
} CrossSetValues;

typedef struct CrossSetCacheEntry {
  uint64_t key;
  CrossSetValues values;
} CrossSetCacheEntry;

static_assert(sizeof(CrossSetCacheEntry) <= CROSS_SET_CACHE_ENTRY_BYTES,
              "a cross set cache entry fits one 64-byte block");

typedef struct CrossSetCache {
  // 2^index_bits entries of CROSS_SET_CACHE_ENTRY_BYTES each, indexed by the
  // key's top index_bits bits.
  unsigned char *entries;
  int index_bits;
  uint64_t salt;
} CrossSetCache;

static inline CrossSetCache *cross_set_cache_create(int index_bits) {
  CrossSetCache *cache = malloc_or_die(sizeof(CrossSetCache));
  const size_t entries_size =
      ((size_t)1 << index_bits) * CROSS_SET_CACHE_ENTRY_BYTES;
  void *entries = NULL;
  if (portable_aligned_alloc(&entries, CROSS_SET_CACHE_ENTRY_BYTES,
                             entries_size) != 0) {
    log_fatal("could not allocate the cross set cache");
  }
  // A zero key is never a cross set's.
  memset(entries, 0, entries_size);
  cache->entries = (unsigned char *)entries;
  cache->index_bits = index_bits;
  cache->salt = 0;
  return cache;
}

static inline void cross_set_cache_destroy(CrossSetCache *cache) {
  if (cache == NULL) {
    return;
  }
  portable_aligned_free(cache->entries);
  free(cache);
}

// Makes every entry recorded before unreachable.
static inline void cross_set_cache_set_salt(CrossSetCache *cache,
                                            uint64_t salt) {
  cache->salt = salt;
}

static inline CrossSetCacheEntry *cross_set_cache_entry(CrossSetCache *cache,
                                                        uint64_t key) {
  return (CrossSetCacheEntry *)(cache->entries +
                                (key >> (64 - cache->index_bits)) *
                                    CROSS_SET_CACHE_ENTRY_BYTES);
}

#endif

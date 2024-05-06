

#ifndef KWG_CACHE_H
#define KWG_CACHE_H

#include "../def/cross_set_defs.h"

#include "kwg.h"
#include "rack.h"

// FIXME: Figure out a way to determine the best
// constants for a given device.
#define KWG_CACHE_BUCKET_SIZE 3
#define KWG_CACHE_BUCKETS 10000
#define KWG_CACHE_SIZE (KWG_CACHE_BUCKETS * KWG_CACHE_BUCKET_SIZE)

typedef struct KWGCacheEntry {
  uint32_t node_index;
  uint32_t node;
} KWGCacheEntry;

typedef struct KWGCache {
  KWG *kwg;
  KWGCacheEntry map[KWG_CACHE_SIZE];
} KWGCache;

static inline uint32_t kwgc_hash(uint32_t x) {
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = ((x >> 16) ^ x) * 0x45d9f3b;
  x = (x >> 16) ^ x;
  return x % KWG_CACHE_BUCKETS;
}

static inline uint32_t kwgc_node(KWGCache *kwgc, uint32_t node_index) {
  // We use a node_index of 0 to designate an empty cache bucket, so
  // a node index of 0 should never be an actual KWG value in the map.
  if (node_index == 0) {
    return kwg_node(kwgc->kwg, node_index);
  }
  const uint32_t kwgc_index = kwgc_hash(node_index);
  int bucket_index = KWG_CACHE_BUCKET_SIZE - 1;
  uint32_t node = 0;
  for (int i = 0; i < KWG_CACHE_BUCKET_SIZE; i++) {
    if (kwgc->map[kwgc_index + i].node_index == node_index) {
      bucket_index = i;
      node = kwgc->map[kwgc_index + i].node;
      break;
    } else if (kwgc->map[kwgc_index + i].node == 0) {
      bucket_index = i;
      break;
    }
  }

  // Node didn't exist in the cache, so we
  // have to get it from the KWG
  if (node == 0) {
    node = kwg_node(kwgc->kwg, node_index);
  }
  // Move this bucket to the "front" so that the
  // "backmost" bucket is discarded on the next collision,
  // implementing an LRU policy.
  for (int i = bucket_index; i > 0; i--) {
    kwgc->map[kwgc_index + i] = kwgc->map[kwgc_index + i - 1];
  }
  kwgc->map[kwgc_index].node = node;
  kwgc->map[kwgc_index].node_index = node_index;
  return node;
}

// Pass through for kwg_get_root_node_index
static inline uint32_t kwgc_get_root_node_index(const KWGCache *kwgc) {
  return kwg_get_root_node_index(kwgc->kwg);
}

static inline KWG *kwgc_get_kwg(KWGCache *kwgc) { return kwgc->kwg; }

static inline uint32_t
kwgc_get_next_node_index(KWGCache *kwgc, uint32_t node_index, uint8_t letter) {
  uint32_t i = node_index;
  while (1) {
    const uint32_t node = kwgc_node(kwgc, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_arc_index_prefetch(node, kwgc_get_kwg(kwgc));
    }
    if (kwg_node_is_end(node)) {
      return 0;
    }
    i++;
  }
}

static inline void kwgc_clear(KWGCache *kwgc) {
  memset(kwgc->map, 0, sizeof(kwgc->map));
}

static inline void kwgc_init(KWGCache *kwgc, KWG *kwg) {
  kwgc->kwg = kwg;
  kwgc_clear(kwgc);
}

#endif
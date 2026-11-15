#ifndef LANE_SHADOW_CACHE_H
#define LANE_SHADOW_CACHE_H

#include "../compat/malloc.h"
#include "../def/board_defs.h"
#include "../util/io_util.h"
#include "equity.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// An endgame worker's memory of MOVE_RECORD_BEST_SMALL lane shadows. A lane's
// shadow is the anchors it adds to the heap, with their bounds, and depends
// only on the lane's squares and direction, the rack, the out bonus and the
// lexicon. Across
// an endgame's playouts, the same lanes recur with the same racks: the boards
// differ in a few lanes, and each side's racks are subsets of one rack. So a
// lane whose key the cache holds adds the anchors recorded under it instead
// of being shadowed again (see shadow_lanes_small_cached in move_gen.c).
//
// The cache is direct-mapped by the key's top bits, and an entry holds the
// whole 64-bit key. The keys are salted per solve, so that entries from an
// earlier solve, perhaps in another lexicon, never match.
//
// An entry is one aligned 64-byte block, so a lookup touches one cache line,
// and holds up to LANE_SHADOW_MAX_ANCHORS anchors. In endgames about 99.95%
// of the lanes shadowed have that few; a lane with more is never recorded,
// and is shadowed every time.

enum {
  LANE_SHADOW_MAX_ANCHORS = 9,
  // A hit writes this many anchors without a loop: 94% to 98% of the lanes
  // shadowed in endgames have at most 4.
  LANE_SHADOW_UNCONDITIONAL_ANCHORS = 4,
  LANE_SHADOW_ENTRY_BYTES = 64,
};

typedef struct LaneShadowEntry {
  uint64_t key;
  uint8_t count;
  uint8_t cols[LANE_SHADOW_MAX_ANCHORS];
  uint8_t last_anchor_cols[LANE_SHADOW_MAX_ANCHORS];
  Equity bounds[LANE_SHADOW_MAX_ANCHORS];
} LaneShadowEntry;

static_assert(sizeof(LaneShadowEntry) == LANE_SHADOW_ENTRY_BYTES,
              "a lane shadow entry fills one 64-byte block");

// The best play of one anchor of a lane, as the small generator finds it:
// the generator searches an anchor for every play whatever the best so far
// is, so an anchor's best play depends only on what its lane's key holds, on
// which lane it is (a play names its row), on the anchor's column and on how
// ties are broken. The lanes that recur across an endgame's playouts recur
// with their anchors, so a generation takes the play remembered for an anchor
// instead of searching it again (see gen_record_scoring_plays_impl in
// move_gen.c). Direct-mapped by the key's top bits, like the lanes' entries;
// a key is never 0.
typedef struct AnchorPlayEntry {
  uint64_t key;
  // 0 when the anchor has no play.
  uint64_t tiny_move;
  // The play's score, with the out bonus if it empties the rack.
  Equity value;
  uint16_t score;
  uint8_t play_length;
  uint8_t tiles_played;
  uint64_t unused;
} AnchorPlayEntry;

enum {
  ANCHOR_PLAY_ENTRY_BYTES = 32,
};

static_assert(sizeof(AnchorPlayEntry) == ANCHOR_PLAY_ENTRY_BYTES,
              "two anchor play entries fill one 64-byte block");

typedef struct LaneShadowCache {
  LaneShadowEntry *entries;
  // The entries are 2^index_bits, indexed by the key's top index_bits bits.
  int index_bits;
  uint64_t salt;
  // The anchors' plays are 2^anchor_index_bits, indexed the same way.
  AnchorPlayEntry *anchor_plays;
  int anchor_index_bits;
} LaneShadowCache;

static inline LaneShadowCache *lane_shadow_cache_create(int index_bits,
                                                        int anchor_index_bits) {
  LaneShadowCache *cache = malloc_or_die(sizeof(LaneShadowCache));
  const size_t plays_size =
      ((size_t)1 << anchor_index_bits) * sizeof(AnchorPlayEntry);
  void *plays = NULL;
  if (portable_aligned_alloc(&plays, LANE_SHADOW_ENTRY_BYTES, plays_size) !=
      0) {
    log_fatal("could not allocate the anchor play cache");
  }
  // A zero key is never an anchor's.
  memset(plays, 0, plays_size);
  cache->anchor_plays = (AnchorPlayEntry *)plays;
  cache->anchor_index_bits = anchor_index_bits;
  const size_t entries_size =
      ((size_t)1 << index_bits) * sizeof(LaneShadowEntry);
  void *entries = NULL;
  if (portable_aligned_alloc(&entries, LANE_SHADOW_ENTRY_BYTES, entries_size) !=
      0) {
    log_fatal("could not allocate the lane shadow cache");
  }
  // A zero key is never a lane's.
  memset(entries, 0, entries_size);
  cache->entries = (LaneShadowEntry *)entries;
  cache->index_bits = index_bits;
  cache->salt = 0;
  return cache;
}

static inline void lane_shadow_cache_destroy(LaneShadowCache *cache) {
  if (cache == NULL) {
    return;
  }
  portable_aligned_free(cache->entries);
  portable_aligned_free(cache->anchor_plays);
  free(cache);
}

// Makes every entry recorded before unreachable.
static inline void lane_shadow_cache_set_salt(LaneShadowCache *cache,
                                              uint64_t salt) {
  cache->salt = salt;
}

static inline LaneShadowEntry *lane_shadow_cache_entry(LaneShadowCache *cache,
                                                       uint64_t key) {
  return &cache->entries[key >> (64 - cache->index_bits)];
}

static inline AnchorPlayEntry *
lane_shadow_cache_anchor_play(LaneShadowCache *cache, uint64_t key) {
  return &cache->anchor_plays[key >> (64 - cache->anchor_index_bits)];
}

#endif

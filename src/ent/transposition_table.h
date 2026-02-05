#ifndef TRANSPOSITION_TABLE_H
#define TRANSPOSITION_TABLE_H

#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "zobrist.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Platform-specific TT configuration
#ifdef __EMSCRIPTEN__
// WASM: 17-byte entries, 2^21 minimum (34.5 MB) for mobile
#define TT_MIN_SIZE_POWER 21
#define TTENTRY_SIZE_BYTES 17
#else
// Native: 16-byte entries, 2^24 minimum (256 MB) for performance
#define TT_MIN_SIZE_POWER 24
#define TTENTRY_SIZE_BYTES 16
#define BOTTOM3_BYTE_MASK ((1 << 24) - 1)
#endif

enum {
  TT_EXACT = 0x01,
  TT_LOWER = 0x02,
  TT_UPPER = 0x03,
  DEPTH_MASK = ((1 << 6) - 1),
};

typedef struct TTEntry {
  uint32_t top_4_bytes; // Bits [63:32] of hash
  int16_t score;
#ifdef __EMSCRIPTEN__
  uint16_t bytes_5_6; // Bits [31:16] (WASM: store 48 bits total)
#else
  uint8_t fifth_byte; // Bits [31:24] (Native: store 40 bits total)
#endif
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntry;

inline static uint64_t ttentry_full_hash(TTEntry t, uint64_t index,
                                         int size_power) {
#ifdef __EMSCRIPTEN__
  // WASM: Store 48 bits, take remaining bits from index
  uint64_t stored_hash = ((uint64_t)(t.top_4_bytes) << 16) | t.bytes_5_6;
  return (stored_hash << size_power) | index;
#else
  // Native: Store 40 bits, take bottom 24 bits from index
  (void)size_power; // Unused in native build
  return ((uint64_t)(t.top_4_bytes) << 32) + ((uint64_t)(t.fifth_byte) << 24) +
         (index & BOTTOM3_BYTE_MASK);
#endif
}

inline static uint8_t ttentry_flag(TTEntry t) { return t.flag_and_depth >> 6; }

inline static uint8_t ttentry_depth(TTEntry t) {
  return t.flag_and_depth & DEPTH_MASK;
}

inline static int16_t ttentry_score(TTEntry t) { return t.score; }

inline static bool ttentry_valid(TTEntry t) { return ttentry_flag(t) != 0; }

inline static uint64_t ttentry_move(TTEntry t) { return t.tiny_move; }

inline static void ttentry_reset(TTEntry *t) {
  t->top_4_bytes = 0;
  t->score = 0;
#ifdef __EMSCRIPTEN__
  t->bytes_5_6 = 0;
#else
  t->fifth_byte = 0;
#endif
  t->flag_and_depth = 0;
  t->tiny_move = 0;
}

typedef struct TranspositionTable {
  TTEntry *table;
  int size_power_of_2;
  uint64_t size_mask;
  Zobrist *zobrist;
  atomic_int created;
  atomic_int hits;
  atomic_int lookups;
  atomic_int t2_collisions;
} TranspositionTable;

static inline TranspositionTable *
transposition_table_create(double fraction_of_memory) {
  TranspositionTable *tt = malloc_or_die(sizeof(TranspositionTable));

  uint64_t total_memory = get_total_memory();
  uint64_t desired_n_elems =
      (uint64_t)(fraction_of_memory *
                 ((double)(total_memory) / (double)TTENTRY_SIZE_BYTES));
  // find biggest power of 2 lower than desired.
  int size_required = 0;
  while (desired_n_elems >>= 1) {
    size_required++;
  }

  tt->size_power_of_2 = size_required;

  // Platform-specific minimum table size
  if (tt->size_power_of_2 < TT_MIN_SIZE_POWER) {
    tt->size_power_of_2 = TT_MIN_SIZE_POWER;
    log_warn("TT size clamped to minimum: 2^%d elements (%d MB)",
             TT_MIN_SIZE_POWER,
             (1 << TT_MIN_SIZE_POWER) * TTENTRY_SIZE_BYTES / (1024 * 1024));
  }
  int num_elems = 1 << tt->size_power_of_2;
  size_t memory_mb = (sizeof(TTEntry) * num_elems) / (1024 * 1024);
  log_info("Creating transposition table. System memory: %llu, TT size: 2^%d "
           "(elements: %d, memory: %zu MB)",
           (unsigned long long)total_memory, tt->size_power_of_2, num_elems,
           memory_mb);
  tt->table = malloc_or_die(sizeof(TTEntry) * num_elems);
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);
  tt->size_mask = num_elems - 1;
  tt->zobrist = zobrist_create(12345); // Fixed seed for determinism
  atomic_init(&tt->created, 0);
  atomic_init(&tt->hits, 0);
  atomic_init(&tt->lookups, 0);
  atomic_init(&tt->t2_collisions, 0);
  return tt;
}

static inline void transposition_table_reset(TranspositionTable *tt) {
  // This function resets the transposition table. If you want to reallocate
  // space for it, destroy and recreate it with the new space.
  memset(tt->table, 0, sizeof(TTEntry) * (tt->size_mask + 1));
  atomic_store(&tt->created, 0);
  atomic_store(&tt->hits, 0);
  atomic_store(&tt->lookups, 0);
  atomic_store(&tt->t2_collisions, 0);
}

static inline TTEntry transposition_table_lookup(TranspositionTable *tt,
                                                 uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  TTEntry entry = tt->table[idx];
  atomic_fetch_add(&tt->lookups, 1);
  uint64_t full_hash = ttentry_full_hash(entry, idx, tt->size_power_of_2);
  if (full_hash != zval) {
    if (ttentry_valid(entry)) {
      // There is another unrelated node at this position. This is a
      // type 2 collision.
      atomic_fetch_add(&tt->t2_collisions, 1);
    }
    TTEntry e;
    ttentry_reset(&e);
    return e;
  }
  atomic_fetch_add(&tt->hits, 1);
  // Assume the same zobrist hash is the same position. If it's not, that's
  // a type 1 collision, which we can't do anything about. It should happen
  // extremely rarely.
  return entry;
}

static inline void transposition_table_store(TranspositionTable *tt,
                                             uint64_t zval, TTEntry tentry) {
  uint64_t idx = zval & tt->size_mask;
#ifdef __EMSCRIPTEN__
  // WASM: Store top 48 bits (shift right by size_power to remove index bits)
  uint64_t stored_hash = zval >> tt->size_power_of_2;
  tentry.top_4_bytes = (uint32_t)(stored_hash >> 16);
  tentry.bytes_5_6 = (uint16_t)(stored_hash & 0xFFFF);
#else
  // Native: Store top 40 bits (bits 63-24)
  tentry.top_4_bytes = (uint32_t)(zval >> 32);
  tentry.fifth_byte = (uint8_t)(zval >> 24);
#endif
  atomic_fetch_add(&tt->created, 1);
  tt->table[idx] = tentry;
}

static inline void transposition_table_destroy(TranspositionTable *tt) {
  if (!tt) {
    return;
  }
  zobrist_destroy(tt->zobrist);
  free(tt->table);
  free(tt);
}

#endif
#ifndef TRANSPOSITION_TABLE_H
#define TRANSPOSITION_TABLE_H

#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "zobrist.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

enum {
  TT_EXACT = 0x01,
  TT_LOWER = 0x02,
  TT_UPPER = 0x03,
  TTENTRY_SIZE_BYTES = 16,
  BOTTOM3_BYTE_MASK = ((1 << 24) - 1),
  DEPTH_MASK = ((1 << 6) - 1),
  // Cache line size for padding to prevent false sharing
  CACHE_LINE_SIZE = 64,
  // Default nproc size power: 2^12 = 4K entries (256KB with cache-line padding)
  // Benchmarks show this is optimal for parallel endgame search.
  NPROC_SIZE_POWER_DEFAULT = 11,
};

typedef struct TTEntry {
  // Don't store the full hash, but the top 5 bytes. The bottom 3 bytes
  // can be determined from the bucket in the array.
  uint32_t top_4_bytes;
  int16_t score;
  uint8_t fifth_byte;
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntry;

inline static uint64_t ttentry_full_hash(TTEntry t, uint64_t index) {
  return ((uint64_t)(t.top_4_bytes) << 32) + ((uint64_t)(t.fifth_byte) << 24) +
         (index & BOTTOM3_BYTE_MASK);
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
  t->fifth_byte = 0;
  t->flag_and_depth = 0;
  t->tiny_move = 0;
}

// ABDADA: cache-line aligned nproc entry to prevent false sharing.
// Each entry is padded to CACHE_LINE_SIZE bytes so that different threads
// accessing different entries don't share cache lines.
typedef struct NprocEntry {
  atomic_uchar count;
  char padding[CACHE_LINE_SIZE - sizeof(atomic_uchar)];
} NprocEntry;

typedef struct TranspositionTable {
  TTEntry *table;
  NprocEntry *nproc;  // ABDADA: cache-aligned table for tracking concurrent searches
  int size_power_of_2;
  uint64_t size_mask;
  int nproc_size;      // Number of nproc entries (power of 2)
  uint64_t nproc_mask; // Mask for nproc index (nproc_size - 1)
  Zobrist *zobrist;
  atomic_int created;
  atomic_int hits;
  atomic_int lookups;
  atomic_int t2_collisions;
  atomic_int replacements;  // For compatibility with bucket version
} TranspositionTable;

// Create transposition table.
// fraction_of_memory: fraction of system RAM for TT (e.g., 0.25 = 25%)
// nproc_size_power: power of 2 for nproc table size (e.g., 12 = 4K entries)
//                   Use NPROC_SIZE_POWER_DEFAULT (11) for best performance.
static inline TranspositionTable *
transposition_table_create(double fraction_of_memory, int nproc_size_power) {
  TranspositionTable *tt = (TranspositionTable *)malloc_or_die(sizeof(TranspositionTable));

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

  // Guarantee at least 2^24 elements in the table. Anything less and our
  // 5-byte full hash proxy won't work.
  if (tt->size_power_of_2 < 24) {
    tt->size_power_of_2 = 24;
    log_warn("New TT size: 2**24 bytes");
  }
  int num_elems = 1 << tt->size_power_of_2;
  log_warn("Creating transposition table. System memory: %lu, TT size: 2**%d "
           "(number of elements: %d, memory required: %dMB)",
           total_memory, tt->size_power_of_2, num_elems,
           (int)((sizeof(TTEntry) * num_elems) / (size_t)(1024 * 1024)));
  tt->table = (TTEntry *)malloc_or_die(sizeof(TTEntry) * num_elems);
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);

  // ABDADA: allocate cache-aligned nproc table for tracking concurrent searches.
  // Each entry is padded to a full cache line to prevent false sharing between
  // threads accessing different entries.
  // If nproc_size_power is 0, use the default (2^12 = 4K entries).
  if (nproc_size_power == 0) {
    nproc_size_power = NPROC_SIZE_POWER_DEFAULT;
  }
  tt->nproc_size = 1 << nproc_size_power;
  tt->nproc_mask = tt->nproc_size - 1;
  log_warn("ABDADA nproc table: 2**%d entries (%d KB)",
           nproc_size_power, (int)(sizeof(NprocEntry) * tt->nproc_size / 1024));
  tt->nproc = (NprocEntry *)malloc_or_die(sizeof(NprocEntry) * tt->nproc_size);
  for (int i = 0; i < tt->nproc_size; i++) {
    atomic_init(&tt->nproc[i].count, 0);
  }
  tt->size_mask = num_elems - 1;
  tt->zobrist = zobrist_create(12345); // Fixed seed for determinism
  atomic_init(&tt->created, 0);
  atomic_init(&tt->hits, 0);
  atomic_init(&tt->lookups, 0);
  atomic_init(&tt->t2_collisions, 0);
  atomic_init(&tt->replacements, 0);
  return tt;
}

static inline void transposition_table_reset(TranspositionTable *tt) {
  // This function resets the transposition table. If you want to reallocate
  // space for it, destroy and recreate it with the new space.
  uint64_t num_elems = tt->size_mask + 1;
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);
  // ABDADA: reset nproc counters
  for (int i = 0; i < tt->nproc_size; i++) {
    atomic_store_explicit(&tt->nproc[i].count, 0, memory_order_relaxed);
  }
  atomic_store(&tt->created, 0);
  atomic_store(&tt->hits, 0);
  atomic_store(&tt->lookups, 0);
  atomic_store(&tt->t2_collisions, 0);
  atomic_store(&tt->replacements, 0);
}

static inline void transposition_table_prefetch(TranspositionTable *tt,
                                                uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  __builtin_prefetch(&tt->table[idx], 0, 3);
}

static inline TTEntry transposition_table_lookup(TranspositionTable *tt,
                                                 uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  TTEntry entry = tt->table[idx];
  atomic_fetch_add(&tt->lookups, 1);
  uint64_t full_hash = ttentry_full_hash(entry, idx);
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
  tentry.top_4_bytes = (uint32_t)(zval >> 32);
  tentry.fifth_byte = (uint8_t)(zval >> 24);

  // Track replacements when overwriting valid entries
  if (ttentry_valid(tt->table[idx])) {
    uint64_t existing_hash = ttentry_full_hash(tt->table[idx], idx);
    if (existing_hash != zval) {
      atomic_fetch_add(&tt->replacements, 1);
    }
  }

  atomic_fetch_add(&tt->created, 1);
  tt->table[idx] = tentry;
}

static inline void transposition_table_destroy(TranspositionTable *tt) {
  if (!tt) {
    return;
  }
  zobrist_destroy(tt->zobrist);
  free(tt->table);
  free(tt->nproc);
  free(tt);
}

// ABDADA functions for tracking concurrent node searches
// Uses a smaller table (256K entries) with relaxed memory ordering for speed

// Get pointer to TT entry (for checking nproc without copying)
static inline TTEntry *transposition_table_get_entry(TranspositionTable *tt,
                                                     uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  return &tt->table[idx];
}

// Check if node is being searched by another processor
// Uses relaxed ordering since exact count doesn't matter, just > 0
static inline bool transposition_table_is_busy(TranspositionTable *tt,
                                               uint64_t zval) {
  uint64_t idx = zval & tt->nproc_mask;
  return atomic_load_explicit(&tt->nproc[idx].count, memory_order_relaxed) > 0;
}

// Enter node: increment nproc counter
// Uses relaxed ordering - we just need eventual visibility
static inline void transposition_table_enter_node(TranspositionTable *tt,
                                                  uint64_t zval) {
  uint64_t idx = zval & tt->nproc_mask;
  atomic_fetch_add_explicit(&tt->nproc[idx].count, 1, memory_order_relaxed);
}

// Leave node: decrement nproc counter
// Uses relaxed ordering - we just need eventual visibility
static inline void transposition_table_leave_node(TranspositionTable *tt,
                                                  uint64_t zval) {
  uint64_t idx = zval & tt->nproc_mask;
  atomic_fetch_sub_explicit(&tt->nproc[idx].count, 1, memory_order_relaxed);
}

#endif

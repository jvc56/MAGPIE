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
  // ABDADA nproc table: use a smaller table than TT to reduce memory and
  // improve cache locality. Collisions just cause extra deferrals.
  NPROC_SIZE_POWER = 18,  // 2^18 = 256K entries
  NPROC_SIZE = (1 << NPROC_SIZE_POWER),
  NPROC_MASK = (NPROC_SIZE - 1),
};

// TTEntry is stored as two atomic 64-bit values for lock-free concurrent access
// with torn read detection.
// atomic1: [top_4_bytes:32][score:16][fifth_byte:8][flag_and_depth:8]
// atomic2: [tiny_move:64]
typedef struct TTEntry {
  _Atomic uint64_t atomic1;
  _Atomic uint64_t atomic2;  // tiny_move
} TTEntry;

// Pack fields into atomic1 format
static inline uint64_t ttentry_pack_atomic1(uint32_t top_4_bytes, int16_t score,
                                             uint8_t fifth_byte,
                                             uint8_t flag_and_depth) {
  return ((uint64_t)top_4_bytes) | ((uint64_t)(uint16_t)score << 32) |
         ((uint64_t)fifth_byte << 48) | ((uint64_t)flag_and_depth << 56);
}

// Unpack fields from atomic1 format
static inline void ttentry_unpack_atomic1(uint64_t atomic1,
                                           uint32_t *top_4_bytes, int16_t *score,
                                           uint8_t *fifth_byte,
                                           uint8_t *flag_and_depth) {
  *top_4_bytes = (uint32_t)(atomic1 & 0xFFFFFFFF);
  *score = (int16_t)((atomic1 >> 32) & 0xFFFF);
  *fifth_byte = (uint8_t)((atomic1 >> 48) & 0xFF);
  *flag_and_depth = (uint8_t)((atomic1 >> 56) & 0xFF);
}

// Legacy TTEntry accessors that work with unpacked values
typedef struct TTEntryData {
  uint32_t top_4_bytes;
  int16_t score;
  uint8_t fifth_byte;
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntryData;

// Full hash reconstruction from TTEntryData
inline static uint64_t ttentry_data_full_hash(TTEntryData t, uint64_t index) {
  return ((uint64_t)(t.top_4_bytes) << 32) + ((uint64_t)(t.fifth_byte) << 24) +
         (index & BOTTOM3_BYTE_MASK);
}

inline static uint8_t ttentry_data_flag(TTEntryData t) {
  return t.flag_and_depth >> 6;
}

inline static uint8_t ttentry_data_depth(TTEntryData t) {
  return t.flag_and_depth & DEPTH_MASK;
}

inline static int16_t ttentry_data_score(TTEntryData t) { return t.score; }

inline static bool ttentry_data_valid(TTEntryData t) {
  return ttentry_data_flag(t) != 0;
}

inline static uint64_t ttentry_data_move(TTEntryData t) { return t.tiny_move; }

inline static void ttentry_data_reset(TTEntryData *t) {
  t->top_4_bytes = 0;
  t->score = 0;
  t->fifth_byte = 0;
  t->flag_and_depth = 0;
  t->tiny_move = 0;
}

// Initialize atomic TTEntry to zero
inline static void ttentry_reset(TTEntry *t) {
  atomic_store_explicit(&t->atomic1, 0, memory_order_relaxed);
  atomic_store_explicit(&t->atomic2, 0, memory_order_relaxed);
}

typedef struct TranspositionTable {
  TTEntry *table;
  atomic_uchar *nproc;  // ABDADA: small table for tracking concurrent searches
  int size_power_of_2;
  uint64_t size_mask;
  Zobrist *zobrist;
  atomic_int created;
  atomic_int hits;
  atomic_int lookups;
  atomic_int t2_collisions;
  atomic_int torn_reads;  // Detected concurrent writes during read
  atomic_int depth_rejects;  // Entries not replaced due to depth preference
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
           (sizeof(TTEntry) * num_elems) / (size_t)(1024 * 1024));
  tt->table = malloc_or_die(sizeof(TTEntry) * num_elems);
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);
  // ABDADA: allocate smaller nproc table for tracking concurrent searches
  // Using a smaller table (256K vs millions) improves cache locality
  tt->nproc = (atomic_uchar *)malloc_or_die(sizeof(atomic_uchar) * NPROC_SIZE);
  for (int i = 0; i < NPROC_SIZE; i++) {
    atomic_init(&tt->nproc[i], 0);
  }
  tt->size_mask = num_elems - 1;
  tt->zobrist = zobrist_create(ctime_get_current_time());
  atomic_init(&tt->created, 0);
  atomic_init(&tt->hits, 0);
  atomic_init(&tt->lookups, 0);
  atomic_init(&tt->t2_collisions, 0);
  atomic_init(&tt->torn_reads, 0);
  atomic_init(&tt->depth_rejects, 0);
  return tt;
}

static inline void transposition_table_reset(TranspositionTable *tt) {
  // This function resets the transposition table. If you want to reallocate
  // space for it, destroy and recreate it with the new space.
  uint64_t num_elems = tt->size_mask + 1;
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);
  // ABDADA: reset nproc counters (smaller table)
  for (int i = 0; i < NPROC_SIZE; i++) {
    atomic_store_explicit(&tt->nproc[i], 0, memory_order_relaxed);
  }
  atomic_store(&tt->created, 0);
  atomic_store(&tt->hits, 0);
  atomic_store(&tt->lookups, 0);
  atomic_store(&tt->t2_collisions, 0);
  atomic_store(&tt->torn_reads, 0);
  atomic_store(&tt->depth_rejects, 0);
}

// Lookup with torn read detection: read atomic1, atomic2, atomic1 again
// If atomic1 changed, a concurrent write occurred - return invalid entry
static inline TTEntryData transposition_table_lookup(TranspositionTable *tt,
                                                     uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  atomic_fetch_add(&tt->lookups, 1);

  // Validated atomic read: detect torn reads from concurrent writes
  uint64_t atomic1_before =
      atomic_load_explicit(&tt->table[idx].atomic1, memory_order_acquire);
  uint64_t atomic2 =
      atomic_load_explicit(&tt->table[idx].atomic2, memory_order_acquire);
  uint64_t atomic1_after =
      atomic_load_explicit(&tt->table[idx].atomic1, memory_order_acquire);

  if (atomic1_before != atomic1_after) {
    // Torn read detected - concurrent write happened between our reads
    atomic_fetch_add(&tt->torn_reads, 1);
    TTEntryData e;
    ttentry_data_reset(&e);
    return e;
  }

  // Unpack the entry
  TTEntryData entry;
  ttentry_unpack_atomic1(atomic1_before, &entry.top_4_bytes, &entry.score,
                         &entry.fifth_byte, &entry.flag_and_depth);
  entry.tiny_move = atomic2;

  // Validate hash
  uint64_t full_hash = ttentry_data_full_hash(entry, idx);
  if (full_hash != zval) {
    if (ttentry_data_valid(entry)) {
      // There is another unrelated node at this position. This is a
      // type 2 collision.
      atomic_fetch_add(&tt->t2_collisions, 1);
    }
    TTEntryData e;
    ttentry_data_reset(&e);
    return e;
  }

  atomic_fetch_add(&tt->hits, 1);
  // Assume the same zobrist hash is the same position. If it's not, that's
  // a type 1 collision, which we can't do anything about. It should happen
  // extremely rarely.
  return entry;
}

// Store with atomic writes: write atomic2 first, then atomic1
// This ensures the validation check in lookup() works correctly
// Uses depth-preferred replacement: only replace if new depth >= existing depth
static inline void transposition_table_store(TranspositionTable *tt,
                                             uint64_t zval, TTEntryData tentry) {
  uint64_t idx = zval & tt->size_mask;
  uint32_t top_4_bytes = (uint32_t)(zval >> 32);
  uint8_t fifth_byte = (uint8_t)(zval >> 24);

  // Depth-preferred replacement: check if existing entry has greater depth
  uint64_t existing_atomic1 =
      atomic_load_explicit(&tt->table[idx].atomic1, memory_order_relaxed);
  if (existing_atomic1 != 0) {
    // Extract existing depth (bottom 6 bits of flag_and_depth, which is top byte)
    uint8_t existing_flag_and_depth = (uint8_t)((existing_atomic1 >> 56) & 0xFF);
    uint8_t existing_depth = existing_flag_and_depth & DEPTH_MASK;
    uint8_t new_depth = tentry.flag_and_depth & DEPTH_MASK;
    if (existing_depth > new_depth) {
      // Don't replace deeper entry with shallower one
      atomic_fetch_add(&tt->depth_rejects, 1);
      return;
    }
  }

  atomic_fetch_add(&tt->created, 1);

  // Pack into atomic format
  uint64_t atomic1 = ttentry_pack_atomic1(top_4_bytes, tentry.score,
                                          fifth_byte, tentry.flag_and_depth);

  // Write atomic2 first, then atomic1 (so validation check works)
  atomic_store_explicit(&tt->table[idx].atomic2, tentry.tiny_move,
                        memory_order_release);
  atomic_store_explicit(&tt->table[idx].atomic1, atomic1, memory_order_release);
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
  uint64_t idx = zval & NPROC_MASK;
  return atomic_load_explicit(&tt->nproc[idx], memory_order_relaxed) > 0;
}

// Enter node: increment nproc counter
// Uses relaxed ordering - we just need eventual visibility
static inline void transposition_table_enter_node(TranspositionTable *tt,
                                                  uint64_t zval) {
  uint64_t idx = zval & NPROC_MASK;
  atomic_fetch_add_explicit(&tt->nproc[idx], 1, memory_order_relaxed);
}

// Leave node: decrement nproc counter
// Uses relaxed ordering - we just need eventual visibility
static inline void transposition_table_leave_node(TranspositionTable *tt,
                                                  uint64_t zval) {
  uint64_t idx = zval & NPROC_MASK;
  atomic_fetch_sub_explicit(&tt->nproc[idx], 1, memory_order_relaxed);
}

#endif
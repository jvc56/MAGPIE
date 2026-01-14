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
  tt->size_mask = num_elems - 1;
  tt->zobrist = zobrist_create(ctime_get_current_time());
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
#ifdef TT_STATS
  atomic_fetch_add(&tt->lookups, 1);
#endif
  uint64_t full_hash = ttentry_full_hash(entry, idx);
  if (full_hash != zval) {
#ifdef TT_STATS
    if (ttentry_valid(entry)) {
      // There is another unrelated node at this position. This is a
      // type 2 collision.
      atomic_fetch_add(&tt->t2_collisions, 1);
    }
#endif
    TTEntry e;
    ttentry_reset(&e);
    return e;
  }
#ifdef TT_STATS
  atomic_fetch_add(&tt->hits, 1);
#endif
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
#ifdef TT_STATS
  atomic_fetch_add(&tt->created, 1);
#endif
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
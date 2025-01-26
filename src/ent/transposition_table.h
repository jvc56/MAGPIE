#ifndef TRANSPOSITION_TABLE_H
#define TRANSPOSITION_TABLE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../compat/memory_info.h"
#include "zobrist.h"

#define TT_EXACT 0x01
#define TT_LOWER 0x02
#define TT_UPPER 0x03

#define TTENTRY_SIZE_BYTES 16
#define BOTTOM3_BYTE_MASK ((1 << 24) - 1)
#define DEPTH_MASK ((1 << 6) - 1)

typedef struct __attribute__((packed)) TTEntry {
  // Don't store the full hash, but the top 5 bytes. The bottom 3 bytes
  // can be determined from the bucket in the array.
  uint64_t top_5_bytes : 40;
  int16_t score;
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntry;

inline static uint64_t ttentry_full_hash(TTEntry t, uint64_t index) {
  return (t.top_5_bytes << 24) | (index & BOTTOM3_BYTE_MASK);
}

inline static uint8_t ttentry_flag(TTEntry t) { return t.flag_and_depth >> 6; }

inline static uint8_t ttentry_depth(TTEntry t) {
  return t.flag_and_depth & DEPTH_MASK;
}

inline static int16_t ttentry_score(TTEntry t) { return t.score; }

inline static bool ttentry_valid(TTEntry t) { return ttentry_flag(t) != 0; }

inline static uint64_t ttentry_move(TTEntry t) { return t.tiny_move; }

inline static void ttentry_reset(TTEntry *t) {
  t->top_5_bytes = 0;
  t->score = 0;
  t->flag_and_depth = 0;
  t->tiny_move = 0;
}

typedef struct TranspositionTable {
  TTEntry *table;
  int size_power_of_2;
  uint64_t size_mask;
  Zobrist *zobrist;
  uint64_t created;
  uint64_t hits;
  uint64_t lookups;
  uint64_t t2_collisions;
} TranspositionTable;

static inline TranspositionTable *
transposition_table_create(double fraction_of_memory) {
  TranspositionTable *tt =
      (TranspositionTable *)malloc_or_die(sizeof(TranspositionTable));

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
           (sizeof(TTEntry) * num_elems) / (1024 * 1024));
  tt->table = (TTEntry *)malloc_or_die(sizeof(TTEntry) * num_elems);
  memset(tt->table, 0, sizeof(TTEntry) * num_elems);
  tt->size_mask = num_elems - 1;
  tt->zobrist = zobrist_create(time(NULL));
  tt->created = 0;
  ;
  tt->hits = 0;
  tt->lookups = 0;
  tt->t2_collisions = 0;
  return tt;
}

static inline void transposition_table_reset(TranspositionTable *tt) {
  // This function resets the transposition table. If you want to reallocate
  // space for it, destroy and recreate it with the new space.
  memset(tt->table, 0, sizeof(TTEntry) * (tt->size_mask + 1));
  tt->created = 0;
  ;
  tt->hits = 0;
  tt->lookups = 0;
  tt->t2_collisions = 0;
}

static inline TTEntry transposition_table_lookup(TranspositionTable *tt,
                                                 uint64_t zval) {
  uint64_t idx = zval & tt->size_mask;
  TTEntry entry = tt->table[idx];
  tt->lookups++;
  uint64_t full_hash = ttentry_full_hash(entry, idx);
  if (full_hash != zval) {
    if (ttentry_valid(entry)) {
      // There is another unrelated node at this position. This is a
      // type 2 collision.
      tt->t2_collisions++;
    }
    TTEntry e;
    ttentry_reset(&e);
    return e;
  }
  tt->hits++;
  // Assume the same zobrist hash is the same position. If it's not, that's
  // a type 1 collision, which we can't do anything about. It should happen
  // extremely rarely.
  return entry;
}

static inline void transposition_table_store(TranspositionTable *tt,
                                             uint64_t zval, TTEntry tentry) {
  uint64_t idx = zval & tt->size_mask;
  tentry.top_5_bytes = zval >> 24;
  tt->created++;
  tt->table[idx] = tentry;
}

static inline void transposition_table_destroy(TranspositionTable *tt) {
  zobrist_destroy(tt->zobrist);
  free(tt->table);
  free(tt);
}

#endif
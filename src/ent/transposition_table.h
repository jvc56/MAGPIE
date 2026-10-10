#ifndef TRANSPOSITION_TABLE_H
#define TRANSPOSITION_TABLE_H

#include "../compat/ctime.h"
#include "../compat/malloc.h"
#include "../compat/memory_info.h"
#include "zobrist.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// 16-byte entries for lockless hashing on all platforms
#define TTENTRY_SIZE_BYTES 16

// Entries are grouped into buckets that share an index and fill one cache line
// (4 x 16 bytes). A bucket is searched linearly on lookup and a store replaces
// the shallowest entry in it, so a deep entry is not evicted by a shallow one
// that happens to map to the same slot.
#ifndef TT_BUCKET_POWER
#define TT_BUCKET_POWER 2
#endif
#define TT_BUCKET_SIZE (1 << TT_BUCKET_POWER)
// The diagnostic counters are updated on every lookup and store. When many
// threads share a table that write contention costs ~8% of endgame solve time,
// so release builds (NDEBUG) skip it; test and dev builds keep the counters.
#ifdef NDEBUG
#define TT_STAT(x) ((void)0)
#else
#define TT_STAT(x) x
#endif
#define TT_STORED_HASH_MASK ((1ULL << 40) - 1)

#ifdef __EMSCRIPTEN__
#define TT_MIN_SIZE_POWER 12 // 64 KiB permits splitting a browser PEG budget
#else
#define TT_MIN_SIZE_POWER 24 // 2^24 minimum (256 MB) for performance
#endif

enum {
  TT_EXACT = 0x01,
  TT_LOWER = 0x02,
  TT_UPPER = 0x03,
  DEPTH_MASK = ((1 << 6) - 1),
  // ABDADA nproc table: use a smaller table than TT to reduce memory and
  // improve cache locality. Collisions just cause extra deferrals.
  NPROC_SIZE_POWER = 18, // 2^18 = 256K entries
  NPROC_SIZE = (1 << NPROC_SIZE_POWER),
  NPROC_MASK = (NPROC_SIZE - 1),
};

typedef struct TTEntry {
  uint32_t top_4_bytes; // Bits [63:32] of hash
  int16_t score;
  uint8_t fifth_byte; // Bits [31:24] of hash (40 stored bits total)
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntry;

static_assert(sizeof(TTEntry) == TTENTRY_SIZE_BYTES,
              "TTEntry must be exactly 16 bytes for lockless hashing");

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
  _Atomic uint64_t *table; // Pairs of uint64_t for lockless hashing
  atomic_uchar *nproc; // ABDADA: small table for tracking concurrent searches
  int size_power_of_2; // log2 of the number of entries
  int index_bits;      // log2 of the number of buckets
  uint64_t bucket_mask;
  Zobrist *zobrist;
  // Diagnostic counters, maintained only when NDEBUG is not defined (see
  // TT_STAT).
  atomic_int created;
  atomic_int hits;
  atomic_int lookups;
  atomic_int t2_collisions;
} TranspositionTable;

// Maximum table count that fits even at the minimum entry count.
static inline int transposition_table_max_workers(double total_fraction) {
  const uint64_t minimum = ((uint64_t)TTENTRY_SIZE_BYTES << TT_MIN_SIZE_POWER) +
                           (NPROC_SIZE * sizeof(atomic_uchar)) +
                           sizeof(TranspositionTable) +
                           zobrist_allocation_size();
  return (int)(total_fraction * (double)get_total_memory() / (double)minimum);
}

// Reserve overhead as well as entries within an aggregate worker budget.
static inline double transposition_table_worker_fraction(double total_fraction,
                                                         int workers) {
  const double memory = (double)get_total_memory();
  const uint64_t overhead = (NPROC_SIZE * sizeof(atomic_uchar)) +
                            sizeof(TranspositionTable) +
                            zobrist_allocation_size();
  const double fraction =
      (total_fraction / workers) - ((double)overhead / memory);
  const double minimum =
      (double)((uint64_t)TTENTRY_SIZE_BYTES << TT_MIN_SIZE_POWER) / memory;
  return fraction >= minimum ? fraction : 0;
}

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
    log_warn("TT size clamped to minimum: 2^%d elements (%d KiB)",
             TT_MIN_SIZE_POWER,
             (1 << TT_MIN_SIZE_POWER) * TTENTRY_SIZE_BYTES / 1024);
  }
  int num_elems = 1 << tt->size_power_of_2;
  size_t memory_mb = ((size_t)TTENTRY_SIZE_BYTES * num_elems) / (1024 * 1024);
  log_info("Creating transposition table. System memory: %llu, TT size: 2^%d "
           "(elements: %d, memory: %zu MB)",
           (unsigned long long)total_memory, tt->size_power_of_2, num_elems,
           memory_mb);
  tt->index_bits = tt->size_power_of_2 - TT_BUCKET_POWER;
  tt->bucket_mask = ((uint64_t)1 << tt->index_bits) - 1;
  void *table_memory;
  if (portable_aligned_alloc(&table_memory, TT_BUCKET_SIZE * TTENTRY_SIZE_BYTES,
                             (size_t)TTENTRY_SIZE_BYTES * num_elems) != 0) {
    log_fatal("failed to allocate transposition table");
  }
  tt->table = (_Atomic uint64_t *)table_memory;
  memset(tt->table, 0, sizeof(uint64_t) * 2 * num_elems);
  // ABDADA: allocate smaller nproc table for tracking concurrent searches
  // Using a smaller table (256K vs millions) improves cache locality
  tt->nproc = (atomic_uchar *)malloc_or_die(sizeof(atomic_uchar) * NPROC_SIZE);
  for (int i = 0; i < NPROC_SIZE; i++) {
    atomic_init(&tt->nproc[i], 0);
  }
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
  const uint64_t num_elems = (uint64_t)1 << tt->size_power_of_2;
  memset(tt->table, 0, sizeof(uint64_t) * 2 * num_elems);
  // ABDADA: reset nproc counters (smaller table)
  for (int i = 0; i < NPROC_SIZE; i++) {
    atomic_store_explicit(&tt->nproc[i], 0, memory_order_relaxed);
  }
  atomic_store(&tt->created, 0);
  atomic_store(&tt->hits, 0);
  atomic_store(&tt->lookups, 0);
  atomic_store(&tt->t2_collisions, 0);
}

// Lockless hashing (Hyatt 1999): each TTEntry is stored as two 8-byte halves
// with the key half XOR'd against the data half. Each half is loaded
// atomically (relaxed ordering) so it is internally consistent. If a
// concurrent write causes a torn read (halves from different writes), the XOR
// produces invalid hash bits and the check rejects the entry, preventing
// incorrect alpha-beta pruning from corrupted TT data.
static inline TTEntry tt_load_slot(const _Atomic uint64_t *slot) {
  uint64_t xored_key = atomic_load_explicit(&slot[0], memory_order_relaxed);
  uint64_t data = atomic_load_explicit(&slot[1], memory_order_relaxed);
  uint64_t key_half = xored_key ^ data;
  TTEntry entry;
  memcpy(&entry, &key_half, 8);
  entry.tiny_move = data;
  return entry;
}

inline static uint64_t ttentry_stored_hash(TTEntry t) {
  return ((uint64_t)(t.top_4_bytes) << 8) | t.fifth_byte;
}

static inline TTEntry transposition_table_lookup(TranspositionTable *tt,
                                                 uint64_t zval) {
  const uint64_t bucket = zval & tt->bucket_mask;
  const uint64_t want = (zval >> tt->index_bits) & TT_STORED_HASH_MASK;
  TT_STAT(atomic_fetch_add(&tt->lookups, 1));

  const _Atomic uint64_t *slots = &tt->table[bucket * TT_BUCKET_SIZE * 2];
  bool other_valid = false;
  for (int i = 0; i < TT_BUCKET_SIZE; i++) {
    TTEntry entry = tt_load_slot(&slots[i * 2]);
    if (ttentry_valid(entry) && ttentry_stored_hash(entry) == want) {
      TT_STAT(atomic_fetch_add(&tt->hits, 1));
      // Assume the same zobrist hash is the same position. If it's not,
      // that's a type 1 collision, which we can't do anything about. It
      // should happen extremely rarely.
      return entry;
    }
    other_valid |= ttentry_valid(entry);
  }
  if (other_valid) {
    // There are only unrelated nodes in this bucket. This is a type 2
    // collision.
    TT_STAT(atomic_fetch_add(&tt->t2_collisions, 1));
  }
  TTEntry e;
  ttentry_reset(&e);
  return e;
}

static inline void transposition_table_store(TranspositionTable *tt,
                                             uint64_t zval, TTEntry tentry) {
  const uint64_t bucket = zval & tt->bucket_mask;
  // Store the low 40 bits of the hash that remains after removing the index.
  const uint64_t stored_hash = (zval >> tt->index_bits) & TT_STORED_HASH_MASK;
  tentry.top_4_bytes = (uint32_t)(stored_hash >> 8);
  tentry.fifth_byte = (uint8_t)(stored_hash & 0xFF);
  TT_STAT(atomic_fetch_add(&tt->created, 1));

  _Atomic uint64_t *slots = &tt->table[bucket * TT_BUCKET_SIZE * 2];
  // Replace, in order of preference: the entry for this position, an empty
  // slot, the shallowest entry.
  int victim = 0;
  int victim_rank = INT32_MAX;
  for (int i = 0; i < TT_BUCKET_SIZE; i++) {
    TTEntry entry = tt_load_slot(&slots[i * 2]);
    int rank;
    if (ttentry_valid(entry)) {
      if (ttentry_stored_hash(entry) == stored_hash) {
        victim = i;
        break;
      }
      rank = 1 + ttentry_depth(entry);
      // A pure entry (depth DEPTH_MASK) comes from a subtree that reached only
      // terminal positions, which is cheap to search again, so it is replaced
      // before any entry that records real search effort.
      if (ttentry_depth(entry) == DEPTH_MASK) {
        rank = 1;
      }
    } else {
      rank = 0;
    }
    if (rank < victim_rank) {
      victim_rank = rank;
      victim = i;
    }
  }

  uint64_t key_half;
  memcpy(&key_half, &tentry, 8);
  _Atomic uint64_t *slot = &slots[victim * 2];
  atomic_store_explicit(&slot[0], key_half ^ tentry.tiny_move,
                        memory_order_relaxed);
  atomic_store_explicit(&slot[1], tentry.tiny_move, memory_order_relaxed);
}

static inline void transposition_table_destroy(TranspositionTable *tt) {
  if (!tt) {
    return;
  }
  zobrist_destroy(tt->zobrist);
  portable_aligned_free(tt->table);
  free(tt->nproc);
  free(tt);
}

// ABDADA functions for tracking concurrent node searches
// Uses a smaller table (256K entries) with relaxed memory ordering for speed

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
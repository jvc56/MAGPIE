#ifndef TRANSPOSITION_TABLE_H
#define TRANSPOSITION_TABLE_H

#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "zobrist.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// 16-byte entries for lockless hashing on all platforms
#define TTENTRY_SIZE_BYTES 16

#ifdef __EMSCRIPTEN__
#define TT_MIN_SIZE_POWER 21 // 2^21 minimum (32 MB) for mobile
#else
#define TT_MIN_SIZE_POWER 24 // 2^24 minimum (256 MB) for performance
#endif

// Number of low hash bits stored per entry as a verification tag. fastrange
// indexing (see tt_bucket_index) derives the bucket from the HIGH hash bits, so
// the LOW bits are what discriminate distinct positions within a bucket. As
// long as the table has at least 2^(64 - TT_TAG_BITS) entries, every bucket
// spans at most 2^TT_TAG_BITS hashes, so two distinct hashes in the same bucket
// cannot share a tag -- verification is exact, matching the old power-of-2 +
// index-reconstruction scheme. With TT_TAG_BITS == 40 that threshold is 2^24,
// i.e. TT_MIN_SIZE_POWER on native builds (WASM's 2^21 minimum loses a few high
// bits, the same tradeoff the old scheme accepted on small tables).
#define TT_TAG_BITS 40
#define TT_TAG_MASK ((UINT64_C(1) << TT_TAG_BITS) - 1)

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
  // top_4_bytes and fifth_byte together hold the 40-bit verification tag: the
  // low TT_TAG_BITS bits of the hash (top_4_bytes = tag bits [39:8],
  // fifth_byte = tag bits [7:0]).
  uint32_t top_4_bytes;
  int16_t score;
  uint8_t fifth_byte;
  uint8_t flag_and_depth;
  uint64_t tiny_move;
} TTEntry;

static_assert(sizeof(TTEntry) == TTENTRY_SIZE_BYTES,
              "TTEntry must be exactly 16 bytes for lockless hashing");

// The 40-bit verification tag stored in an entry (low TT_TAG_BITS hash bits).
inline static uint64_t ttentry_tag(TTEntry t) {
  return ((uint64_t)(t.top_4_bytes) << 8) | t.fifth_byte;
}

// Lemire's fastrange (multiply-shift) reduction: map a uniformly distributed
// 64-bit hash into [0, num_entries) without requiring num_entries to be a power
// of two -- floor(zval * num_entries / 2^64), one 64x64->128 multiply + shift.
// https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
inline static uint64_t tt_bucket_index(uint64_t zval, uint64_t num_entries) {
  return (uint64_t)(((__uint128_t)zval * (__uint128_t)num_entries) >> 64);
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
  _Atomic uint64_t *table; // Pairs of uint64_t for lockless hashing
  atomic_uchar *nproc; // ABDADA: small table for tracking concurrent searches
  // Number of entries; NOT required to be a power of two thanks to fastrange
  // indexing, so the table fills the requested memory fraction exactly instead
  // of rounding down to the nearest power of two.
  uint64_t num_entries;
  Zobrist *zobrist;
  // 64-bit so they don't overflow on long searches (a deep solve can do
  // billions of lookups, well past the old atomic_int range).
  atomic_uint_fast64_t created;
  atomic_uint_fast64_t hits;
  atomic_uint_fast64_t lookups;
  atomic_uint_fast64_t t2_collisions;
} TranspositionTable;

static inline TranspositionTable *
transposition_table_create(double fraction_of_memory) {
  TranspositionTable *tt = malloc_or_die(sizeof(TranspositionTable));

  uint64_t total_memory = get_total_memory();
  uint64_t desired_n_elems =
      (uint64_t)(fraction_of_memory *
                 ((double)(total_memory) / (double)TTENTRY_SIZE_BYTES));

  // Platform-specific minimum table size.
  const uint64_t min_entries = UINT64_C(1) << TT_MIN_SIZE_POWER;
  if (desired_n_elems < min_entries) {
    if (desired_n_elems > 0) {
      log_warn("TT size clamped to minimum: %llu elements (%llu MB)",
               (unsigned long long)min_entries,
               (unsigned long long)(min_entries * TTENTRY_SIZE_BYTES /
                                    (1024 * 1024)));
    }
    desired_n_elems = min_entries;
  }

  tt->num_entries = desired_n_elems;
  size_t memory_mb =
      (size_t)(TTENTRY_SIZE_BYTES * tt->num_entries) / (1024 * 1024);
  log_info("Creating transposition table. System memory: %llu, TT entries: "
           "%llu (memory: %zu MB)",
           (unsigned long long)total_memory,
           (unsigned long long)tt->num_entries, memory_mb);
  tt->table =
      (_Atomic uint64_t *)malloc_or_die(sizeof(uint64_t) * 2 * tt->num_entries);
  memset(tt->table, 0, sizeof(uint64_t) * 2 * tt->num_entries);
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
  memset(tt->table, 0, sizeof(uint64_t) * 2 * tt->num_entries);
  // ABDADA: reset nproc counters (smaller table)
  for (int i = 0; i < NPROC_SIZE; i++) {
    atomic_store_explicit(&tt->nproc[i], 0, memory_order_relaxed);
  }
  atomic_store(&tt->created, 0);
  atomic_store(&tt->hits, 0);
  atomic_store(&tt->lookups, 0);
  atomic_store(&tt->t2_collisions, 0);
}

static inline TTEntry transposition_table_lookup(TranspositionTable *tt,
                                                 uint64_t zval) {
  uint64_t idx = tt_bucket_index(zval, tt->num_entries);
  atomic_fetch_add(&tt->lookups, 1);

  // Lockless hashing (Hyatt 1999): each TTEntry is stored as two 8-byte
  // halves with the key half XOR'd against the data half. Each half is
  // loaded atomically (relaxed ordering) so it is internally consistent.
  // If a concurrent write causes a torn read (halves from different writes),
  // the XOR produces invalid hash bits and the check below rejects the entry,
  // preventing incorrect alpha-beta pruning from corrupted TT data.
  const _Atomic uint64_t *slot = &tt->table[idx * 2];
  uint64_t xored_key = atomic_load_explicit(&slot[0], memory_order_relaxed);
  uint64_t data = atomic_load_explicit(&slot[1], memory_order_relaxed);
  uint64_t key_half = xored_key ^ data;

  TTEntry entry;
  memcpy(&entry, &key_half, 8);
  entry.tiny_move = data;

  if (ttentry_tag(entry) != (zval & TT_TAG_MASK)) {
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
  uint64_t idx = tt_bucket_index(zval, tt->num_entries);
  // Store the low TT_TAG_BITS of the hash as the verification tag (the high
  // bits are encoded by the bucket index under fastrange).
  uint64_t tag = zval & TT_TAG_MASK;
  tentry.top_4_bytes = (uint32_t)(tag >> 8);
  tentry.fifth_byte = (uint8_t)(tag & 0xFF);
  atomic_fetch_add(&tt->created, 1);

  // Lockless hashing: XOR key half with data half so torn reads
  // (one half from one write, the other from a different write)
  // are detected by hash mismatch on lookup.
  uint64_t key_half;
  memcpy(&key_half, &tentry, 8);
  _Atomic uint64_t *slot = &tt->table[idx * 2];
  atomic_store_explicit(&slot[0], key_half ^ tentry.tiny_move,
                        memory_order_relaxed);
  atomic_store_explicit(&slot[1], tentry.tiny_move, memory_order_relaxed);
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

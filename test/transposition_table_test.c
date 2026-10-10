#include "transposition_table_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/transposition_table.h"
#include "../src/ent/xoshiro.h"
#include "../src/ent/zobrist.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

void test_transposition_table(void) {
  const uint64_t zobrist_bytes =
      sizeof(Zobrist) + prng_allocation_size() +
      ((uint64_t)BOARD_DIM * BOARD_DIM * sizeof(uint64_t *)) +
      ((uint64_t)BOARD_DIM * BOARD_DIM * ZOBRIST_MAX_LETTERS * 2 *
       sizeof(uint64_t)) +
      (2ULL * ZOBRIST_MAX_LETTERS * sizeof(uint64_t *)) +
      (2ULL * ZOBRIST_MAX_LETTERS * (RACK_SIZE + 1) * sizeof(uint64_t));
  assert(zobrist_allocation_size() == zobrist_bytes);
#ifdef __EMSCRIPTEN__
  const uint64_t budget_base = get_total_memory();
  assert(budget_base == 256ULL * 1024 * 1024);
  // Deep PEG may allocate a table for every worker plus the helper. Check
  // actual allocations, including ABDADA storage, at every supported count.
  for (int budget_mb = 16; budget_mb <= 32; budget_mb *= 2) {
    const int capacity =
        transposition_table_max_workers((double)budget_mb / 256);
    assert(capacity >= 33);
    assert(transposition_table_worker_fraction((double)budget_mb / 256,
                                               capacity + 1) == 0);
    for (int workers = 1; workers <= capacity; workers++) {
      TranspositionTable *worker_tt =
          transposition_table_create(transposition_table_worker_fraction(
              (double)budget_mb / 256, workers));
      const uint64_t bytes =
          ((uint64_t)TTENTRY_SIZE_BYTES << worker_tt->size_power_of_2) +
          NPROC_SIZE * sizeof(atomic_uchar) + sizeof(TranspositionTable) +
          zobrist_bytes;
      assert(bytes * workers <= (uint64_t)budget_mb * 1024 * 1024);
      transposition_table_destroy(worker_tt);
    }
    TranspositionTable *endgame_tt =
        transposition_table_create((double)budget_mb / 256);
    assert(((uint64_t)TTENTRY_SIZE_BYTES << endgame_tt->size_power_of_2) ==
           (uint64_t)budget_mb * 1024 * 1024);
    transposition_table_destroy(endgame_tt);
  }
#endif
  // Passing 0 clamps to the platform minimum.
  TranspositionTable *tt = transposition_table_create(0);
  assert(tt->size_power_of_2 == TT_MIN_SIZE_POWER);

  // The top bits are set so this hash would be lost if the stored bits were
  // reconstructed and compared in full: only 40 hash bits are stored, so
  // small tables (WASM, size_power=21) drop the top bits of the hash.
  const uint64_t base_hash = 0xF234567890ABCDEFULL;

  TTEntry entry;
  ttentry_reset(&entry);
  entry.score = 12;
  entry.flag_and_depth = 128 + 64 + 23;
  transposition_table_store(tt, base_hash, entry);

  TTEntry lu_entry = transposition_table_lookup(tt, base_hash);
  assert(ttentry_depth(lu_entry) == 23);
  assert(ttentry_flag(lu_entry) == TT_UPPER);
  assert(ttentry_score(lu_entry) == 12);

  assert(atomic_load(&tt->t2_collisions) == 0);
  // Create a type-2 collision: same index bits, different stored hash.
  // Offset by 2^size_power so the index wraps to the same slot.
  const uint64_t collision_hash = base_hash + (1ULL << tt->size_power_of_2);
  TTEntry te = transposition_table_lookup(tt, collision_hash);
  assert(te.fifth_byte == 0);
  assert(te.top_4_bytes == 0);
  assert(te.flag_and_depth == 0);
  assert(te.score == 0);
  assert(te.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);

  // Another lookup, but not a collision (different index).
  TTEntry te2 = transposition_table_lookup(tt, base_hash + 1);
  assert(te2.fifth_byte == 0);
  assert(te2.top_4_bytes == 0);
  assert(te2.flag_and_depth == 0);
  assert(te2.score == 0);
  assert(te2.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);
  assert(atomic_load(&tt->lookups) == 3);

  // Entries whose hashes share a bucket but differ in the stored bits coexist,
  // up to the bucket size.
  const uint64_t bucket_bits = 0x5A5A5AULL & tt->bucket_mask;
  uint64_t bucket_hashes[TT_BUCKET_SIZE + 1];
  for (int i = 0; i <= TT_BUCKET_SIZE; i++) {
    bucket_hashes[i] = ((uint64_t)(i + 1) << tt->index_bits) | bucket_bits;
  }
  for (int i = 0; i < TT_BUCKET_SIZE; i++) {
    TTEntry e;
    ttentry_reset(&e);
    e.score = (int16_t)i;
    e.flag_and_depth = (uint8_t)((TT_EXACT << 6) + 5 * (i + 1));
    transposition_table_store(tt, bucket_hashes[i], e);
  }
  for (int i = 0; i < TT_BUCKET_SIZE; i++) {
    const TTEntry e = transposition_table_lookup(tt, bucket_hashes[i]);
    assert(ttentry_valid(e));
    assert(ttentry_score(e) == i);
    assert(ttentry_depth(e) == 5 * (i + 1));
  }

  // Storing the same position again replaces its entry in place.
  TTEntry same;
  ttentry_reset(&same);
  same.score = 99;
  same.flag_and_depth = (TT_LOWER << 6) + 1;
  transposition_table_store(tt, bucket_hashes[1], same);
  const TTEntry same_lu = transposition_table_lookup(tt, bucket_hashes[1]);
  assert(ttentry_score(same_lu) == 99);
  assert(ttentry_depth(same_lu) == 1);
  assert(ttentry_valid(transposition_table_lookup(tt, bucket_hashes[0])));

  // A new position in a full bucket evicts the shallowest entry (now the one
  // that was just overwritten with depth 1) and keeps the deeper ones.
  TTEntry extra;
  ttentry_reset(&extra);
  extra.score = 77;
  extra.flag_and_depth = (TT_EXACT << 6) + 30;
  transposition_table_store(tt, bucket_hashes[TT_BUCKET_SIZE], extra);
  assert(ttentry_score(transposition_table_lookup(
             tt, bucket_hashes[TT_BUCKET_SIZE])) == 77);
  assert(!ttentry_valid(transposition_table_lookup(tt, bucket_hashes[1])));
  for (int i = 0; i < TT_BUCKET_SIZE; i++) {
    if (i != 1) {
      assert(ttentry_valid(transposition_table_lookup(tt, bucket_hashes[i])));
    }
  }

  transposition_table_destroy(tt);
}
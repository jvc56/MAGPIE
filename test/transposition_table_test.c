#include "../src/ent/transposition_table.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

void test_transposition_table(void) {
  // Passing 0 clamps to the platform minimum: 2^24 native, 2^21 WASM.
  TranspositionTable *tt = transposition_table_create(0);
  assert(tt->num_entries == (UINT64_C(1) << TT_MIN_SIZE_POWER));

  const uint64_t base_hash = 1234567890123456789ULL;

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
  // Type-2 collision: a hash that lands in the same fastrange bucket (shared
  // high bits) but carries a different verification tag (low bits). For a
  // power-of-two table fastrange(zval) == zval >> (64 - TT_MIN_SIZE_POWER), so
  // bumping only the low bits keeps the bucket and changes the tag.
  const uint64_t collision_hash = base_hash + 1;
  assert(tt_bucket_index(collision_hash, tt->num_entries) ==
         tt_bucket_index(base_hash, tt->num_entries));
  TTEntry te = transposition_table_lookup(tt, collision_hash);
  assert(te.fifth_byte == 0);
  assert(te.top_4_bytes == 0);
  assert(te.flag_and_depth == 0);
  assert(te.score == 0);
  assert(te.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);

  // A lookup that lands in a different (empty) bucket is not a collision.
  // Adding 2^TT_TAG_BITS changes the high (index) bits.
  const uint64_t other_hash = base_hash + (UINT64_C(1) << TT_TAG_BITS);
  assert(tt_bucket_index(other_hash, tt->num_entries) !=
         tt_bucket_index(base_hash, tt->num_entries));
  TTEntry te2 = transposition_table_lookup(tt, other_hash);
  assert(te2.fifth_byte == 0);
  assert(te2.top_4_bytes == 0);
  assert(te2.flag_and_depth == 0);
  assert(te2.score == 0);
  assert(te2.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);
  assert(atomic_load(&tt->lookups) == 3);

  transposition_table_destroy(tt);
}

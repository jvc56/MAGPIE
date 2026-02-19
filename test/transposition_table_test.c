#include "../src/ent/transposition_table.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

void test_transposition_table(void) {
  // Passing 0 clamps to the platform minimum: 2^24 native, 2^21 WASM.
  TranspositionTable *tt = transposition_table_create(0);
  assert(tt->size_power_of_2 == TT_MIN_SIZE_POWER);

  // Use a hash < 2^61 so all stored bits survive the round-trip on both
  // native (size_power=24, 0 bits lost) and WASM (size_power=21, 3 bits lost).
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

  transposition_table_destroy(tt);
}
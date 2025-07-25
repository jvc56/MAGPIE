#include <assert.h>
#include <stdatomic.h>

#include "../src/ent/transposition_table.h"

void test_transposition_table(void) {
  // Ensure that tt size is 2**24 in size by passing in 0.
  TranspositionTable *tt = transposition_table_create(0);
  assert(tt->size_power_of_2 == 24);

  TTEntry entry;
  ttentry_reset(&entry);
  entry.score = 12;
  entry.flag_and_depth = 128 + 64 + 23;
  transposition_table_store(tt, 9409641586937047728ULL, entry);

  TTEntry lu_entry = transposition_table_lookup(tt, 9409641586937047728ULL);
  assert(ttentry_depth(lu_entry) == 23);
  assert(ttentry_flag(lu_entry) == TT_UPPER);
  assert(ttentry_score(lu_entry) == 12);
  assert(lu_entry.top_4_bytes == 2190852907);
  assert(lu_entry.fifth_byte == 61);

  assert(atomic_load(&tt->t2_collisions) == 0);
  // create a collision. This index is 16777216 higher than the last one, or
  // 2**24.
  TTEntry te = transposition_table_lookup(tt, 9409641586953824944ULL);
  assert(te.fifth_byte == 0);
  assert(te.top_4_bytes == 0);
  assert(te.flag_and_depth == 0);
  assert(te.score == 0);
  assert(te.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);

  // another lookup, but not a collision.
  TTEntry te2 = transposition_table_lookup(tt, 9409641586937047728ULL + 1);
  assert(te2.fifth_byte == 0);
  assert(te2.top_4_bytes == 0);
  assert(te2.flag_and_depth == 0);
  assert(te2.score == 0);
  assert(te2.tiny_move == 0);
  assert(atomic_load(&tt->t2_collisions) == 1);
  assert(atomic_load(&tt->lookups) == 3);

  transposition_table_destroy(tt);
}
#include "test_constants.h"
#include "../src/ent/rack_hash_table.h"
#include "test_util.h"
#include "rack_hash_table_test.h"
#include <assert.h>

void test_rack_hash_table_basic(void) {
  RackHashTable *rht = rack_hash_table_create(1024, 10, 16);
  assert(rht != NULL);

  BitRack rack1 = bit_rack_create_empty();
  bit_rack_add_letter(&rack1, 0); // Blank

  Move *m1 = move_create();
  move_set_score(m1, 100);
  move_set_equity(m1, 150);

  rack_hash_table_add_move(rht, &rack1, 10, 5, 0.5f, m1);

  InferredRackMoveList *res = rack_hash_table_lookup(rht, &rack1);
  assert(res != NULL);
  assert(bit_rack_equals(&res->rack, &rack1));
  assert(res->leave_value == 10);
  assert(res->draws == 5);
  assert(res->weight == 0.5f);
  assert(res->moves->count == 1);
  assert(move_get_score(res->moves->moves[0]) == 100);

  move_destroy(m1);
  rack_hash_table_destroy(rht);
}

void test_rack_hash_table_top_moves(void) {
  int cap = 5;
  RackHashTable *rht = rack_hash_table_create(1024, cap, 16);

  BitRack rack1 = bit_rack_create_empty();
  bit_rack_add_letter(&rack1, 1); // 'A' (assuming 0 is blank)

  for (int i = 0; i < 10; i++) {
    Move *m = move_create();
    move_set_score(m, i * 10);
    move_set_equity(m, i * 10); // Equity matches score for simplicity
    rack_hash_table_add_move(rht, &rack1, 5, 1, 1.0f, m);
    move_destroy(m);
  }

  InferredRackMoveList *res = rack_hash_table_lookup(rht, &rack1);
  assert(res != NULL);
  assert(res->moves->count == cap);

  // Verify we kept the top 5 (equities 50, 60, 70, 80, 90)
  // MoveList is a min-heap. The root (index 0) should be the smallest of the
  // top 5, which is 50.
  assert(move_get_equity(res->moves->moves[0]) == 50);

  // Sort them to verify full content
  move_list_sort_moves(res->moves);
  // Now they should be sorted descending: 90, 80, 70, 60, 50
  assert(move_get_equity(res->moves->moves[0]) == 90);
  assert(move_get_equity(res->moves->moves[4]) == 50);

  rack_hash_table_destroy(rht);
}

void test_rack_hash_table_collisions(void) {
  // Force collisions by using small bucket count
  RackHashTable *rht = rack_hash_table_create(2, 10, 1);

  BitRack r1 = bit_rack_create_empty();
  bit_rack_add_letter(&r1, 0);
  BitRack r2 = bit_rack_create_empty();
  bit_rack_add_letter(&r2, 1);
  BitRack r3 = bit_rack_create_empty();
  bit_rack_add_letter(&r3, 2);

  Move *m = move_create();

  rack_hash_table_add_move(rht, &r1, 1, 1, 1.0f, m);
  rack_hash_table_add_move(rht, &r2, 2, 1, 1.0f, m);
  rack_hash_table_add_move(rht, &r3, 3, 1, 1.0f, m);

  InferredRackMoveList *l1 = rack_hash_table_lookup(rht, &r1);
  InferredRackMoveList *l2 = rack_hash_table_lookup(rht, &r2);
  InferredRackMoveList *l3 = rack_hash_table_lookup(rht, &r3);

  assert(l1 != NULL && bit_rack_equals(&l1->rack, &r1));
  assert(l2 != NULL && bit_rack_equals(&l2->rack, &r2));
  assert(l3 != NULL && bit_rack_equals(&l3->rack, &r3));

  move_destroy(m);
  rack_hash_table_destroy(rht);
}

void rack_hash_table_test(void) {
  test_rack_hash_table_basic();
  test_rack_hash_table_top_moves();
  test_rack_hash_table_collisions();
  // printf("All rack_hash_table tests passed.\n");
}

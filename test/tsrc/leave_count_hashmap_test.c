#include <assert.h>

#include "../../src/ent/leave_count_hashmap.h"

#include "test_util.h"

void test_leave_count_hashmap(void) {

  uint64_t capacity = 1;
  LeaveCountHashMap *hm = leave_count_hashmap_create(capacity);

  leave_count_hashmap_set(hm, 1, 1);
  assert(leave_count_hashmap_get(hm, 1) == 1);
  assert(leave_count_hashmap_get_num_entries(hm) == 1);

  leave_count_hashmap_reset(hm);
  assert(leave_count_hashmap_get_num_entries(hm) == 0);

  assert(leave_count_hashmap_get(hm, 1) == UNSET_KEY_VALUE);

  leave_count_hashmap_destroy(hm);

  capacity = 10;
  hm = leave_count_hashmap_create(capacity);

  leave_count_hashmap_set(hm, 1, 1);
  assert(leave_count_hashmap_get(hm, 1) == 1);
  assert(leave_count_hashmap_get_num_entries(hm) == 1);

  leave_count_hashmap_set(hm, 2, 1);
  assert(leave_count_hashmap_get(hm, 2) == 1);
  assert(leave_count_hashmap_get_num_entries(hm) == 2);

  leave_count_hashmap_reset(hm);
  assert(leave_count_hashmap_get_num_entries(hm) == 0);

  assert(leave_count_hashmap_get(hm, 1) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, 2) == UNSET_KEY_VALUE);

  for (uint64_t i = 0; i < capacity; i++) {
    leave_count_hashmap_set(hm, i, i * 7);
  }

  for (uint64_t i = 0; i < capacity; i++) {
    assert(leave_count_hashmap_get(hm, i) == i * 7);
  }

  int num_cycles = 10;

  for (uint64_t i = 0; i < capacity * num_cycles; i++) {
    leave_count_hashmap_set(hm, i % capacity, i * 7);
    assert(leave_count_hashmap_get(hm, i % capacity) == i * 7);
  }

  for (uint64_t i = capacity * (num_cycles - 1); i < capacity * num_cycles;
       i++) {
    assert(leave_count_hashmap_get(hm, i % capacity) == i * 7);
  }

  assert(leave_count_hashmap_get_num_entries(hm) == capacity);

  leave_count_hashmap_reset(hm);
  assert(leave_count_hashmap_get_num_entries(hm) == 0);

  for (uint64_t i = 0; i < capacity; i++) {
    leave_count_hashmap_set(hm, capacity * i, i);
  }

  for (uint64_t i = 0; i < capacity; i++) {
    assert(leave_count_hashmap_get(hm, capacity * i) == i);
  }

  assert(leave_count_hashmap_get_num_entries(hm) == capacity);

  leave_count_hashmap_delete(hm, 0);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == 2);
  assert(leave_count_hashmap_get(hm, capacity * 3) == 3);
  assert(leave_count_hashmap_get(hm, capacity * 4) == 4);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == 6);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == 9);
  assert(leave_count_hashmap_get_num_entries(hm) == 9);

  leave_count_hashmap_delete(hm, capacity * 9);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == 2);
  assert(leave_count_hashmap_get(hm, capacity * 3) == 3);
  assert(leave_count_hashmap_get(hm, capacity * 4) == 4);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == 6);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 8);

  leave_count_hashmap_delete(hm, capacity * 6);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == 2);
  assert(leave_count_hashmap_get(hm, capacity * 3) == 3);
  assert(leave_count_hashmap_get(hm, capacity * 4) == 4);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 7);

  leave_count_hashmap_delete(hm, capacity * 3);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == 2);
  assert(leave_count_hashmap_get(hm, capacity * 3) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 4) == 4);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 6);

  leave_count_hashmap_delete(hm, capacity * 4);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == 2);
  assert(leave_count_hashmap_get(hm, capacity * 3) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 4) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 5);

  leave_count_hashmap_delete(hm, capacity * 2);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == 1);
  assert(leave_count_hashmap_get(hm, capacity * 2) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 3) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 4) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == 7);
  assert(leave_count_hashmap_get(hm, capacity * 8) == 8);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 4);

  leave_count_hashmap_delete(hm, capacity * 1);
  leave_count_hashmap_delete(hm, capacity * 7);
  leave_count_hashmap_delete(hm, capacity * 8);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 2) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 3) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 4) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 5) == 5);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 8) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 1);

  leave_count_hashmap_delete(hm, capacity * 5);
  assert(leave_count_hashmap_get(hm, 0) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 2) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 3) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 4) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 5) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 6) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 7) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 8) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get(hm, capacity * 9) == UNSET_KEY_VALUE);
  assert(leave_count_hashmap_get_num_entries(hm) == 0);

  leave_count_hashmap_delete(hm, capacity * 1);
  leave_count_hashmap_delete(hm, capacity * 2);
  leave_count_hashmap_delete(hm, capacity * 3);
  leave_count_hashmap_delete(hm, capacity * 4);
  leave_count_hashmap_delete(hm, capacity * 5);
  leave_count_hashmap_delete(hm, 0);
  leave_count_hashmap_delete(hm, 0);
  leave_count_hashmap_delete(hm, 0);
  leave_count_hashmap_delete(hm, 0);
  leave_count_hashmap_delete(hm, 0);
  leave_count_hashmap_delete(hm, 0);

  assert(leave_count_hashmap_get_num_entries(hm) == 0);

  for (uint64_t i = 0; i < capacity * num_cycles; i++) {
    leave_count_hashmap_set(hm, i % capacity, i * 11);
    assert(leave_count_hashmap_get(hm, i % capacity) == i * 11);
  }

  for (uint64_t i = capacity * (num_cycles - 1); i < capacity * num_cycles;
       i++) {
    assert(leave_count_hashmap_get(hm, i % capacity) == i * 11);
  }

  leave_count_hashmap_destroy(hm);
}
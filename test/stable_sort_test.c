#include "stable_sort_test.h"

#include "../src/ent/xoshiro.h"
#include "../src/util/io_util.h"
#include "../src/util/stable_sort.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct KeyedItem {
  int key;
  int position;
} KeyedItem;

static int compare_keyed_items_by_key(const void *a, const void *b) {
  const int first = ((const KeyedItem *)a)->key;
  const int second = ((const KeyedItem *)b)->key;
  return (first > second) - (first < second);
}

// Sorts count items whose keys take only key_range values, so most of them
// tie, and checks that the keys ascend and that equal keys keep the order
// they started in.
static void assert_sorts_stably(XoshiroPRNG *prng, int count, int key_range) {
  KeyedItem *items = malloc_or_die(sizeof(KeyedItem) * (size_t)(count + 1));
  for (int item_idx = 0; item_idx < count; item_idx++) {
    items[item_idx].key = (int)prng_get_random_number(prng, key_range);
    items[item_idx].position = item_idx;
  }
  stable_sort(items, (size_t)count, sizeof(KeyedItem),
              compare_keyed_items_by_key);
  for (int item_idx = 1; item_idx < count; item_idx++) {
    const KeyedItem *previous = &items[item_idx - 1];
    const KeyedItem *current = &items[item_idx];
    assert(previous->key <= current->key);
    if (previous->key == current->key) {
      assert(previous->position < current->position);
    }
  }
  free(items);
}

void test_stable_sort(void) {
  XoshiroPRNG *prng = prng_create(42);
  // Sizes on both sides of the stack scratch limit, and all small ones.
  for (int count = 0; count <= 70; count++) {
    assert_sorts_stably(prng, count, 3);
  }
  assert_sorts_stably(prng, 200, 5);
  assert_sorts_stably(prng, 5000, 7);
  assert_sorts_stably(prng, 5000, 5000);
  assert_sorts_stably(prng, 5000, 1);
  prng_destroy(prng);
}

#include "stable_sort_test.h"

#include "../src/ent/xoshiro.h"
#include "../src/util/io_util.h"
#include "../src/util/stable_sort.h"
#include "test_platform.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Where an element of any size keeps its key and its starting position.
  ELEMENT_KEY_OFFSET = 0,
  ELEMENT_POSITION_OFFSET = sizeof(int),
  ELEMENT_HEADER_BYTES = 2 * sizeof(int),
};

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

static int element_key(const void *element) {
  int key;
  memcpy(&key, (const char *)element + ELEMENT_KEY_OFFSET, sizeof(key));
  return key;
}

// Rock, paper, scissors: each key beats the one below it, and the lowest
// beats the highest, so the order is not transitive, and equal keys tie --
// as compare_simmed_plays can be. A sort's result under it depends on the
// exact comparisons the sort makes.
static int compare_rock_paper_scissors(const void *a, const void *b) {
  const int first = element_key(a);
  const int second = element_key(b);
  if (first == second) {
    return 0;
  }
  return (first - second + 3) % 3 == 1 ? 1 : -1;
}

// Sorts the same `count` elements of `size` bytes with qsort and stable_sort
// and checks that the results are byte for byte the same.
static void assert_sorts_like_glibc(XoshiroPRNG *prng, size_t count,
                                    size_t size) {
  unsigned char *original = malloc_or_die(count * size + 1);
  for (size_t element_idx = 0; element_idx < count; element_idx++) {
    unsigned char *element = original + element_idx * size;
    const int key = (int)prng_get_random_number(prng, 3);
    const int position = (int)element_idx;
    memcpy(element + ELEMENT_KEY_OFFSET, &key, sizeof(key));
    memcpy(element + ELEMENT_POSITION_OFFSET, &position, sizeof(position));
    for (size_t byte_idx = ELEMENT_HEADER_BYTES; byte_idx < size; byte_idx++) {
      element[byte_idx] = (unsigned char)(element_idx * 31 + byte_idx);
    }
  }
  unsigned char *by_qsort = malloc_or_die(count * size + 1);
  unsigned char *by_stable_sort = malloc_or_die(count * size + 1);
  memcpy(by_qsort, original, count * size);
  memcpy(by_stable_sort, original, count * size);
  qsort(by_qsort, count, size, compare_rock_paper_scissors);
  stable_sort(by_stable_sort, count, size, compare_rock_paper_scissors);
  assert(memcmp(by_qsort, by_stable_sort, count * size) == 0);
  free(by_stable_sort);
  free(by_qsort);
  free(original);
}

// stable_sort is glibc's merge sort step for step, so on glibc it gives what
// qsort gives even for a comparator that is not transitive, which is what
// keeps derived files and search order on Linux as they were. Element sizes
// cover glibc's word-sized copies, its byte copies, and its indirect sort of
// pointers for elements over 32 bytes; counts cover its scratch on the stack
// and on the heap.
static void test_stable_sort_matches_glibc(XoshiroPRNG *prng) {
  if (!test_platform_qsort_is_glibc_merge_sort()) {
    printf("skipping the comparison with qsort: it is not glibc's merge "
           "sort here\n");
    return;
  }
  static const size_t sizes[] = {8, 12, 16, 24, 32, 40, 64, 100};
  static const size_t counts[] = {2, 3, 7, 33, 100, 257, 1000};
  for (size_t size_idx = 0; size_idx < sizeof(sizes) / sizeof(sizes[0]);
       size_idx++) {
    for (size_t count_idx = 0; count_idx < sizeof(counts) / sizeof(counts[0]);
         count_idx++) {
      assert_sorts_like_glibc(prng, counts[count_idx], sizes[size_idx]);
    }
  }
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
  test_stable_sort_matches_glibc(prng);
  prng_destroy(prng);
}

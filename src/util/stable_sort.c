#include "stable_sort.h"

#include "io_util.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Scratch space up to this size lives on the stack, as in glibc.
  STABLE_SORT_STACK_BYTES = 1024,
};

typedef struct StableSort {
  size_t size;
  int (*compare)(const void *, const void *);
  char *scratch;
} StableSort;

// Merge sorts count elements at base. Splits like glibc's msort_with_tmp
// (the left half is the smaller one), and merges taking the left element
// while it compares less than or equal to the right one, so equal elements
// keep their order and the sequence of comparisons matches glibc's.
static void stable_sort_range(const StableSort *sort, char *base,
                              size_t count) {
  if (count <= 1) {
    return;
  }
  const size_t size = sort->size;
  size_t left_count = count / 2;
  size_t right_count = count - left_count;
  char *left = base;
  char *right = base + left_count * size;
  stable_sort_range(sort, left, left_count);
  stable_sort_range(sort, right, right_count);
  char *merged = sort->scratch;
  while (left_count > 0 && right_count > 0) {
    if (sort->compare(left, right) <= 0) {
      memcpy(merged, left, size);
      left += size;
      left_count--;
    } else {
      memcpy(merged, right, size);
      right += size;
      right_count--;
    }
    merged += size;
  }
  if (left_count > 0) {
    memcpy(merged, left, left_count * size);
  }
  // Whatever remains of the right half is already in place.
  memcpy(base, sort->scratch, (count - right_count) * size);
}

void stable_sort(void *base, size_t count, size_t size,
                 int (*compare)(const void *, const void *)) {
  if (count <= 1 || size == 0) {
    return;
  }
  const size_t scratch_bytes = count * size;
  uint64_t stack_scratch[STABLE_SORT_STACK_BYTES / sizeof(uint64_t)];
  char *heap_scratch = NULL;
  StableSort sort = {.size = size, .compare = compare};
  if (scratch_bytes <= sizeof(stack_scratch)) {
    sort.scratch = (char *)stack_scratch;
  } else {
    heap_scratch = malloc_or_die(scratch_bytes);
    sort.scratch = heap_scratch;
  }
  stable_sort_range(&sort, (char *)base, count);
  free(heap_scratch);
}

#include "anchor.h"

#include <stdlib.h>

#include "../def/board_defs.h"

#include "../util/util.h"

Anchor *anchor_create() { return malloc_or_die(sizeof(Anchor)); }

AnchorList *anchor_list_create(int board_area) {
  AnchorList *al = malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->capacity = board_area;
  al->anchors = malloc_or_die((sizeof(Anchor *)) * al->capacity);
  for (int i = 0; i < al->capacity; i++) {
    al->anchors[i] = anchor_create();
  }
  return al;
}

void anchor_destroy(Anchor *anchor) {
  if (!anchor) {
    return;
  }
  free(anchor);
}

void anchor_list_destroy(AnchorList *al) {
  if (!al) {
    return;
  }
  for (int i = 0; i < al->capacity; i++) {
    anchor_destroy(al->anchors[i]);
  }
  free(al->anchors);
  free(al);
}

int anchor_compare(const void *a, const void *b) {
  const Anchor *anchor_a = *(const Anchor **)a;
  const Anchor *anchor_b = *(const Anchor **)b;
  if (anchor_a->highest_possible_equity > anchor_b->highest_possible_equity) {
    return -1;
  } else if (anchor_a->highest_possible_equity <
             anchor_b->highest_possible_equity) {
    return 1;
  } else {
    return 0;
  }
}

void anchor_list_sort(AnchorList *al) {
  qsort(al->anchors, al->count, sizeof(Anchor *), anchor_compare);
}

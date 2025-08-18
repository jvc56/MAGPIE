#ifndef ALIAS_METHOD_H
#define ALIAS_METHOD_H

#include "../util/io_util.h"
#include "encoded_rack.h"
#include "rack.h"
#include "xoshiro.h"
#include <stdint.h>
#include <stdlib.h>

#define INITIAL_AM_ENTRIES_SIZE 1024

typedef struct AliasMethodItem {
  EncodedRack rack;
  uint32_t count;
  uint32_t alias;
  uint32_t overfull_item_index;
  uint32_t underfull_item_index;
  double probability;
} AliasMethodItem;

typedef struct AliasMethod {
  AliasMethodItem *items;
  uint32_t num_items;
  uint32_t capacity;
  uint64_t total_item_count;
} AliasMethod;

static inline AliasMethod *alias_method_create(void) {
  AliasMethod *am = (AliasMethod *)malloc_or_die(sizeof(AliasMethod));
  am->capacity = INITIAL_AM_ENTRIES_SIZE;
  am->items =
      (AliasMethodItem *)malloc_or_die(sizeof(AliasMethodItem) * am->capacity);
  am->num_items = 0;
  am->total_item_count = 0;
  return am;
}

static inline void alias_method_destroy(AliasMethod *am) {
  free(am->items);
  free(am);
}

static inline void alias_method_reset(AliasMethod *am) {
  am->num_items = 0;
  am->total_item_count = 0;
}

static inline void alias_method_add_rack(AliasMethod *am, Rack *rack,
                                         int count) {
  if (am->num_items == am->capacity) {
    am->capacity *= 2;
    am->items = (AliasMethodItem *)realloc_or_die(
        am->items, sizeof(AliasMethodItem) * am->capacity);
  }
  AliasMethodItem *item = &am->items[am->num_items];
  rack_encode(rack, &item->rack);
  item->count = (uint32_t)count;
  am->total_item_count += (uint64_t)count;
  am->num_items++;
}

static inline void alias_method_generate_tables(AliasMethod *am) {
  if (am->num_items == 0) {
    log_fatal("cannot generate tables for an empty alias method");
  }
  if (am->total_item_count == 0) {
    log_fatal(
        "cannot generate tables for an alias method with a total count of 0");
  }

  uint32_t num_overfull_items = 0;
  uint32_t num_underfull_items = 0;

  // Scale probabilities and separate into small/large queues
  double n = (double)am->num_items;
  for (uint32_t i = 0; i < am->num_items; i++) {
    double prob = (double)am->items[i].count / (double)am->total_item_count;
    am->items[i].probability = prob * n;
    if (am->items[i].probability > 1.0) {
      am->items[num_overfull_items++].overfull_item_index = i;
    } else {
      am->items[num_underfull_items++].underfull_item_index = i;
    }
  }

  // Process pairs from small and large lists
  while (num_overfull_items > 0 && num_underfull_items > 0) {
    num_overfull_items--;
    num_underfull_items--;
    uint32_t overfull_item_index =
        am->items[num_overfull_items].overfull_item_index;
    uint32_t underfull_item_index =
        am->items[num_underfull_items].underfull_item_index;

    // Set up the alias relationship
    am->items[underfull_item_index].alias = overfull_item_index;

    // Update the large item's scaled probability
    am->items[overfull_item_index].probability =
        am->items[overfull_item_index].probability +
        am->items[underfull_item_index].probability - 1.0;

    // Reclassify the large item
    if (am->items[overfull_item_index].probability > 1.0) {
      am->items[num_overfull_items++].overfull_item_index = overfull_item_index;
    } else {
      am->items[num_underfull_items++].underfull_item_index =
          overfull_item_index;
    }
  }

  while (num_overfull_items) {
    num_overfull_items--;
    uint32_t overfull_item_index =
        am->items[num_overfull_items].overfull_item_index;
    am->items[overfull_item_index].probability = 1.0;
    am->items[overfull_item_index].alias = overfull_item_index;
  }

  while (num_underfull_items) {
    num_underfull_items--;
    uint32_t underfull_item_index =
        am->items[num_underfull_items].underfull_item_index;
    am->items[underfull_item_index].probability = 1.0;
    am->items[underfull_item_index].alias = underfull_item_index;
  }
}

static inline void alias_method_sample(AliasMethod *am, XoshiroPRNG *prng,
                                       Rack *rack_to_update) {
  if (am->num_items == 0) {
    log_fatal("cannot sample from an empty alias method");
  }

  // Choose a random bin
  const uint32_t bin = (uint32_t)prng_get_random_number(prng, am->num_items);
  const double rand_between_0_and_1 =
      (double)prng_get_random_number(prng, XOSHIRO_MAX) / XOSHIRO_MAX;

  uint32_t chosen_idx;
  if (rand_between_0_and_1 < am->items[bin].probability) {
    chosen_idx = bin;
  } else {
    chosen_idx = am->items[bin].alias;
  }
  rack_decode(&am->items[chosen_idx].rack, rack_to_update);
}

#endif
#ifndef ALIAS_METHOD_H
#define ALIAS_METHOD_H

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../util/io_util.h"
#include "encoded_rack.h"
#include "rack.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
  // For a table of the racks kept by an exchange, the tiles each item's
  // exchange threw back, by item index. Only alias_method_add_exchange and
  // alias_method_add_weighted_exchange write them, and a table holds them for
  // all of its items or for none.
  EncodedRack *exchanged_racks;
  uint32_t exchanged_racks_capacity;
  bool has_exchanged_racks;
  uint32_t num_items;
  uint32_t capacity;
  uint64_t total_item_count;
  bool use_weights;
  cpthread_mutex_t mutex;
} AliasMethod;

static inline AliasMethod *alias_method_create(void) {
  AliasMethod *am = (AliasMethod *)malloc_or_die(sizeof(AliasMethod));
  am->capacity = INITIAL_AM_ENTRIES_SIZE;
  am->items =
      (AliasMethodItem *)malloc_or_die(sizeof(AliasMethodItem) * am->capacity);
  am->exchanged_racks = NULL;
  am->exchanged_racks_capacity = 0;
  am->has_exchanged_racks = false;
  am->num_items = 0;
  am->total_item_count = 0;
  am->use_weights = false;
  cpthread_mutex_init(&am->mutex);
  return am;
}

static inline void alias_method_destroy(AliasMethod *am) {
  if (!am) {
    return;
  }
  free(am->items);
  free(am->exchanged_racks);
  free(am);
}

static inline void alias_method_reset(AliasMethod *am) {
  am->num_items = 0;
  am->total_item_count = 0;
  am->use_weights = false;
  am->has_exchanged_racks = false;
}

static inline bool alias_method_has_exchanged_racks(const AliasMethod *am) {
  return am->has_exchanged_racks;
}

// Appends an item with the count, for the caller to give a rack, and returns
// its index. The caller holds the mutex.
static inline uint32_t alias_method_append_item(AliasMethod *am, int count) {
  if (am->num_items == am->capacity) {
    am->capacity *= 2;
    am->items = (AliasMethodItem *)realloc_or_die(
        am->items, sizeof(AliasMethodItem) * am->capacity);
  }
  const uint32_t new_item_index = am->num_items;
  am->total_item_count += (uint64_t)count;
  am->num_items++;
  am->items[new_item_index].count = (uint32_t)count;
  return new_item_index;
}

// Records the tiles thrown back by the exchange of the item just appended at
// item_index. The caller holds the mutex. Until the tables are generated, the
// item's alias is the index its exchanged tiles are stored at, which is how
// they follow the item through the sort.
static inline void
alias_method_set_exchanged_rack(AliasMethod *am, uint32_t item_index,
                                const EncodedRack *exchanged) {
  if (am->exchanged_racks_capacity < am->capacity) {
    am->exchanged_racks = (EncodedRack *)realloc_or_die(
        am->exchanged_racks, sizeof(EncodedRack) * am->capacity);
    am->exchanged_racks_capacity = am->capacity;
  }
  am->has_exchanged_racks = true;
  am->exchanged_racks[item_index] = *exchanged;
  am->items[item_index].alias = item_index;
}

// Thread safe
static inline void alias_method_add_rack(AliasMethod *am, const Rack *rack,
                                         int count) {
  cpthread_mutex_lock(&am->mutex);
  const uint32_t new_item_index = alias_method_append_item(am, count);
  rack_encode(rack, &am->items[new_item_index].rack);
  cpthread_mutex_unlock(&am->mutex);
}

// Thread safe. As alias_method_add_rack, for the rack an exchange kept and
// the tiles it threw back. Both are encoded before the mutex is taken: an
// exchange with most tiles unseen adds millions of items from every thread.
static inline void alias_method_add_exchange(AliasMethod *am, const Rack *rack,
                                             const Rack *exchanged, int count) {
  EncodedRack encoded_rack;
  rack_encode(rack, &encoded_rack);
  EncodedRack encoded_exchanged;
  rack_encode(exchanged, &encoded_exchanged);
  cpthread_mutex_lock(&am->mutex);
  const uint32_t new_item_index = alias_method_append_item(am, count);
  am->items[new_item_index].rack = encoded_rack;
  alias_method_set_exchanged_rack(am, new_item_index, &encoded_exchanged);
  cpthread_mutex_unlock(&am->mutex);
}

// Thread safe. Stores the unnormalized weight in item->probability before
// alias_method_generate_tables normalizes it, avoiding any extra field on
// AliasMethodItem or extra stores on the unweighted path.
static inline void alias_method_add_weighted_rack(AliasMethod *am,
                                                  const Rack *rack, int count,
                                                  double weight) {
  cpthread_mutex_lock(&am->mutex);
  const uint32_t new_item_index = alias_method_append_item(am, count);
  rack_encode(rack, &am->items[new_item_index].rack);
  am->use_weights = true;
  am->items[new_item_index].probability = weight;
  cpthread_mutex_unlock(&am->mutex);
}

// Thread safe. As alias_method_add_weighted_rack, for the rack an exchange
// kept and the tiles it threw back, both encoded before the mutex is taken
// (see alias_method_add_exchange).
static inline void alias_method_add_weighted_exchange(AliasMethod *am,
                                                      const Rack *rack,
                                                      const Rack *exchanged,
                                                      int count,
                                                      double weight) {
  EncodedRack encoded_rack;
  rack_encode(rack, &encoded_rack);
  EncodedRack encoded_exchanged;
  rack_encode(exchanged, &encoded_exchanged);
  cpthread_mutex_lock(&am->mutex);
  const uint32_t new_item_index = alias_method_append_item(am, count);
  am->items[new_item_index].rack = encoded_rack;
  am->use_weights = true;
  am->items[new_item_index].probability = weight;
  alias_method_set_exchanged_rack(am, new_item_index, &encoded_exchanged);
  cpthread_mutex_unlock(&am->mutex);
}

static inline int alias_method_compare_items(const void *a, const void *b) {
  const AliasMethodItem *item_a = (const AliasMethodItem *)a;
  const AliasMethodItem *item_b = (const AliasMethodItem *)b;
  const int rack_order =
      memcmp(&item_a->rack, &item_b->rack, sizeof(item_a->rack));
  if (rack_order != 0) {
    return rack_order;
  }
  return (item_a->count > item_b->count) - (item_a->count < item_b->count);
}

static inline int alias_method_compare_weighted_items(const void *a,
                                                      const void *b) {
  const AliasMethodItem *item_a = (const AliasMethodItem *)a;
  const AliasMethodItem *item_b = (const AliasMethodItem *)b;
  const int rack_order =
      memcmp(&item_a->rack, &item_b->rack, sizeof(item_a->rack));
  if (rack_order != 0) {
    return rack_order;
  }
  if (item_a->probability != item_b->probability) {
    return (item_a->probability > item_b->probability) -
           (item_a->probability < item_b->probability);
  }
  return (item_a->count > item_b->count) - (item_a->count < item_b->count);
}

static inline int alias_method_compare_exchanged_racks(const void *a,
                                                       const void *b) {
  return memcmp(a, b, sizeof(EncodedRack));
}

// Puts the exchanged racks in the order the sort left their items in. Items
// that compare equal are interchangeable but for their exchanged racks, which
// are put in ascending order, so that the order of the exchanged racks does
// not depend on the order the items were added in either. It reads the index
// each item was added at from the item's alias, so it runs once, before the
// tables are built.
static inline void alias_method_sort_exchanged_racks(
    AliasMethod *am, int (*compare_items)(const void *, const void *)) {
  EncodedRack *sorted_racks = (EncodedRack *)malloc_or_die(
      sizeof(EncodedRack) * am->exchanged_racks_capacity);
  for (uint32_t item_idx = 0; item_idx < am->num_items; item_idx++) {
    sorted_racks[item_idx] = am->exchanged_racks[am->items[item_idx].alias];
  }
  free(am->exchanged_racks);
  am->exchanged_racks = sorted_racks;
  uint32_t run_start_idx = 0;
  for (uint32_t item_idx = 1; item_idx <= am->num_items; item_idx++) {
    if (item_idx < am->num_items &&
        compare_items(&am->items[run_start_idx], &am->items[item_idx]) == 0) {
      continue;
    }
    if (item_idx - run_start_idx > 1) {
      qsort(&sorted_racks[run_start_idx], item_idx - run_start_idx,
            sizeof(EncodedRack), alias_method_compare_exchanged_racks);
    }
    run_start_idx = item_idx;
  }
}

// Returns true if there are a nonzero number of items and counts to generate
// tables for and returns false otherwise.
static inline bool alias_method_generate_tables(AliasMethod *am) {
  if (am->num_items == 0 || am->total_item_count == 0) {
    return false;
  }

  // Sort the items (by rack, then weight/count) so the tables, and the racks a
  // seeded sample draws, do not depend on the order the items were added in:
  // inference threads add them in whatever order they finish.
  int (*compare_items)(const void *, const void *) =
      am->use_weights ? alias_method_compare_weighted_items
                      : alias_method_compare_items;
  qsort(am->items, am->num_items, sizeof(AliasMethodItem), compare_items);
  if (am->has_exchanged_racks) {
    alias_method_sort_exchanged_racks(am, compare_items);
  }

  uint32_t num_overfull_items = 0;
  uint32_t num_underfull_items = 0;

  // Scale probabilities and separate into small/large queues
  double n = (double)am->num_items;
  if (am->use_weights) {
    double total_weight = 0.0;
    for (uint32_t i = 0; i < am->num_items; i++) {
      total_weight += am->items[i].probability;
    }
    if (total_weight <= 0.0) {
      // No representable posterior mass: sampling must report failure.
      am->num_items = 0;
      return false;
    }
    for (uint32_t i = 0; i < am->num_items; i++) {
      double prob = am->items[i].probability / total_weight;
      am->items[i].probability = prob * n;
      if (am->items[i].probability > 1.0) {
        am->items[num_overfull_items++].overfull_item_index = i;
      } else {
        am->items[num_underfull_items++].underfull_item_index = i;
      }
    }
  } else {
    for (uint32_t i = 0; i < am->num_items; i++) {
      double prob = (double)am->items[i].count / (double)am->total_item_count;
      am->items[i].probability = prob * n;
      if (am->items[i].probability > 1.0) {
        am->items[num_overfull_items++].overfull_item_index = i;
      } else {
        am->items[num_underfull_items++].underfull_item_index = i;
      }
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
  return true;
}

// Samples an item and sets item_index to its index. Returns true if there are
// a nonzero number of items and counts to sample from and returns false
// otherwise.
static inline bool alias_method_sample_item_index(const AliasMethod *am,
                                                  XoshiroPRNG *prng,
                                                  uint32_t *item_index) {
  if (am->num_items == 0 || am->total_item_count == 0) {
    return false;
  }

  const uint32_t bin = (uint32_t)prng_get_random_number(prng, am->num_items);
  const double rand_between_0_and_1 =
      (double)prng_get_random_number(prng, XOSHIRO_MAX) / (double)XOSHIRO_MAX;

  if (rand_between_0_and_1 < am->items[bin].probability) {
    *item_index = bin;
  } else {
    *item_index = am->items[bin].alias;
  }
  return true;
}

// Returns true if there are a nonzero number of items and counts to sample from
// and returns false otherwise.
static inline bool alias_method_sample(AliasMethod *am, XoshiroPRNG *prng,
                                       Rack *rack_to_update) {
  uint32_t chosen_idx;
  if (!alias_method_sample_item_index(am, prng, &chosen_idx)) {
    return false;
  }
  rack_decode(&am->items[chosen_idx].rack, rack_to_update);
  return true;
}

// As alias_method_sample for a table of exchanges (see
// alias_method_has_exchanged_racks): the same item for the same PRNG state,
// with exchanged_to_update set to the tiles its exchange threw back.
static inline bool alias_method_sample_exchange(const AliasMethod *am,
                                                XoshiroPRNG *prng,
                                                Rack *rack_to_update,
                                                Rack *exchanged_to_update) {
  uint32_t chosen_idx;
  if (!alias_method_sample_item_index(am, prng, &chosen_idx)) {
    return false;
  }
  rack_decode(&am->items[chosen_idx].rack, rack_to_update);
  rack_decode(&am->exchanged_racks[chosen_idx], exchanged_to_update);
  return true;
}

#endif
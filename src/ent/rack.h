#ifndef RACK_H
#define RACK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/rack_defs.h"

#include "letter_distribution.h"

#include "../util/string_util.h"
#include "../util/util.h"

typedef struct Rack {
  int dist_size;
  int array[MAX_ALPHABET_SIZE];
  int number_of_letters;
} Rack;

static inline void rack_reset(Rack *rack) {
  for (int i = 0; i < rack->dist_size; i++) {
    rack->array[i] = 0;
  }
  rack->number_of_letters = 0;
}

static inline Rack *rack_create(int dist_size) {
  Rack *rack = malloc_or_die(sizeof(Rack));
  rack->dist_size = dist_size;
  rack_reset(rack);
  return rack;
}

static inline void rack_copy(Rack *dst, const Rack *src) {
  memory_copy(dst, src, sizeof(Rack));
}

static inline void rack_set_dist_size(Rack *rack, int dist_size) {
  rack->dist_size = dist_size;
}

static inline Rack *rack_duplicate(const Rack *rack) {
  Rack *new_rack = rack_create(rack->dist_size);
  rack_copy(new_rack, rack);
  return new_rack;
}

static inline void rack_destroy(Rack *rack) {
  if (!rack) {
    return;
  }
  free(rack);
}

static inline int rack_get_dist_size(const Rack *rack) {
  return rack->dist_size;
}

static inline int rack_get_letter(const Rack *rack, uint8_t machine_letter) {
  return rack->array[machine_letter];
}

static inline void rack_set_letter(Rack *rack, uint8_t machine_letter,
                                   int count) {
  rack->array[machine_letter] = count;
}

static inline int rack_get_total_letters(const Rack *rack) {
  return rack->number_of_letters;
}

static inline void rack_set_total_letters(Rack *rack, int total_letters) {
  rack->number_of_letters = total_letters;
}

static inline bool rack_is_empty(const Rack *rack) {
  return rack->number_of_letters == 0;
}

static inline bool racks_are_equal(const Rack *rack1, const Rack *rack2) {
  if (!rack1 && !rack2) {
    return true;
  }
  if (!rack1 || !rack2 || rack1->dist_size != rack2->dist_size) {
    return false;
  }
  for (int i = 0; i < rack1->dist_size; i++) {
    if (rack1->array[i] != rack2->array[i]) {
      return false;
    }
  }
  return true;
}

// Returns true if rack_to_update contains value_to_sub
// and subtracts value_to_sub from rack_to_update
// on success.
static inline bool rack_subtract(Rack *rack_to_update,
                                 const Rack *value_to_sub) {
  for (int i = 0; i < rack_to_update->dist_size; i++) {
    if (rack_to_update->array[i] < value_to_sub->array[i]) {
      return false;
    }
    rack_to_update->array[i] -= value_to_sub->array[i];
    rack_to_update->number_of_letters -= value_to_sub->array[i];
  }
  return true;
}

static inline void rack_add(Rack *rack_to_update, const Rack *value_to_add) {
  for (int i = 0; i < rack_to_update->dist_size; i++) {
    rack_to_update->array[i] += value_to_add->array[i];
    rack_to_update->number_of_letters += value_to_add->array[i];
  }
}

static inline int rack_contains_subrack(const Rack *rack, const Rack *subrack) {
  const int dist_size = rack->dist_size;
  for (int i = 0; i < dist_size; i++) {
    if (subrack->array[i] > rack->array[i]) {
      return false;
    }
  }
  return true;
}

static inline void rack_take_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]--;
  rack->number_of_letters--;
}

static inline void rack_take_letters(Rack *rack, uint8_t letter, int count) {
  rack->array[letter] -= count;
  rack->number_of_letters -= count;
}

static inline void rack_add_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]++;
  rack->number_of_letters++;
}

static inline void rack_add_letters(Rack *rack, uint8_t letter, int count) {
  rack->array[letter] += count;
  rack->number_of_letters += count;
}

static inline int rack_set_to_string(const LetterDistribution *ld, Rack *rack,
                                     const char *rack_string) {
  rack_reset(rack);

  uint8_t mls[MAX_RACK_SIZE];
  int num_mls = ld_str_to_mls(ld, rack_string, false, mls, MAX_RACK_SIZE);
  for (int i = 0; i < num_mls; i++) {
    rack_add_letter(rack, mls[i]);
  }
  return num_mls;
}

// Get the sum of the tile values on the rack
static inline int rack_get_score(const LetterDistribution *ld,
                                 const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    sum += rack_get_letter(rack, i) * ld_get_score(ld, i);
  }
  return sum;
}

static inline Rack *get_new_bag_as_rack(const LetterDistribution *ld) {
  const int ld_size = ld_get_size(ld);
  Rack *bag_as_rack = rack_create(ld_size);
  for (int ml = 0; ml < ld_size; ml++) {
    rack_add_letters(bag_as_rack, ml, ld_get_dist(ld, ml));
  }
  return bag_as_rack;
}

static inline void rack_increment_unseen_count(const Rack *rack,
                                               int *unseen_count) {
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    unseen_count[i] += rack_get_letter(rack, i);
  }
}

#endif
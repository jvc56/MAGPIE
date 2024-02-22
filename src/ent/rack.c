#include "rack.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/rack_defs.h"

#include "letter_distribution.h"

#include "../util/util.h"

void rack_reset(Rack *rack) {
  for (int i = 0; i < rack->dist_size; i++) {
    rack->array[i] = 0;
  }
  rack->empty = true;
  rack->number_of_letters = 0;
}

Rack *rack_create(int dist_size) {
  Rack *rack = malloc_or_die(sizeof(Rack));
  rack->dist_size = dist_size;
  rack_reset(rack);
  return rack;
}

Rack *rack_duplicate(const Rack *rack) {
  Rack *new_rack = rack_create(rack->dist_size);
  rack_copy(new_rack, rack);
  return new_rack;
}

void rack_copy(Rack *dst, const Rack *src) {
  for (int i = 0; i < src->dist_size; i++) {
    dst->array[i] = src->array[i];
  }
  dst->number_of_letters = src->number_of_letters;
  dst->empty = src->empty;
}

void rack_destroy(Rack *rack) {
  if (!rack) {
    return;
  }
  free(rack);
}

int rack_get_score(const LetterDistribution *ld, const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    sum += rack_get_letter(rack, i) * ld_get_score(ld, i);
  }
  return sum;
}

// Returns true if rack_to_update contains value_to_sub
// and subtracts value_to_sub from rack_to_update
// on success.
// This function is not in the critical path, so
// we can leave it in the .c file unlike other rack functions.
bool rack_subtract(Rack *rack_to_update, Rack *value_to_sub) {
  for (int i = 0; i < rack_to_update->dist_size; i++) {
    if (rack_to_update->array[i] < value_to_sub->array[i]) {
      return false;
    }
    rack_to_update->array[i] -= value_to_sub->array[i];
    rack_to_update->number_of_letters -= value_to_sub->array[i];
    if (rack_to_update->number_of_letters == 0) {
      rack_to_update->empty = true;
    }
  }
  return true;
}

int rack_set_to_string(const LetterDistribution *ld, Rack *rack,
                       const char *rack_string) {
  rack_reset(rack);

  uint8_t mls[MAX_RACK_SIZE];
  int num_mls = ld_str_to_mls(ld, rack_string, false, mls, MAX_RACK_SIZE);
  for (int i = 0; i < num_mls; i++) {
    rack_add_letter(rack, mls[i]);
  }
  return num_mls;
}

bool racks_are_equal(const Rack *rack1, const Rack *rack2) {
  if (!rack1 && !rack2) {
    return true;
  }
  if (!rack1 || !rack2 || rack1->dist_size != rack2->dist_size ||
      rack1->empty != rack2->empty) {
    return false;
  }
  for (int i = 0; i < rack1->dist_size; i++) {
    if (rack1->array[i] != rack2->array[i]) {
      return false;
    }
  }
  return true;
}
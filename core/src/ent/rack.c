#include "rack.h"

#include <stdbool.h>
#include <stdint.h>

#include "../def/rack_defs.h"

#include "letter_distribution.h"

#include "../util/util.h"

struct Rack {
  int array_size;
  int *array;
  bool empty;
  int number_of_letters;
};

void rack_reset(Rack *rack) {
  for (int i = 0; i < (rack->array_size); i++) {
    rack->array[i] = 0;
  }
  rack->empty = true;
  rack->number_of_letters = 0;
}

Rack *rack_create(int array_size) {
  Rack *rack = malloc_or_die(sizeof(Rack));
  rack->array_size = array_size;
  rack->array = (int *)malloc_or_die(rack->array_size * sizeof(int));
  rack_reset(rack);
  return rack;
}

Rack *rack_duplicate(const Rack *rack) {
  Rack *new_rack = rack_create(rack->array_size);
  rack_copy(new_rack, rack);
  return new_rack;
}

void rack_copy(Rack *dst, const Rack *src) {
  for (int i = 0; i < src->array_size; i++) {
    dst->array[i] = src->array[i];
  }
  dst->number_of_letters = src->number_of_letters;
  dst->empty = src->empty;
}

void rack_destroy(Rack *rack) {
  if (!rack) {
    return;
  }
  free(rack->array);
  free(rack);
}

int rack_get_dist_size(const Rack *rack) { return rack->array_size; }

int rack_get_letter(const Rack *rack, uint8_t machine_letter) {
  return rack->array[machine_letter];
}

int rack_get_total_letters(const Rack *rack) { return rack->number_of_letters; }

bool rack_is_empty(const Rack *rack) { return rack->empty; }

int rack_get_score(const LetterDistribution *ld, const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    sum += rack_get_letter(rack, i) * ld_get_score(ld, i);
  }
  return sum;
}

void rack_take_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]--;
  rack->number_of_letters--;
  if (rack->number_of_letters == 0) {
    rack->empty = true;
  }
}

void rack_add_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]++;
  rack->number_of_letters++;
  if (rack->empty == 1) {
    rack->empty = false;
  }
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
  if (!rack1 || !rack2 || rack1->array_size != rack2->array_size ||
      rack1->empty != rack2->empty) {
    return false;
  }
  for (int i = 0; i < rack1->array_size; i++) {
    if (rack1->array[i] != rack2->array[i]) {
      return false;
    }
  }
  return true;
}
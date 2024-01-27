#ifndef RACK_H
#define RACK_H

#include <stdbool.h>
#include <stdint.h>

#include "letter_distribution.h"

typedef struct Rack {
  int array_size;
  int *array;
  bool empty;
  int number_of_letters;
} Rack;

Rack *rack_create(int array_size);
void rack_destroy(Rack *rack);
Rack *rack_duplicate(const Rack *rack);
void rack_copy(Rack *dst, const Rack *src);
void rack_reset(Rack *rack);

static inline int rack_get_dist_size(const Rack *rack) { return rack->array_size; }

static inline int rack_get_letter(const Rack *rack, uint8_t machine_letter) {
  return rack->array[machine_letter];
}

static inline int rack_get_total_letters(const Rack *rack) {
  return rack->number_of_letters;
}

static inline bool rack_is_empty(const Rack *rack) { return rack->empty; }

bool racks_are_equal(const Rack *rack1, const Rack *rack2);

static inline void rack_take_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]--;
  rack->number_of_letters--;
  if (rack->number_of_letters == 0) {
    rack->empty = true;
  }
}

static inline void rack_add_letter(Rack *rack, uint8_t letter) {
  rack->array[letter]++;
  rack->number_of_letters++;
  if (rack->empty == 1) {
    rack->empty = false;
  }
}

int rack_set_to_string(const LetterDistribution *ld, Rack *rack,
                       const char *rack_string);
int rack_get_score(const LetterDistribution *ld, const Rack *rack);

#endif
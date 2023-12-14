#include <stdbool.h>
#include <stdint.h>

#include "../def/rack_defs.h"

#include "../util/util.h"

#include "letter_distribution.h"
#include "rack.h"

struct Rack {
  int array_size;
  int *array;
  bool empty;
  int number_of_letters;
};

int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < get_array_size(rack); i++) {
    sum += get_number_of_letter(rack, i) *
           letter_distribution_get_score(letter_distribution, i);
  }
  return sum;
}

int get_array_size(const Rack *rack) { return rack->array_size; }

int get_number_of_letter(const Rack *rack, uint8_t machine_letter) {
  return rack->array[machine_letter];
}

int get_number_of_letters(const Rack *rack) { return rack->number_of_letters; }

bool rack_is_empty(const Rack *rack) { return rack->empty; }

void reset_rack(Rack *rack) {
  for (int i = 0; i < (rack->array_size); i++) {
    rack->array[i] = 0;
  }
  rack->empty = true;
  rack->number_of_letters = 0;
}

Rack *create_rack(int array_size) {
  Rack *rack = malloc_or_die(sizeof(Rack));
  rack->array_size = array_size;
  rack->array = (int *)malloc_or_die(rack->array_size * sizeof(int));
  reset_rack(rack);
  return rack;
}

void update_or_create_rack(Rack **rack, int array_size) {
  if (!(*rack)) {
    *rack = create_rack(array_size);
  } else if ((*rack)->array_size != array_size) {
    destroy_rack(*rack);
    *rack = create_rack(array_size);
  }
}

Rack *rack_duplicate(const Rack *rack) {
  Rack *new_rack = malloc_or_die(sizeof(Rack));
  new_rack->array = (int *)malloc_or_die(rack->array_size * sizeof(int));
  new_rack->array_size = rack->array_size;
  reset_rack(new_rack);
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

void destroy_rack(Rack *rack) {
  free(rack->array);
  free(rack);
}

void take_letter_from_rack(Rack *rack, uint8_t letter) {
  rack->array[letter]--;
  rack->number_of_letters--;
  if (rack->number_of_letters == 0) {
    rack->empty = true;
  }
}

void add_letter_to_rack(Rack *rack, uint8_t letter) {
  rack->array[letter]++;
  rack->number_of_letters++;
  if (rack->empty == 1) {
    rack->empty = false;
  }
}

int set_rack_to_string(const LetterDistribution *letter_distribution,
                       Rack *rack, const char *rack_string) {
  reset_rack(rack);

  uint8_t mls[MAX_RACK_SIZE];
  int num_mls = str_to_machine_letters(letter_distribution, rack_string, false,
                                       mls, MAX_RACK_SIZE);
  for (int i = 0; i < num_mls; i++) {
    add_letter_to_rack(rack, mls[i]);
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
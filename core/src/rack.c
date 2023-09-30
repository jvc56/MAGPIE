#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bag.h"
#include "letter_distribution.h"
#include "log.h"
#include "rack.h"
#include "string_util.h"
#include "util.h"

void reset_rack(Rack *rack) {
  for (int i = 0; i < (rack->array_size); i++) {
    rack->array[i] = 0;
  }
  rack->empty = 1;
  rack->number_of_letters = 0;
}

Rack *create_rack(int array_size) {
  Rack *rack = malloc_or_die(sizeof(Rack));
  rack->array_size = array_size;
  rack->array = (int *)malloc_or_die(rack->array_size * sizeof(int));
  reset_rack(rack);
  return rack;
}

Rack *copy_rack(Rack *rack) {
  Rack *new_rack = malloc_or_die(sizeof(Rack));
  new_rack->array = (int *)malloc_or_die(rack->array_size * sizeof(int));
  new_rack->array_size = rack->array_size;
  reset_rack(new_rack);
  copy_rack_into(new_rack, rack);
  return new_rack;
}

void copy_rack_into(Rack *dst, Rack *src) {
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
    rack->empty = 1;
  }
}

void add_letter_to_rack(Rack *rack, uint8_t letter) {
  rack->array[letter]++;
  rack->number_of_letters++;
  if (rack->empty == 1) {
    rack->empty = 0;
  }
}

int score_on_rack(LetterDistribution *letter_distribution, Rack *rack) {
  int sum = 0;
  for (int i = 0; i < (rack->array_size); i++) {
    sum += rack->array[i] * letter_distribution->scores[i];
  }
  return sum;
}

void set_rack_to_string(Rack *rack, const char *rack_string,
                        LetterDistribution *letter_distribution) {
  reset_rack(rack);

  uint8_t mls[BAG_SIZE];
  int num_mls =
      str_to_machine_letters(letter_distribution, rack_string, false, mls);
  if (num_mls > BAG_SIZE) {
    log_fatal("rack contains more letters than the bag: %d\n", num_mls);
  }
  for (int i = 0; i < num_mls; i++) {
    add_letter_to_rack(rack, mls[i]);
  }
}

void write_rack(Rack *rack, LetterDistribution *letter_distribution,
                char *rack_string) {
  char *rp = rack_string;
  for (int i = 0; i < rack->array_size; i++) {
    for (int j = 0; j < rack->array[i]; j++) {
      char tile[MAX_LETTER_CHAR_LENGTH];
      machine_letter_to_human_readable_letter(letter_distribution, i, tile);
      rp += sprintf(rp, "%s", tile);
    }
  }
}

void string_builder_add_rack(Rack *rack,
                             LetterDistribution *letter_distribution,
                             StringBuilder *string_builder) {
  char rack_string[MAX_LETTER_CHAR_LENGTH * BAG_SIZE] = "";
  write_rack(rack, letter_distribution, rack_string);
  string_builder_add_string(string_builder, rack_string, 0);
}

bool racks_are_equal(Rack *rack1, Rack *rack2) {
  if (rack1 == NULL && rack2 == NULL) {
    return true;
  }
  if (rack1 == NULL || rack2 == NULL ||
      rack1->array_size != rack2->array_size || rack1->empty != rack2->empty) {
    return false;
  }
  for (int i = 0; i < rack1->array_size; i++) {
    if (rack1->array[i] != rack2->array[i]) {
      return false;
    }
  }
  return true;
}
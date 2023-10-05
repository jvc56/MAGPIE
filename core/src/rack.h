#ifndef RACK_H
#define RACK_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "letter_distribution.h"

#define RACK_SIZE 7

typedef struct Rack {
  int array_size;
  int *array;
  int empty;
  int number_of_letters;
} Rack;

void add_letter_to_rack(Rack *rack, uint8_t letter);
Rack *create_rack(int array_size);
Rack *copy_rack(Rack *rack);
void copy_rack_into(Rack *dst, Rack *src);
void destroy_rack(Rack *rack);
void reset_rack(Rack *rack);
int score_on_rack(LetterDistribution *letter_distribution, Rack *rack);
int set_rack_to_string(Rack *rack, const char *rack_string,
                       LetterDistribution *letter_distribution);
void take_letter_from_rack(Rack *rack, uint8_t letter);
bool racks_are_equal(Rack *rack1, Rack *rack2);
void string_builder_add_rack(Rack *rack,
                             LetterDistribution *letter_distribution,
                             StringBuilder *string_builder);
#endif
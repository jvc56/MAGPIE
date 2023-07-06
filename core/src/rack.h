#ifndef RACK_H
#define RACK_H

#include <stdint.h>

#include "constants.h"
#include "letter_distribution.h"

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
void rack_to_string(Rack *rack, char *rack_string, LetterDistribution *letter_distribution);
void reset_rack(Rack *rack);
int score_on_rack(LetterDistribution *letter_distribution, Rack *rack);
void set_rack_to_string(Rack *rack, const char *rack_string,
                        LetterDistribution *letter_distribution);
void take_letter_from_rack(Rack *rack, uint8_t letter);

#endif
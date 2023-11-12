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
  bool empty;
  int number_of_letters;
} Rack;

void add_letter_to_rack(Rack *rack, uint8_t letter);
Rack *create_rack(int array_size);
void update_or_create_rack(Rack **rack, int array_size);
Rack *copy_rack(const Rack *rack);
void copy_rack_into(Rack *dst, const Rack *src);
void destroy_rack(Rack *rack);
void reset_rack(Rack *rack);
int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack);
int set_rack_to_string(Rack *rack, const char *rack_string,
                       const LetterDistribution *letter_distribution);
void take_letter_from_rack(Rack *rack, uint8_t letter);
bool racks_are_equal(const Rack *rack1, const Rack *rack2);
void string_builder_add_rack(const Rack *rack,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder);
#endif
#ifndef RACK_H
#define RACK_H

#include "letter_distribution.h"

struct Rack;
typedef struct Rack Rack;

void add_letter_to_rack(Rack *rack, uint8_t letter);
Rack *create_rack(int array_size);
Rack *rack_duplicate(const Rack *rack);
void rack_copy(Rack *dst, const Rack *src);
void destroy_rack(Rack *rack);
int get_array_size(const Rack *rack);
int get_number_of_letter(const Rack *rack, uint8_t machine_letter);
void reset_rack(Rack *rack);
void take_letter_from_rack(Rack *rack, uint8_t letter);
bool racks_are_equal(const Rack *rack1, const Rack *rack2);
bool rack_is_empty(const Rack *rack);
int get_number_of_letters(const Rack *rack);

int set_rack_to_string(const LetterDistribution *letter_distribution,
                       Rack *rack, const char *rack_string);
int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack);

#endif
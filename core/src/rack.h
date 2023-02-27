#ifndef RACK_H
#define RACK_H

#include <stdint.h>

#include "alphabet.h"
#include "constants.h"
#include "letter_distribution.h"

typedef struct Rack {
    int array[(RACK_ARRAY_SIZE)];
    int letter_to_array_nonzero_index[(RACK_ARRAY_SIZE)];
    int array_nonzero_indexes[(RACK_SIZE)];
    int number_of_nonzero_indexes;
    int empty;
    int number_of_letters;
} Rack;

void add_letter_to_rack(Rack * rack, uint8_t letter, int nonzero_array_index);
Rack * create_rack();
void destroy_rack(Rack * rack);
void reset_rack(Rack * rack);
int score_on_rack(LetterDistribution * letter_distribution, Rack * rack);
void set_rack_to_string(Rack * rack, const char* rack_string, Alphabet * alphabet);
int take_letter_from_rack(Rack * rack, uint8_t letter);

#endif
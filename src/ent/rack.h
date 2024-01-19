#ifndef RACK_H
#define RACK_H

#include <stdbool.h>
#include <stdint.h>

#include "letter_distribution.h"

typedef struct Rack Rack;

Rack *rack_create(int array_size);
void rack_destroy(Rack *rack);
Rack *rack_duplicate(const Rack *rack);
void rack_copy(Rack *dst, const Rack *src);
void rack_reset(Rack *rack);

int rack_get_dist_size(const Rack *rack);
int rack_get_letter(const Rack *rack, uint8_t machine_letter);
int rack_get_total_letters(const Rack *rack);
bool rack_is_empty(const Rack *rack);
bool racks_are_equal(const Rack *rack1, const Rack *rack2);
bool rack_subtract(Rack *rack, Rack *subrack);

void rack_add_letter(Rack *rack, uint8_t letter);
void rack_take_letter(Rack *rack, uint8_t letter);
int rack_set_to_string(const LetterDistribution *ld, Rack *rack,
                       const char *rack_string);
int rack_get_score(const LetterDistribution *ld, const Rack *rack);

#endif
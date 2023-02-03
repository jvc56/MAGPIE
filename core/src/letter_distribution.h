#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdint.h>

#include "constants.h"

typedef struct LetterDistribution {
    // These are made big enough to index all of the possible
    // machine letter values so that zeroes can be returned
    // for machine letters that are not in the alphabet
    // (such as blank letters).
    uint32_t distribution[(MACHINE_LETTER_MAX_VALUE + 1)];
    uint32_t scores[(MACHINE_LETTER_MAX_VALUE + 1)];
    uint32_t is_vowel[(MACHINE_LETTER_MAX_VALUE + 1)];
} LetterDistribution;

LetterDistribution * create_letter_distribution(const char* filename);
void destroy_letter_distribution(LetterDistribution * letter_distribution);
void load_letter_distribution(LetterDistribution * letter_distribution, const char* filename);

#endif
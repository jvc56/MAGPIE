#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdint.h>

#include "constants.h"

typedef struct LetterDistribution {
    uint32_t size;
    uint32_t * distribution;
    uint32_t * scores;
    uint32_t * is_vowel;
} LetterDistribution;

LetterDistribution * create_letter_distribution(const char* filename);
void destroy_letter_distribution(LetterDistribution * letter_distribution);
void load_letter_distribution(LetterDistribution * letter_distribution, const char* filename);

#endif
#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdint.h>

#include "constants.h"

typedef struct LetterDistribution {
    uint32_t size;
    uint32_t * distribution;
    uint32_t * scores;
    uint32_t * score_order;
    uint32_t * is_vowel;
    // These used to be in the alphabet
    uint32_t * human_readable_letter_to_machine_letter;
    uint32_t * machine_letter_to_human_readable_letter;
} LetterDistribution;

LetterDistribution * create_letter_distribution(const char* filename);
void destroy_letter_distribution(LetterDistribution * letter_distribution);
void load_letter_distribution(LetterDistribution * letter_distribution, const char* filename);
uint8_t human_readable_letter_to_machine_letter(LetterDistribution * letter_distribution, unsigned char r);
unsigned char machine_letter_to_human_readable_letter(LetterDistribution * letter_distribution, uint8_t ml);

inline uint8_t get_blanked_machine_letter(uint8_t ml) {
	return ml | BLANK_MASK;
}

inline uint8_t get_unblanked_machine_letter(uint8_t ml) {
	return ml & UNBLANK_MASK;
}

inline uint8_t is_blanked(uint8_t ml) {
	return (ml & BLANK_MASK) > 0;
}

#endif
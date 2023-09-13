#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdint.h>

#include "constants.h"

typedef struct LetterDistribution {
  uint32_t size;
  uint32_t *distribution;
  uint32_t *scores;
  uint32_t *score_order;
  uint32_t *is_vowel;
  int max_tile_length;
  char machine_letter_to_human_readable_letter[MACHINE_LETTER_MAX_VALUE]
                                              [MAX_LETTER_CHAR_LENGTH];
} LetterDistribution;

LetterDistribution *create_letter_distribution(const char *filename);
void destroy_letter_distribution(LetterDistribution *letter_distribution);
void load_letter_distribution(LetterDistribution *letter_distribution,
                              const char *filename);
uint8_t
human_readable_letter_to_machine_letter(LetterDistribution *letter_distribution,
                                        char *letter);
void machine_letter_to_human_readable_letter(
    LetterDistribution *letter_distribution, uint8_t ml,
    char letter[MAX_LETTER_CHAR_LENGTH]);

inline uint8_t get_blanked_machine_letter(uint8_t ml) {
  return ml | BLANK_MASK;
}

inline uint8_t get_unblanked_machine_letter(uint8_t ml) {
  return ml & UNBLANK_MASK;
}

inline uint8_t is_blanked(uint8_t ml) { return (ml & BLANK_MASK) > 0; }

int str_to_machine_letters(LetterDistribution *letter_distribution,
                           const char *str, uint8_t *mls);
char *get_letter_distribution_filepath(const char *ld_name);

#endif
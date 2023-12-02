#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include "../def/letter_distribution_defs.h"

struct LetterDistribution;
typedef struct LetterDistribution LetterDistribution;

LetterDistribution *
create_letter_distribution(const char *letter_distribution_name);
void destroy_letter_distribution(LetterDistribution *letter_distribution);

uint32_t letter_distribution_get_size(const LetterDistribution *ld);
uint32_t letter_distribution_get_distribution(const LetterDistribution *ld,
                                              uint8_t machine_letter);
uint32_t letter_distribution_get_score(const LetterDistribution *ld,
                                        uint8_t machine_letter);
uint32_t letter_distribution_get_score_order(const LetterDistribution *ld,
                                             uint8_t machine_letter);
bool letter_distribution_get_is_vowel(const LetterDistribution *ld,
                                      uint8_t machine_letter);
int letter_distribution_get_total_tiles(const LetterDistribution *ld);
int letter_distribution_get_max_tile_length(const LetterDistribution *ld);
char *get_default_letter_distribution_name(const char *lexicon_name);

uint8_t human_readable_letter_to_machine_letter(
    const LetterDistribution *letter_distribution, char *letter);

inline uint8_t get_blanked_machine_letter(uint8_t ml) {
  return ml | BLANK_MASK;
}

inline uint8_t get_unblanked_machine_letter(uint8_t ml) {
  return ml & UNBLANK_MASK;
}

inline bool is_blanked(uint8_t ml) { return (ml & BLANK_MASK) > 0; }

#endif
#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "string_util.h"

#define BLANK_MASK 0x80
#define UNBLANK_MASK (0x80 - 1)
#define MAX_ALPHABET_SIZE 50
#define MACHINE_LETTER_MAX_VALUE 255
#define MAX_LETTER_CHAR_LENGTH 6

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

LetterDistribution *
create_letter_distribution(const char *letter_distribution_name);
void destroy_letter_distribution(LetterDistribution *letter_distribution);
void load_letter_distribution(LetterDistribution *letter_distribution,
                              const char *letter_distribution_name);
uint8_t
human_readable_letter_to_machine_letter(LetterDistribution *letter_distribution,
                                        char *letter);

inline uint8_t get_blanked_machine_letter(uint8_t ml) {
  return ml | BLANK_MASK;
}

inline uint8_t get_unblanked_machine_letter(uint8_t ml) {
  return ml & UNBLANK_MASK;
}

inline uint8_t is_blanked(uint8_t ml) { return (ml & BLANK_MASK) > 0; }

int str_to_machine_letters(LetterDistribution *letter_distribution,
                           const char *str, bool allow_played_through_marker,
                           uint8_t *mls);
char *get_letter_distribution_name_from_lexicon_name(const char *lexicon_name);

void string_builder_add_user_visible_letter(
    LetterDistribution *letter_distribution, uint8_t ml, size_t len,
    StringBuilder *string_builder);

#endif
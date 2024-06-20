#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../def/letter_distribution_defs.h"

typedef struct LetterDistribution {
  char *name;
  int size;
  int *distribution;
  int *scores;
  // machine letters sorted in descending
  // score order
  int *score_order;
  bool *is_vowel;
  int total_tiles;
  int max_tile_length;
  char ld_ml_to_hl[MACHINE_LETTER_MAX_VALUE][MAX_LETTER_BYTE_LENGTH];
} LetterDistribution;

LetterDistribution *ld_create(const char *data_path, const char *ld_name);
void ld_destroy(LetterDistribution *ld);

static inline const char *ld_get_name(const LetterDistribution *ld) {
  return ld->name;
}

static inline int ld_get_size(const LetterDistribution *ld) { return ld->size; }

static inline int ld_get_dist(const LetterDistribution *ld,
                              uint8_t machine_letter) {
  return ld->distribution[machine_letter];
}

static inline int ld_get_score(const LetterDistribution *ld,
                               uint8_t machine_letter) {
  return ld->scores[machine_letter];
}

static inline int ld_get_score_order(const LetterDistribution *ld,
                                     uint8_t machine_letter) {
  return ld->score_order[machine_letter];
}

static inline bool ld_get_is_vowel(const LetterDistribution *ld,
                                   uint8_t machine_letter) {
  return ld->is_vowel[machine_letter];
}

int ld_get_total_tiles(const LetterDistribution *ld);
int ld_get_max_tile_length(const LetterDistribution *ld);
char *ld_get_default_name(const char *lexicon_name);

char *ld_ml_to_hl(const LetterDistribution *ld, uint8_t ml);

uint8_t ld_hl_to_ml(const LetterDistribution *ld, char *letter);
int ld_str_to_mls(const LetterDistribution *ld, const char *str,
                  bool allow_played_through_marker, uint8_t *mls,
                  size_t mls_size);

static inline uint8_t get_blanked_machine_letter(uint8_t ml) {
  return ml | BLANK_MASK;
}

static inline uint8_t get_unblanked_machine_letter(uint8_t ml) {
  return ml & UNBLANK_MASK;
}

static inline bool get_is_blanked(uint8_t ml) { return (ml & BLANK_MASK) > 0; }

#endif
#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct LetterDistribution LetterDistribution;

LetterDistribution *ld_create(const char *ld_name);
void ld_destroy(LetterDistribution *ld);

int ld_get_size(const LetterDistribution *ld);
int ld_get_dist(const LetterDistribution *ld, uint8_t machine_letter);
int ld_get_score(const LetterDistribution *ld, uint8_t machine_letter);
int ld_get_score_order(const LetterDistribution *ld, uint8_t machine_letter);
bool ld_get_is_vowel(const LetterDistribution *ld, uint8_t machine_letter);
int ld_get_total_tiles(const LetterDistribution *ld);
int ld_get_max_tile_length(const LetterDistribution *ld);
char *ld_get_default_name(const char *lexicon_name);

char *ld_ml_to_hl(const LetterDistribution *ld, uint8_t ml);

uint8_t ld_hl_to_ml(const LetterDistribution *ld, char *letter);
int ld_str_to_mls(const LetterDistribution *ld, const char *str,
                  bool allow_played_through_marker, uint8_t *mls,
                  size_t mls_size);

uint8_t get_blanked_machine_letter(uint8_t ml);
uint8_t get_unblanked_machine_letter(uint8_t ml);
bool get_is_blanked(uint8_t ml);

#endif
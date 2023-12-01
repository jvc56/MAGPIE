#ifndef LETTERS_H
#define LETTERS_H

#include <stdint.h>

int str_to_machine_letters(const LetterDistribution *letter_distribution,
                           const char *str, bool allow_played_through_marker,
                           uint8_t *mls, size_t mls_size);

void string_builder_add_user_visible_letter(
    const LetterDistribution *letter_distribution,
    StringBuilder *string_builder, uint8_t ml);

#endif
#ifndef LETTER_DISTRIBUTION_STRING_H
#define LETTER_DISTRIBUTION_STRING_H

#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_user_visible_letter(
    const LetterDistribution *letter_distribution,
    StringBuilder *string_builder, uint8_t ml);

#endif

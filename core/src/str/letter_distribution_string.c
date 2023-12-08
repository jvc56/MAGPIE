#include <stdint.h>
#include <stdlib.h>

#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_user_visible_letter(
    const LetterDistribution *letter_distribution,
    StringBuilder *string_builder, uint8_t ml) {
  char *human_readable_letter = ml_to_hl(letter_distribution, ml);
  string_builder_add_string(string_builder, human_readable_letter);
  free(human_readable_letter);
}
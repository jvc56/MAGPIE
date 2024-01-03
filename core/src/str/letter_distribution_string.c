#include <stdint.h>
#include <stdlib.h>

#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_user_visible_letter(
    const LetterDistribution *ld,
    StringBuilder *string_builder, uint8_t ml) {
  char *human_readable_letter = ld_ml_to_hl(ld, ml);
  string_builder_add_string(string_builder, human_readable_letter);
  free(human_readable_letter);
}
#include <stdint.h>
#include <stdlib.h>

#include "../def/letter_distribution_defs.h"
#include "../ent/letter_distribution.h"

#include "../util/string_util.h"

void string_builder_add_user_visible_letter(StringBuilder *string_builder,
                                            const LetterDistribution *ld,
                                            MachineLetter ml) {
  char *human_readable_letter = ld_ml_to_hl(ld, ml);
  string_builder_add_string(string_builder, human_readable_letter);
  free(human_readable_letter);
}
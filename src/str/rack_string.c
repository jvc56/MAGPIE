#include "../def/letter_distribution_defs.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include <stdint.h>

void add_blanks(StringBuilder *string_builder, const Rack *rack,
                const LetterDistribution *ld) {
  int8_t number_of_blanks = rack_get_letter(rack, BLANK_MACHINE_LETTER);
  for (int8_t j = 0; j < number_of_blanks; j++) {
    string_builder_add_user_visible_letter(string_builder, ld,
                                           BLANK_MACHINE_LETTER);
  }
}

void string_builder_add_rack(StringBuilder *string_builder, const Rack *rack,
                             const LetterDistribution *ld, bool blanks_first) {
  if (blanks_first) {
    add_blanks(string_builder, rack, ld);
  }
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (i != BLANK_MACHINE_LETTER) {
      int8_t number_of_letter = rack_get_letter(rack, i);
      for (int8_t j = 0; j < number_of_letter; j++) {
        string_builder_add_user_visible_letter(string_builder, ld, i);
      }
    }
  }
  if (!blanks_first) {
    add_blanks(string_builder, rack, ld);
  }
}

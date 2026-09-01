#include "rack_string.h"

#include "../def/letter_distribution_defs.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void add_blanks(StringBuilder *string_builder, const Rack *rack,
                const LetterDistribution *ld) {
  uint16_t number_of_blanks = rack_get_letter(rack, BLANK_MACHINE_LETTER);
  for (uint16_t j = 0; j < number_of_blanks; j++) {
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
      uint16_t number_of_letter = rack_get_letter(rack, i);
      for (uint16_t j = 0; j < number_of_letter; j++) {
        string_builder_add_user_visible_letter(string_builder, ld, i);
      }
    }
  }
  if (!blanks_first) {
    add_blanks(string_builder, rack, ld);
  }
}

static size_t add_blanks_bounded(char *dest, size_t dest_size, size_t pos,
                                 const Rack *rack,
                                 const LetterDistribution *ld) {
  uint16_t number_of_blanks = rack_get_letter(rack, BLANK_MACHINE_LETTER);
  for (uint16_t j = 0; j < number_of_blanks; j++) {
    char *hl = ld_ml_to_hl(ld, BLANK_MACHINE_LETTER);
    pos = append_bounded(dest, dest_size, pos, hl);
    free(hl);
  }
  return pos;
}

void rack_get_string(const Rack *rack, const LetterDistribution *ld,
                     bool blanks_first, char *dest, size_t dest_size) {
  size_t pos = 0;
  if (blanks_first) {
    pos = add_blanks_bounded(dest, dest_size, pos, rack, ld);
  }
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (i != BLANK_MACHINE_LETTER) {
      uint16_t number_of_letter = rack_get_letter(rack, i);
      for (uint16_t j = 0; j < number_of_letter; j++) {
        char *hl = ld_ml_to_hl(ld, i);
        pos = append_bounded(dest, dest_size, pos, hl);
        free(hl);
      }
    }
  }
  if (!blanks_first) {
    pos = add_blanks_bounded(dest, dest_size, pos, rack, ld);
  }
  dest[pos] = '\0';
}

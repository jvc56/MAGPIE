#include "../util/string_util.h"
#include "letter_distribution_string.h"

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

void string_builder_add_rack(const Rack *rack,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder) {
  for (int i = 0; i < get_array_size(rack); i++) {
    for (int j = 0; j < get_number_of_letter(rack, i); j++) {
      string_builder_add_user_visible_letter(letter_distribution,
                                             string_builder, i);
    }
  }
}

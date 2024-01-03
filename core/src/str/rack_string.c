#include "../util/string_util.h"
#include "letter_distribution_string.h"

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

void string_builder_add_rack(const Rack *rack,
                             const LetterDistribution *ld,
                             StringBuilder *string_builder) {
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    for (int j = 0; j < rack_get_letter(rack, i); j++) {
      string_builder_add_user_visible_letter(ld,
                                             string_builder, i);
    }
  }
}

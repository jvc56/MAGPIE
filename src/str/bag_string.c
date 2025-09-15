#include "bag_string.h"

#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/letter_distribution.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include <string.h>

enum { BLANK_SORT_VALUE = 255 };

void string_builder_add_bag(StringBuilder *bag_string_builder, const Bag *bag,
                            const LetterDistribution *ld) {
  int ld_size = ld_get_size(ld);
  int bag_letter_counts[MAX_ALPHABET_SIZE];
  memset(bag_letter_counts, 0, sizeof(bag_letter_counts));
  bag_increment_unseen_count(bag, bag_letter_counts);

  for (int i = 1; i < ld_size; i++) {
    for (int j = 0; j < bag_letter_counts[i]; j++) {
      string_builder_add_user_visible_letter(bag_string_builder, ld, i);
    }
  }

  // Print the blanks at the end
  for (int i = 0; i < bag_letter_counts[BLANK_MACHINE_LETTER]; i++) {
    string_builder_add_user_visible_letter(bag_string_builder, ld,
                                           BLANK_MACHINE_LETTER);
  }
}
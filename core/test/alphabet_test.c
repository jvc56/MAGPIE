#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/config.h"

#include "superconfig.h"
#include "test_util.h"

void test_alphabet(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);
  // Test blank
  assert(get_blanked_machine_letter(1) == (1 | BLANK_MASK));
  assert(get_blanked_machine_letter(5) == (5 | BLANK_MASK));

  // Test unblank
  assert(get_unblanked_machine_letter(1) == (1 & UNBLANK_MASK));
  assert(get_unblanked_machine_letter(5) == (5 & UNBLANK_MASK));

  // Test val
  // blank
  assert(human_readable_letter_to_machine_letter(
             config->letter_distribution, BLANK_TOKEN) == BLANK_MACHINE_LETTER);
  // blank
  assert(human_readable_letter_to_machine_letter(config->letter_distribution,
                                                 "a") ==
         get_blanked_machine_letter(1));
  assert(human_readable_letter_to_machine_letter(config->letter_distribution,
                                                 "b") ==
         get_blanked_machine_letter(2));
  // not blank
  assert(human_readable_letter_to_machine_letter(config->letter_distribution,
                                                 "C") == 3);
  assert(human_readable_letter_to_machine_letter(config->letter_distribution,
                                                 "D") == 4);

  // Test user visible
  // separation token
  // The separation letter and machine letter should be the only machine
  // letters that map to the same value, since
  // blank
  char letter[MAX_LETTER_CHAR_LENGTH];
  machine_letter_to_human_readable_letter(config->letter_distribution,
                                          BLANK_MACHINE_LETTER, letter);

  assert(strcmp(letter, BLANK_TOKEN) == 0);
  // blank
  machine_letter_to_human_readable_letter(
      config->letter_distribution, get_blanked_machine_letter(1), letter);
  assert(strcmp(letter, "a") == 0);
  machine_letter_to_human_readable_letter(
      config->letter_distribution, get_blanked_machine_letter(2), letter);
  assert(strcmp(letter, "b") == 0);
  // not blank
  machine_letter_to_human_readable_letter(config->letter_distribution, 3,
                                          letter);
  assert(strcmp(letter, "C") == 0);
  machine_letter_to_human_readable_letter(config->letter_distribution, 4,
                                          letter);
  assert(strcmp(letter, "D") == 0);

  Config *catalan_config = get_disc_config(superconfig);
  machine_letter_to_human_readable_letter(catalan_config->letter_distribution,
                                          get_blanked_machine_letter(13),
                                          letter);
  assert(strcmp(letter, "l·l") == 0);
}

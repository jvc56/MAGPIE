#include <assert.h>
#include <stdio.h>

#include "../src/ent/config.h"

#include "../src/str/letter_distribution_string.h"
#include "../src/util/string_util.h"

#include "test_util.h"
#include "testconfig.h"

void test_alphabet(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  LetterDistribution *ld = config_get_letter_distribution(config);
  // Test blank
  assert(get_blanked_machine_letter(1) == (1 | BLANK_MASK));
  assert(get_blanked_machine_letter(5) == (5 | BLANK_MASK));

  // Test unblank
  assert(get_unblanked_machine_letter(1) == (1 & UNBLANK_MASK));
  assert(get_unblanked_machine_letter(5) == (5 & UNBLANK_MASK));

  // Test val
  // blank
  assert(hl_to_ml(ld, "?") == BLANK_MACHINE_LETTER);
  // blank
  assert(hl_to_ml(ld, "a") == get_blanked_machine_letter(1));
  assert(hl_to_ml(ld, "b") == get_blanked_machine_letter(2));
  // not blank
  assert(hl_to_ml(ld, "C") == 3);
  assert(hl_to_ml(ld, "D") == 4);

  // Test user visible
  // separation token
  // The separation letter and machine letter should be the only machine
  // letters that map to the same value, since
  StringBuilder *letter = create_string_builder();

  // blank
  string_builder_add_user_visible_letter(ld, letter, BLANK_MACHINE_LETTER);
  assert_strings_equal(string_builder_peek(letter), "?");
  string_builder_clear(letter);

  // blank A
  string_builder_add_user_visible_letter(ld, letter,
                                         get_blanked_machine_letter(1));
  assert_strings_equal(string_builder_peek(letter), "a");
  string_builder_clear(letter);

  string_builder_add_user_visible_letter(ld, letter,
                                         get_blanked_machine_letter(2));
  assert_strings_equal(string_builder_peek(letter), "b");
  string_builder_clear(letter);

  // not blank
  string_builder_add_user_visible_letter(ld, letter, 3);
  assert_strings_equal(string_builder_peek(letter), "C");
  string_builder_clear(letter);
  string_builder_add_user_visible_letter(ld, letter, 4);
  assert_strings_equal(string_builder_peek(letter), "D");
  string_builder_clear(letter);

  const Config *catalan_config = get_disc_config(testconfig);
  LetterDistribution *catalan_ld =
      config_get_letter_distribution(catalan_config);

  string_builder_add_user_visible_letter(catalan_ld, letter,
                                         get_blanked_machine_letter(13));
  assert_strings_equal(string_builder_peek(letter), "l·l");
  string_builder_clear(letter);

  destroy_string_builder(letter);
}

#include <assert.h>

#include "../src/def/letter_distribution_defs.h"

#include "../src/ent/letter_distribution.h"
#include "../src/impl/config.h"

#include "../src/str/letter_distribution_string.h"

#include "../src/util/string_util.h"

#include "test_util.h"

void test_alphabet(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  // Test blank
  assert(get_blanked_machine_letter(1) == (1 | BLANK_MASK));
  assert(get_blanked_machine_letter(5) == (5 | BLANK_MASK));

  // Test unblank
  assert(get_unblanked_machine_letter(1) == (1 & UNBLANK_MASK));
  assert(get_unblanked_machine_letter(5) == (5 & UNBLANK_MASK));

  // Test val
  // blank
  assert(ld_hl_to_ml(ld, "?") == BLANK_MACHINE_LETTER);
  // blank
  assert(ld_hl_to_ml(ld, "a") == get_blanked_machine_letter(1));
  assert(ld_hl_to_ml(ld, "b") == get_blanked_machine_letter(2));
  // not blank
  assert(ld_hl_to_ml(ld, "C") == 3);
  assert(ld_hl_to_ml(ld, "D") == 4);

  // Test user visible
  // separation token
  // The separation letter and machine letter should be the only machine
  // letters that map to the same value, since
  StringBuilder *letter = string_builder_create();

  // blank
  string_builder_add_user_visible_letter(letter, ld, BLANK_MACHINE_LETTER);
  assert_strings_equal(string_builder_peek(letter), "?");
  string_builder_clear(letter);

  // blank A
  string_builder_add_user_visible_letter(letter, ld,
                                         get_blanked_machine_letter(1));
  assert_strings_equal(string_builder_peek(letter), "a");
  string_builder_clear(letter);

  string_builder_add_user_visible_letter(letter, ld,
                                         get_blanked_machine_letter(2));
  assert_strings_equal(string_builder_peek(letter), "b");
  string_builder_clear(letter);

  // not blank
  string_builder_add_user_visible_letter(letter, ld, 3);
  assert_strings_equal(string_builder_peek(letter), "C");
  string_builder_clear(letter);
  string_builder_add_user_visible_letter(letter, ld, 4);
  assert_strings_equal(string_builder_peek(letter), "D");
  string_builder_clear(letter);

  Config *catalan_config = config_create_or_die(
      "set -lex DISC2 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *catalan_ld = config_get_ld(catalan_config);

  string_builder_add_user_visible_letter(letter, catalan_ld,
                                         get_blanked_machine_letter(13));
  assert_strings_equal(string_builder_peek(letter), "[l·l]");
  string_builder_clear(letter);

  string_builder_destroy(letter);
  config_destroy(config);
  config_destroy(catalan_config);
}

#include <assert.h>
#include <stdio.h>

#include "../src/ent/config.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"

#include "rack_test.h"
#include "test_util.h"

void test_rack_main() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_letter_distribution(config);
  int ld_size = letter_distribution_get_size(ld);
  Rack *rack = create_rack(ld_size);

  // Test score on rack
  set_rack_to_string(ld, rack, "ABCDEFG");
  assert(score_on_rack(ld, rack) == 16);
  set_rack_to_string(ld, rack, "XYZ");
  assert(score_on_rack(ld, rack) == 22);
  set_rack_to_string(ld, rack, "??");
  assert(score_on_rack(ld, rack) == 0);
  set_rack_to_string(ld, rack, "?QWERTY");
  assert(score_on_rack(ld, rack) == 21);
  set_rack_to_string(ld, rack, "RETINAO");
  assert(score_on_rack(ld, rack) == 7);
  set_rack_to_string(ld, rack, "AABBEWW");
  assert(score_on_rack(ld, rack) == 17);

  set_rack_to_string(ld, rack, "AENPPSW");

  assert(get_number_of_letter(rack, 1) == 1);
  assert(get_number_of_letter(rack, 5) == 1);
  assert(get_number_of_letter(rack, 14) == 1);
  assert(get_number_of_letter(rack, 16) == 2);
  assert(get_number_of_letter(rack, 19) == 1);
  assert(get_number_of_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(get_number_of_letters(rack) == 7);

  take_letter_from_rack(rack, 16);
  assert(get_number_of_letter(rack, 1) == 1);
  assert(get_number_of_letter(rack, 5) == 1);
  assert(get_number_of_letter(rack, 14) == 1);
  assert(get_number_of_letter(rack, 16) == 1);
  assert(get_number_of_letter(rack, 19) == 1);
  assert(get_number_of_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(get_number_of_letters(rack) == 6);

  take_letter_from_rack(rack, 14);
  assert(get_number_of_letter(rack, 1) == 1);
  assert(get_number_of_letter(rack, 5) == 1);
  assert(get_number_of_letter(rack, 14) == 0);
  assert(get_number_of_letter(rack, 16) == 1);
  assert(get_number_of_letter(rack, 19) == 1);
  assert(get_number_of_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(get_number_of_letters(rack) == 5);

  take_letter_from_rack(rack, 1);
  assert(!rack_is_empty(rack));
  take_letter_from_rack(rack, 5);
  assert(!rack_is_empty(rack));
  take_letter_from_rack(rack, 16);
  assert(!rack_is_empty(rack));
  take_letter_from_rack(rack, 19);
  assert(!rack_is_empty(rack));
  take_letter_from_rack(rack, 23);
  assert(rack_is_empty(rack));

  add_letter_to_rack(rack, 13);
  take_letter_from_rack(rack, 13);
  add_letter_to_rack(rack, 13);
  add_letter_to_rack(rack, 13);

  assert(get_number_of_letter(rack, 13) == 2);
  assert(!rack_is_empty(rack));
  assert(get_number_of_letters(rack) == 2);

  destroy_rack(rack);
  config_destroy(config);
}

void test_rack() { test_rack_main(); }

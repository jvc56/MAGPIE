#include <assert.h>
#include <stdio.h>

#include "../../src/ent/config.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"

#include "rack_test.h"
#include "test_util.h"

void test_rack_main() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);

  // Test score on rack
  rack_set_to_string(ld, rack, "ABCDEFG");
  assert(rack_get_score(ld, rack) == 16);
  rack_set_to_string(ld, rack, "XYZ");
  assert(rack_get_score(ld, rack) == 22);
  rack_set_to_string(ld, rack, "??");
  assert(rack_get_score(ld, rack) == 0);
  rack_set_to_string(ld, rack, "?QWERTY");
  assert(rack_get_score(ld, rack) == 21);
  rack_set_to_string(ld, rack, "RETINAO");
  assert(rack_get_score(ld, rack) == 7);
  rack_set_to_string(ld, rack, "AABBEWW");
  assert(rack_get_score(ld, rack) == 17);

  rack_set_to_string(ld, rack, "AENPPSW");

  assert(rack_get_letter(rack, 1) == 1);
  assert(rack_get_letter(rack, 5) == 1);
  assert(rack_get_letter(rack, 14) == 1);
  assert(rack_get_letter(rack, 16) == 2);
  assert(rack_get_letter(rack, 19) == 1);
  assert(rack_get_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 7);

  rack_take_letter(rack, 16);
  assert(rack_get_letter(rack, 1) == 1);
  assert(rack_get_letter(rack, 5) == 1);
  assert(rack_get_letter(rack, 14) == 1);
  assert(rack_get_letter(rack, 16) == 1);
  assert(rack_get_letter(rack, 19) == 1);
  assert(rack_get_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 6);

  rack_take_letter(rack, 14);
  assert(rack_get_letter(rack, 1) == 1);
  assert(rack_get_letter(rack, 5) == 1);
  assert(rack_get_letter(rack, 14) == 0);
  assert(rack_get_letter(rack, 16) == 1);
  assert(rack_get_letter(rack, 19) == 1);
  assert(rack_get_letter(rack, 23) == 1);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 5);

  rack_take_letter(rack, 1);
  assert(!rack_is_empty(rack));
  rack_take_letter(rack, 5);
  assert(!rack_is_empty(rack));
  rack_take_letter(rack, 16);
  assert(!rack_is_empty(rack));
  rack_take_letter(rack, 19);
  assert(!rack_is_empty(rack));
  rack_take_letter(rack, 23);
  assert(rack_is_empty(rack));

  rack_add_letter(rack, 13);
  rack_take_letter(rack, 13);
  rack_add_letter(rack, 13);
  rack_add_letter(rack, 13);

  assert(rack_get_letter(rack, 13) == 2);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 2);

  rack_destroy(rack);
  config_destroy(config);
}

void test_rack() { test_rack_main(); }

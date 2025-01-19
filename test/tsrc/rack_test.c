#include <assert.h>

#include "../../src/ent/encoded_rack.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"
#include "../../src/impl/config.h"

#include "rack_test.h"
#include "test_util.h"

void test_rack_main(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  Rack *rack_sub = rack_create(ld_size);

  // Test score on rack
  rack_set_to_string(ld, rack, "ABCDEFG");
  assert_rack_score(ld, rack, 16);
  rack_set_to_string(ld, rack, "XYZ");
  assert_rack_score(ld, rack, 22);
  rack_set_to_string(ld, rack, "??");
  assert_rack_score(ld, rack, 0);
  rack_set_to_string(ld, rack, "?QWERTY");
  assert_rack_score(ld, rack, 21);
  rack_set_to_string(ld, rack, "RETINAO");
  assert_rack_score(ld, rack, 7);
  rack_set_to_string(ld, rack, "AABBEWW");
  assert_rack_score(ld, rack, 17);

  // Test subtraction
  rack_set_to_string(ld, rack, "ABCDEFG");
  rack_set_to_string(ld, rack_sub, "ABC");
  assert(rack_subtract(rack, rack_sub));

  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "D")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "E")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "F")) == 1);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "G")) == 1);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 4);

  rack_set_to_string(ld, rack_sub, "DEFG");
  assert(rack_subtract(rack, rack_sub));
  assert(rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 0);

  rack_set_to_string(ld, rack, "AAAABBB");
  rack_set_to_string(ld, rack_sub, "AABB");
  assert(rack_subtract(rack, rack_sub));
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "A")) == 2);
  assert(rack_get_letter(rack, ld_hl_to_ml(ld, "B")) == 1);
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == 3);

  rack_set_to_string(ld, rack_sub, "AAAA");
  assert(!rack_subtract(rack, rack_sub));

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

  rack_reset(rack);

  Rack *rack_to_add = rack_duplicate(rack);
  rack_set_to_string(ld, rack, "ABBC");
  rack_set_to_string(ld, rack_to_add, "BCCEF");
  rack_add(rack, rack_to_add);

  // Use rack_sub as comparison
  rack_set_to_string(ld, rack_sub, "ABBBCCCEF");
  racks_are_equal(rack, rack_sub);

  rack_destroy(rack);
  rack_destroy(rack_sub);
  rack_destroy(rack_to_add);
  config_destroy(config);
}

void assert_rack_encode_decode(const LetterDistribution *ld, Rack *rack1,
                               Rack *rack2, const char *rack_str) {
  EncodedRack encoded_rack;
  rack_set_to_string(ld, rack1, rack_str);
  rack_encode(rack1, &encoded_rack);
  rack_decode(&encoded_rack, rack2);
  assert(racks_are_equal(rack1, rack2));
}

void test_encoded_rack(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Rack *rack1 = rack_create(ld_size);
  Rack *rack2 = rack_create(ld_size);

  assert_rack_encode_decode(ld, rack1, rack2, "");
  assert_rack_encode_decode(ld, rack1, rack2, "?");
  assert_rack_encode_decode(ld, rack1, rack2, "??");
  assert_rack_encode_decode(ld, rack1, rack2, "???????");
  assert_rack_encode_decode(ld, rack1, rack2, "??AA");
  assert_rack_encode_decode(ld, rack1, rack2, "??AAAAA");
  assert_rack_encode_decode(ld, rack1, rack2, "AAAAAAA");
  assert_rack_encode_decode(ld, rack1, rack2, "BBBBBBB");
  assert_rack_encode_decode(ld, rack1, rack2, "??AEEFI");
  assert_rack_encode_decode(ld, rack1, rack2, "??AEFFZ");
  assert_rack_encode_decode(ld, rack1, rack2, "BDIMPTV");
  assert_rack_encode_decode(ld, rack1, rack2, "Z");
  assert_rack_encode_decode(ld, rack1, rack2, "ZZZ");
  assert_rack_encode_decode(ld, rack1, rack2, "ZZZZZZZ");
  assert_rack_encode_decode(ld, rack1, rack2, "YZ");
  assert_rack_encode_decode(ld, rack1, rack2, "YYZ");
  assert_rack_encode_decode(ld, rack1, rack2, "XYYZ");
  assert_rack_encode_decode(ld, rack1, rack2, "VWWXYYZ");
  assert_rack_encode_decode(ld, rack1, rack2, "?Z");
  assert_rack_encode_decode(ld, rack1, rack2, "??YYZ");

  rack_destroy(rack1);
  rack_destroy(rack2);
  config_destroy(config);
}

void test_rack(void) {
  test_rack_main();
  test_encoded_rack();
}

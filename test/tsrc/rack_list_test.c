#include "../../src/impl/config.h"

#include "../../src/impl/rack_list.h"

#include "test_util.h"

void assert_rack_list_item_count_and_mean(const LetterDistribution *ld,
                                          const KLV *klv, RackList *rack_list,
                                          const char *rack_str, uint64_t count,
                                          double mean) {
  Rack *decoded_rack = rack_create(ld_get_size(ld));
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  int klv_index = klv_get_word_index(klv, rack);
  printf("%s: %d\n", rack_str, klv_index);
  // rack_decode(rack_list_get_encoded_rack(rack_list, klv_index),
  // decoded_rack); assert_racks_equal(ld, rack, decoded_rack);

  rack_destroy(rack);
  rack_destroy(decoded_rack);

  assert(rack_list_get_count(rack_list, 0) == count);
  assert(within_epsilon(rack_list_get_mean(rack_list, 0), mean));
}

void test_rack_list(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  RackList *rack_list = rack_list_create(ld, 3);
  const KLV *rack_list_klv = rack_list_get_klv(rack_list);

  const int number_of_racks = rack_list_get_number_of_racks(rack_list);
  assert(number_of_racks == 3199724);

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAA",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAC",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAD",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAE",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAF",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "??AAAAG",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "EEEEEEE",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "LLLLMMN",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWXYY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWXYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWYYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "U", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "A", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "B", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "V", 0,
                                       0.0);

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VV", 0,
                                       0.0);

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVW", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWX", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWXY", 0,
                                       0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWXYY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWXYYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWXY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWXY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWYY",
                                       0, 0.0);
  rack_list_destroy(rack_list);
  config_destroy(config);
}
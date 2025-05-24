#include "../../src/impl/config.h"

#include "../../src/impl/rack_list.h"
#include "../../src/util/math_util.h"

#include "test_util.h"

void assert_rack_list_item_count_and_mean(const LetterDistribution *ld,
                                          const KLV *klv, RackList *rack_list,
                                          const char *rack_str, uint64_t count,
                                          double mean) {
  Rack *decoded_rack = rack_create(ld_get_size(ld));
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  int klv_index = klv_get_word_index(klv, rack) + 1;
  rack_decode(rack_list_get_encoded_rack(rack_list, klv_index), decoded_rack);
  assert_racks_equal(ld, rack, decoded_rack);

  rack_destroy(rack);
  rack_destroy(decoded_rack);

  assert(rack_list_get_count(rack_list, klv_index) == count);
  assert(within_epsilon(rack_list_get_mean(rack_list, klv_index), mean));
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
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VWWXYYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWXYY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWXYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWWYYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "VVWXYYZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWXY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWXY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "UVVWWYY",
                                       0, 0.0);

  Rack rack;
  const int ld_size = ld_get_size(ld);
  rack_set_dist_size(&rack, ld_size);
  rack_reset(&rack);

  char *rack_strs[] = {"AEINRT", "BCCGHI"};
  const int num_racks = sizeof(rack_strs) / sizeof(rack_strs[0]);
  double total_equities[] = {0.0, 0.0};
  uint64_t total_combos[] = {0, 0};
  double all_comb_equities = 0.0;

  for (int rack_index = 0; rack_index < num_racks; rack_index++) {
    rack_set_to_string(ld, &rack, rack_strs[rack_index]);
    for (int ml = 0; ml < ld_size; ml++) {
      const int num_ml_already_in_rack = rack_get_letter(&rack, ml);
      if (num_ml_already_in_rack == ld_get_dist(ld, ml)) {
        continue;
      }
      rack_add_letter(&rack, ml);
      double rack_equity = (double)ml;
      rack_list_add_rack(rack_list, &rack, rack_equity);
      const uint64_t draw_combos = ld_get_dist(ld, ml) - num_ml_already_in_rack;
      total_equities[rack_index] += rack_equity * draw_combos;
      total_combos[rack_index] += draw_combos;
      uint64_t rack_combos = 1;
      for (int j = 0; j < ld_size; j++) {
        const int num_ml = rack_get_letter(&rack, j);
        if (num_ml == 0) {
          continue;
        }
        rack_combos *= choose(ld_get_dist(ld, j), num_ml);
      }
      all_comb_equities += rack_equity * rack_combos;
      rack_take_letter(&rack, ml);
    }
  }

  KLV *leaves_klv = klv_create_or_die(DEFAULT_DATA_PATHS, "CSW21");
  rack_list_write_to_klv(rack_list, ld, leaves_klv);

  for (int rack_index = 0; rack_index < num_racks; rack_index++) {
    rack_set_to_string(ld, &rack, rack_strs[rack_index]);
    Equity actual_eq = klv_get_leave_value(leaves_klv, &rack);
    Equity expected_eq = double_to_equity(
        (total_equities[rack_index] / total_combos[rack_index]) -
        (all_comb_equities / 16007560800));
    assert(actual_eq == expected_eq);
  }

  rack_list_reset(rack_list, 4);

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAEINRT",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "ABEINRT",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AEINRT?",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AEINRTY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AEINRTZ",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "ABCDFGH",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "BBCDFGH",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "?BCDFGH",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "BCDFGHY",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "BCDFGHZ",
                                       0, 0.0);
  klv_destroy(leaves_klv);
  rack_list_destroy(rack_list);
  config_destroy(config);
}
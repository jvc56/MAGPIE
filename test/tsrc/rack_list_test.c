#include "../../src/impl/config.h"

#include "../../src/impl/rack_list.h"
#include "../../src/util/math_util.h"

#include "test_util.h"

void assert_rack_list_item_count_and_mean(
    const LetterDistribution *ld, const KLV *klv, const RackList *rack_list,
    const char *rack_str, const uint64_t count, const double mean) {
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
  Config *config =
      config_create_or_die("set -lex CSW21 -ld english_ab -s1 equity -s2 "
                           "equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  RackList *rack_list = rack_list_create(ld, 3);
  const KLV *rack_list_klv = rack_list_get_klv(rack_list);

  const int number_of_racks = rack_list_get_number_of_racks(rack_list);
  assert(number_of_racks == 8);

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAAAA",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAAAB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAABB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAABBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAABBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AABBBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "ABBBBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "BBBBBBB",
                                       0, 0.0);

  Rack rack;
  const int ld_size = ld_get_size(ld);
  rack_set_dist_size(&rack, ld_size);
  rack_reset(&rack);

  char *rack_strs[] = {"AAAAAA", "AAAABB", "AABBBB", "BBBBBB"};
  const int num_racks = sizeof(rack_strs) / sizeof(rack_strs[0]);
  double *total_equities = calloc_or_die(num_racks, sizeof(double));
  uint64_t *total_combos = calloc_or_die(num_racks, sizeof(uint64_t));
  double all_comb_equities = 0.0;

  for (int rack_index = 0; rack_index < num_racks; rack_index++) {
    rack_set_to_string(ld, &rack, rack_strs[rack_index]);
    for (int ml = 0; ml < ld_size; ml++) {
      const int num_ml_already_in_rack = (int)rack_get_letter(&rack, ml);
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
        const int8_t num_ml = rack_get_letter(&rack, j);
        if (num_ml == 0) {
          continue;
        }
        rack_combos *= choose(ld_get_dist(ld, j), num_ml);
      }
      all_comb_equities += rack_equity * rack_combos;
      rack_take_letter(&rack, ml);
    }
  }

  KLV *leaves_klv = klv_create_or_die(DEFAULT_TEST_DATA_PATH, "CSW21_ab");
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

  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAAAA",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAAAB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAAABB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAAABBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AAABBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "AABBBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "ABBBBBB",
                                       0, 0.0);
  assert_rack_list_item_count_and_mean(ld, rack_list_klv, rack_list, "BBBBBBB",
                                       0, 0.0);
  free(total_equities);
  free(total_combos);
  klv_destroy(leaves_klv);
  rack_list_destroy(rack_list);
  config_destroy(config);
}
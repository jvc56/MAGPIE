#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/rack_list.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void assert_rack_list_item_count_and_mean(
    const LetterDistribution *ld, const KLV *klv, const RackList *rack_list,
    const char *rack_str, const uint64_t count, const double mean) {
  Rack *decoded_rack = rack_create(ld_get_size(ld));
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_str);
  int klv_index = (int)klv_get_word_index(klv, rack) + 1;
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
  ErrorStack *error_stack = error_stack_create();
  RackList *rack_list = rack_list_create(ld, 3, NULL, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
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
  rack_set_dist_size_and_reset(&rack, ld_size);

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
      total_equities[rack_index] += rack_equity * (double)draw_combos;
      total_combos[rack_index] += draw_combos;
      uint64_t rack_combos = 1;
      for (int j = 0; j < ld_size; j++) {
        const uint16_t num_ml = rack_get_letter(&rack, j);
        if (num_ml == 0) {
          continue;
        }
        rack_combos *= choose(ld_get_dist(ld, j), num_ml);
      }
      all_comb_equities += rack_equity * (double)rack_combos;
      rack_take_letter(&rack, ml);
    }
  }

  KLV *leaves_klv = klv_create_or_die(DEFAULT_TEST_DATA_PATH, "CSW21_ab");
  rack_list_write_to_klv(rack_list, ld, leaves_klv);

  for (int rack_index = 0; rack_index < num_racks; rack_index++) {
    rack_set_to_string(ld, &rack, rack_strs[rack_index]);
    Equity actual_eq = klv_get_leave_value(leaves_klv, &rack);
    Equity expected_eq = double_to_equity(
        (total_equities[rack_index] / (double)total_combos[rack_index]) -
        (all_comb_equities / 16007560800.0));
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

void test_rack_list_forced_racks(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -ld english_ab -s1 equity -s2 "
                           "equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);

  // A file with a duplicate rack must be rejected rather than silently
  // corrupting the rare partition (see rack_list_restrict_to_forced_racks).
  const char *duplicate_racks_filename = "test_rack_list_duplicate_racks.txt";
  ErrorStack *write_error_stack = error_stack_create();
  write_string_to_file(duplicate_racks_filename, "w",
                       "AAAAAAB\nABBBBBB\nAAAAAAB\n", write_error_stack);
  assert(error_stack_is_empty(write_error_stack));
  error_stack_destroy(write_error_stack);

  ErrorStack *duplicate_error_stack = error_stack_create();
  const RackList *duplicate_rack_list =
      rack_list_create(ld, 3, duplicate_racks_filename, duplicate_error_stack);
  assert(duplicate_rack_list == NULL);
  assert(error_stack_top(duplicate_error_stack) ==
         ERROR_STATUS_AUTOPLAY_FORCE_RACKS_DUPLICATE_RACK);
  error_stack_destroy(duplicate_error_stack);
  (void)remove(duplicate_racks_filename);

  // A file with distinct racks restricts the rare partition to just those
  // racks, and rack_list_write_rack_equity_csv writes every observed rack
  // (not just forced ones).
  const char *forced_racks_filename = "test_rack_list_forced_racks.txt";
  ErrorStack *forced_write_error_stack = error_stack_create();
  write_string_to_file(forced_racks_filename, "w", "AAAAAAB\nABBBBBB\n",
                       forced_write_error_stack);
  assert(error_stack_is_empty(forced_write_error_stack));
  error_stack_destroy(forced_write_error_stack);

  ErrorStack *error_stack = error_stack_create();
  RackList *rack_list =
      rack_list_create(ld, 3, forced_racks_filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  assert(rack_list_get_racks_below_target_count(rack_list) == 2);

  Rack forced_rack_1;
  rack_set_dist_size(&forced_rack_1, ld_get_size(ld));
  rack_set_to_string(ld, &forced_rack_1, "AAAAAAB");
  Rack forced_rack_2;
  rack_set_dist_size(&forced_rack_2, ld_get_size(ld));
  rack_set_to_string(ld, &forced_rack_2, "ABBBBBB");

  XoshiroPRNG *prng = prng_create(42);
  Rack rare_rack;
  rack_set_dist_size(&rare_rack, ld_get_size(ld));
  for (int draw_idx = 0; draw_idx < 20; draw_idx++) {
    assert(rack_list_get_rare_rack(rack_list, prng, &rare_rack));
    assert(racks_are_equal(&rare_rack, &forced_rack_1) ||
           racks_are_equal(&rare_rack, &forced_rack_2));
  }
  prng_destroy(prng);

  // Also record a non-forced rack: it should show up in the CSV, but never
  // be eligible as a rare draw.
  Rack unforced_rack;
  rack_set_dist_size(&unforced_rack, ld_get_size(ld));
  rack_set_to_string(ld, &unforced_rack, "AAAAABB");
  rack_list_add_rack(rack_list, &unforced_rack, 5.0);
  rack_list_add_rack(rack_list, &unforced_rack, 7.0);

  const char *csv_filename = "test_rack_list_rack_equity.csv";
  ErrorStack *csv_error_stack = error_stack_create();
  rack_list_write_rack_equity_csv(rack_list, ld, csv_filename, csv_error_stack);
  assert(error_stack_is_empty(csv_error_stack));
  error_stack_destroy(csv_error_stack);

  char *csv_contents = get_string_from_file_or_die(csv_filename);
  assert(has_substring(csv_contents, "AAAAABB,2,6.000000"));
  free(csv_contents);
  (void)remove(csv_filename);

  rack_list_destroy(rack_list);
  (void)remove(forced_racks_filename);
  config_destroy(config);
}
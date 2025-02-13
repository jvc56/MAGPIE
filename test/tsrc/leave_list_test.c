#include <assert.h>

#include "../../src/ent/encoded_rack.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void assert_leave_list_item_count_and_mean(const LetterDistribution *ld,
                                           const KLV *klv,
                                           LeaveList *leave_list,
                                           const char *leave_str,
                                           uint64_t count, double mean) {
  Rack *decoded_leave = rack_create(ld_get_size(ld));
  Rack *leave = rack_create(ld_get_size(ld));

  rack_set_to_string(ld, leave, leave_str);
  int klv_index = klv_get_word_index(klv, leave);
  rack_decode(leave_list_get_encoded_rack(leave_list, klv_index),
              decoded_leave);
  assert_racks_equal(ld, leave, decoded_leave);

  rack_destroy(leave);
  rack_destroy(decoded_leave);

  assert(leave_list_get_count(leave_list, klv_index) == count);
  assert(within_epsilon(leave_list_get_mean(leave_list, klv_index), mean));
}

void test_leave_list_normal_leaves(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  int target_leave_count = 3;
  LeaveList *leave_list = leave_list_create(ld, klv, target_leave_count, 1);

  const int number_of_leaves = klv_get_number_of_leaves(klv);

  assert(leave_list_get_number_of_leaves(leave_list) == number_of_leaves);

  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "A", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "B", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "?", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "Z", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "VWWXYZ", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "??", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "??AA", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "??AABB", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "ABCD", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "XYYZ", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "XZ", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "VX", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "AGHM", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "ABC", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "?ABCD", 0, 0.0);

  Rack *rack = rack_create(ld_get_size(ld));
  Rack *subrack = rack_create(ld_get_size(ld));

  // Test adding a subleave

  const double subleave_value = 2000.0;
  rack_set_to_string(ld, rack, "STUV");
  leave_list_add_single_subleave(leave_list, 0, rack, subleave_value);

  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "S", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "T", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "U", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "V", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "ST", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SU", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TU", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "UV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STU", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SUV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TUV", 0, 0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STUV", 1,
                                        subleave_value);

  rack_destroy(rack);
  rack_destroy(subrack);
  leave_list_destroy(leave_list);
  config_destroy(config);
}

void leave_list_add_sas(LeaveList *leave_list, const LetterDistribution *ld,
                        Rack *subleave, const char *subleave_str,
                        int expected_leaves_below_target_count, double equity) {
  rack_reset(subleave);
  rack_set_to_string(ld, subleave, subleave_str);
  leave_list_add_single_subleave(leave_list, 0, subleave, equity);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         expected_leaves_below_target_count);
}

void test_leave_list_small_leaves(void) {
  Config *config =
      config_create_or_die("set -lex CSW21_ab -ld english_ab -s1 equity -s2 "
                           "equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  Bag *bag = bag_create(ld);
  Rack *sl = rack_create(ld_get_size(ld));
  Rack *player_rack = rack_create(ld_get_size(ld));
  Rack *rare_leave = rack_create(ld_get_size(ld));

  int tmc = 3;
  LeaveList *ll = leave_list_create(ld, klv, tmc, 1);

  const int number_of_leaves = klv_get_number_of_leaves(klv);

  int lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "B", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AA", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "BB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAA", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "ABB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "BBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAA", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AABB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "ABBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "BBBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAAA", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAAB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAABB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AABBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "ABBBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "BBBBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAAAA", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAAAB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAAABB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AAABBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "AABBBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc, 1.0);
    leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc, 1.0);
  }

  lutmc--;
  leave_list_add_sas(ll, ld, sl, "A", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "B", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AA", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "BB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAA", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "ABB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "BBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAA", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AABB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "ABBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "BBBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAAA", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAAB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAABB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AABBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "ABBBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "BBBBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAAAA", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAAAB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAAABB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AAABBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc--, 1.0);
  leave_list_add_sas(ll, ld, sl, "AABBBB", lutmc--, 1.0);

  leave_list_reset(ll, tmc);

  lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0);
  }
  for (int i = 0; i < 5; i++) {
    leave_list_add_sas(ll, ld, sl, "A", lutmc - 1, 1.0);
  }

  leave_list_reset(ll, tmc);

  int rare_leaves_drawn = 0;
  XoshiroPRNG *prng = prng_create(100);
  while (leave_list_get_leaves_below_target_count(ll) > 0) {
    assert(leave_list_get_rare_leave(ll, prng, rare_leave));
    leave_list_add_single_subleave(ll, 0, rare_leave, 10.0);
    rare_leaves_drawn++;
  }
  assert(rare_leaves_drawn == number_of_leaves * tmc);
  assert(!leave_list_get_rare_leave(ll, prng, rare_leave));

  prng_destroy(prng);
  bag_destroy(bag);
  rack_destroy(player_rack);
  rack_destroy(rare_leave);
  rack_destroy(sl);
  leave_list_destroy(ll);
  config_destroy(config);
}

void test_leave_list(void) {
  test_leave_list_normal_leaves();
  test_leave_list_small_leaves();
}
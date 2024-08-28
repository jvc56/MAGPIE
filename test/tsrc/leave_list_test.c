#include <assert.h>

#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void assert_leave_list_item(const LetterDistribution *ld, const KLV *klv,
                            LeaveList *leave_list, const char *leave_str,
                            int expected_count_index, uint64_t count,
                            double mean) {
  Rack *expected_rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, expected_rack, leave_str);
  // We can use the count index as the KLV index for a new leave list
  // because the leave list items are copied from the klv ordering
  // to the count ordering upon creation.
  int actual_count_index = leave_list_get_count_index(
      leave_list, klv_get_word_index(klv, expected_rack));
  rack_destroy(expected_rack);
  if (expected_count_index >= 0) {
    assert(actual_count_index == expected_count_index);
  }
  assert(leave_list_get_count(leave_list, actual_count_index) == count);
  assert(within_epsilon(leave_list_get_mean(leave_list, actual_count_index),
                        mean));
}

void test_leave_list_add_leave(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  LeaveList *leave_list = leave_list_create(ld, klv, 100000);

  const int number_of_leaves = klv_get_number_of_leaves(klv);
  const int player_draw_index = 0;

  assert(leave_list_get_number_of_leaves(leave_list) == number_of_leaves);

  assert_leave_list_item(ld, klv, leave_list, "A", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "B", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "Z", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VWWXYZ", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AA", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AABB", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABCD", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XYYZ", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XZ", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VX", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "AGHM", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABC", -1, 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?ABCD", -1, 0, 0.0);

  Rack *rack = rack_create(ld_get_size(ld));

  // Adding the empty leave should have no effect
  rack_set_to_string(ld, rack, "");
  leave_list_add_leave(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 1);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, rack, 4.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 2);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 7.0 / 2));
  assert_leave_list_item(ld, klv, leave_list, "A", number_of_leaves - 1, 1,
                         4.0);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 5.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 3);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 12.0 / 3));
  assert_leave_list_item(ld, klv, leave_list, "B", -1, 1, 5.0);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, rack, 6.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 4);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 18.0 / 4));
  assert_leave_list_item(ld, klv, leave_list, "A", number_of_leaves - 1, 2,
                         10.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 5);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 25.0 / 5));
  assert_leave_list_item(ld, klv, leave_list, "B", -1, 2, 12.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 9.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 6);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 34.0 / 6));
  assert_leave_list_item(ld, klv, leave_list, "B", number_of_leaves - 1, 3,
                         21.0 / 3);

  rack_set_to_string(ld, rack, "C");
  leave_list_add_leave(leave_list, rack, 11.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 7);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 45.0 / 7));
  assert_leave_list_item(ld, klv, leave_list, "C", number_of_leaves - 3, 1,
                         11.0);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 15.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 8);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 60.0 / 8));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 1, 15.0);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 9);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 77.0 / 9));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 2, 32.0 / 2);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 10);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 94.0 / 10));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 3, 49.0 / 3);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 11);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 95.0 / 11));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 4, 50.0 / 4);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 12);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 96.0 / 12));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 5, 51.0 / 5);

  rack_set_to_string(ld, rack, "DEG");
  leave_list_add_leave(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 13);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 99.0 / 13));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "G", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DG", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "EG", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEG", -1, 1, 3.0);

  rack_set_to_string(ld, rack, "HII");
  leave_list_add_leave(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 14);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 106.0 / 14));
  assert_leave_list_item(ld, klv, leave_list, "D", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "E", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "F", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "G", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", -1, 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "DF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DG", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "EG", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", -1, 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEG", -1, 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "H", -1, 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "I", -1, 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "HI", -1, 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "II", -1, 1, 7.0);

  Bag *bag = bag_create(ld);
  Rack *expected_rack = rack_create(ld_get_size(ld));
  clear_bag(bag);

  rack_reset(rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  assert(leave_list_draw_rarest_available_leave(leave_list, bag, rack, NULL,
                                                player_draw_index));
  assert(bag_get_tiles(bag) == 0);
  rack_set_to_string(ld, expected_rack, "A");
  assert(racks_are_equal(expected_rack, rack));

  rack_reset(rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  assert(leave_list_draw_rarest_available_leave(leave_list, bag, rack, NULL,
                                                player_draw_index));
  clear_bag(bag);

  rack_set_to_string(ld, rack, "DE");
  leave_list_add_leave(leave_list, rack, 1.0);
  rack_set_to_string(ld, rack, "DG");
  leave_list_add_leave(leave_list, rack, 1.0);
  rack_set_to_string(ld, rack, "EG");
  leave_list_add_leave(leave_list, rack, 1.0);

  rack_reset(rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  assert(leave_list_draw_rarest_available_leave(leave_list, bag, rack, NULL,
                                                player_draw_index));
  assert(bag_get_tiles(bag) == 0);
  rack_set_to_string(ld, expected_rack, "DEG");
  assert(racks_are_equal(expected_rack, rack));

  rack_set_to_string(ld, rack, "DEG");
  leave_list_add_leave(leave_list, rack, 1.0);
  rack_set_to_string(ld, rack, "H");
  leave_list_add_leave(leave_list, rack, 1.0);
  rack_set_to_string(ld, rack, "I");
  leave_list_add_leave(leave_list, rack, 2.0);
  rack_set_to_string(ld, rack, "HI");
  leave_list_add_leave(leave_list, rack, 4.0);
  rack_set_to_string(ld, rack, "ABC");
  leave_list_add_leave(leave_list, rack, 6.0);
  rack_set_to_string(ld, rack, "ABCI");
  leave_list_add_leave(leave_list, rack, 8.0);

  rack_reset(rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rarest_available_leave(leave_list, bag, rack, NULL,
                                                player_draw_index));
  assert(bag_get_tiles(bag) == 0);
  rack_set_to_string(ld, expected_rack, "ABCII");
  assert(racks_are_equal(expected_rack, rack));

  rack_set_to_string(ld, rack, "J");
  leave_list_add_leave(leave_list, rack, 4.0);
  rack_set_to_string(ld, rack, "JK");
  leave_list_add_leave(leave_list, rack, 5.0);
  rack_set_to_string(ld, rack, "JKL");
  leave_list_add_leave(leave_list, rack, 6.0);
  rack_set_to_string(ld, rack, "KLM");
  leave_list_add_leave(leave_list, rack, 7.0);

  double empty_leave_mean = leave_list_get_empty_leave_mean(leave_list);

  leave_list_write_to_klv(leave_list);

  rack_set_to_string(ld, rack, "J");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 5.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "K");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 6.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "L");
  assert(within_epsilon(klv_get_leave_value(klv, rack),
                        13.0 / 2 - empty_leave_mean));

  rack_set_to_string(ld, rack, "M");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 7.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "JK");
  assert(within_epsilon(klv_get_leave_value(klv, rack),
                        11.0 / 2 - empty_leave_mean));

  rack_set_to_string(ld, rack, "KL");
  assert(within_epsilon(klv_get_leave_value(klv, rack),
                        13.0 / 2 - empty_leave_mean));

  rack_set_to_string(ld, rack, "JL");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 6.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "LM");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 7.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "KM");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 7.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "JKL");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 6.0 - empty_leave_mean));

  rack_set_to_string(ld, rack, "KLM");
  assert(
      within_epsilon(klv_get_leave_value(klv, rack), 7.0 - empty_leave_mean));

  // Ensure that adding a full rack runs without error
  const double full_rack_value = 1013.0;
  rack_set_to_string(ld, rack, "PQRSTUV");
  leave_list_add_leave(leave_list, rack, full_rack_value);

  assert_leave_list_item(ld, klv, leave_list, "S", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "ST", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STUV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "P", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "Q", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "R", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "PQRSTU", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "QRSTUV", -1, 1, full_rack_value);

  // Test adding a subleave

  const double subleave_value = 2000.0;
  rack_set_to_string(ld, rack, "STUV");
  leave_list_add_subleave(leave_list, rack, subleave_value);

  assert_leave_list_item(ld, klv, leave_list, "S", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "T", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "U", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "V", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "ST", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SU", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TU", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "UV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STU", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SUV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TUV", -1, 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STUV", -1, 2,
                         (full_rack_value + subleave_value) / 2);

  rack_destroy(expected_rack);
  rack_destroy(rack);
  bag_destroy(bag);
  leave_list_destroy(leave_list);
  config_destroy(config);
}

int leave_list_add_sas(LeaveList *leave_list, const LetterDistribution *ld,
                       Rack *subleave, const char *subleave_str,
                       int expected_leaves_under_target_min_count,
                       double equity) {
  rack_reset(subleave);
  rack_set_to_string(ld, subleave, subleave_str);
  int lowest_leave_count =
      leave_list_add_subleave(leave_list, subleave, equity);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         expected_leaves_under_target_min_count);
  return lowest_leave_count;
}

void test_leave_list_draw_rarest_available(void) {
  Config *config =
      config_create_or_die("set -lex CSW21_ab -ld english_ab -s1 equity -s2 "
                           "equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  Bag *bag = bag_create(ld);
  Rack *sl = rack_create(ld_get_size(ld));
  Rack *rack = rack_create(ld_get_size(ld));

  int tmc = 3;
  LeaveList *ll = leave_list_create(ld, klv, tmc);

  const int number_of_leaves = klv_get_number_of_leaves(klv);
  const int player_draw_index = 0;

  int lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    rack_set_to_string(ld, rack, "A");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "AAAABB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "BBBBBB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);

    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "B", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AA", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAA", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "ABB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAA", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AABB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "ABBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BBBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAAA", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAAB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAABB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AABBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "ABBBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BBBBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAAAA", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAAAB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAAABB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AAABBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "AABBBB", lutmc, 1.0) == i);

    rack_set_to_string(ld, rack, "A");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "AAAABB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "BBBBBB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);

    assert(leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc, 1.0) == i + 1);

    rack_set_to_string(ld, rack, "A");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "AAAABB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
    rack_set_to_string(ld, rack, "BBBBBB");
    assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                  player_draw_index));
    bag_reset(ld, bag);
  }

  rack_set_to_string(ld, rack, "A");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "AAAABB");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "BBBBBB");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);

  lutmc--;
  assert(leave_list_add_sas(ll, ld, sl, "A", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "B", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AA", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAA", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "ABB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAA", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AABB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "ABBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BBBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAAA", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAAB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAABB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AABBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "ABBBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BBBBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAAAA", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAAAB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAAABB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "AAABBB", lutmc--, 1.0) == tmc - 1);

  rack_set_to_string(ld, rack, "A");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "AAAABB");

  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "BBBBBB");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);

  assert(leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc--, 1.0) == tmc - 1);

  rack_set_to_string(ld, rack, "A");
  assert(leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "AAAABB");
  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "BBBBBB");
  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);

  assert(leave_list_add_sas(ll, ld, sl, "AABBBB", lutmc--, 1.0) == tmc);

  rack_set_to_string(ld, rack, "A");
  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "AAAABB");
  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);
  rack_set_to_string(ld, rack, "BBBBBB");
  assert(!leave_list_draw_rarest_available_leave(ll, bag, rack, NULL,
                                                 player_draw_index));
  bag_reset(ld, bag);

  leave_list_reset(ll);

  lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0) == 0);
  }
  for (int i = 0; i < 5; i++) {
    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc - 1, 1.0) == 0);
  }

  bag_destroy(bag);
  rack_destroy(rack);
  rack_destroy(sl);
  leave_list_destroy(ll);
  config_destroy(config);
}

void test_leave_list(void) {
  test_leave_list_add_leave();
  test_leave_list_draw_rarest_available();
}
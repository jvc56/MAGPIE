#include <assert.h>

#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void assert_leave_list_item(const LetterDistribution *ld, const KLV *klv,
                            LeaveList *leave_list, const char *leave_str,
                            uint64_t count, double mean) {
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, leave_str);
  int klv_index = klv_get_word_index(klv, leave);
  rack_destroy(leave);
  assert(leave_list_get_count(leave_list, klv_index) == count);
  assert(within_epsilon(leave_list_get_mean(leave_list, klv_index), mean));
}

void test_leave_list_add_leave_and_draw_rarest(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  int target_min_leave_count = 3;
  LeaveList *leave_list = leave_list_create(ld, klv, target_min_leave_count);

  const int number_of_leaves = klv_get_number_of_leaves(klv);
  const int player_draw_index = 0;

  assert(leave_list_get_number_of_leaves(leave_list) == number_of_leaves);

  assert_leave_list_item(ld, klv, leave_list, "A", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "B", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "Z", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VWWXYZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AA", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AABB", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABCD", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XYYZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VX", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "AGHM", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABC", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?ABCD", 0, 0.0);

  Rack *rack = rack_create(ld_get_size(ld));

  // Adding the empty leave should have no effect
  rack_set_to_string(ld, rack, "");
  leave_list_add_leave(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 1);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, rack, 4.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 2);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 7.0 / 2));
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item(ld, klv, leave_list, "A", 1, 4.0);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 5.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 3);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 12.0 / 3));
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item(ld, klv, leave_list, "B", 1, 5.0);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, rack, 6.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 4);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 18.0 / 4));
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item(ld, klv, leave_list, "A", 2, 10.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 5);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 25.0 / 5));
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item(ld, klv, leave_list, "B", 2, 12.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, rack, 9.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 6);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 34.0 / 6));
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 1);
  assert_leave_list_item(ld, klv, leave_list, "B", 3, 21.0 / 3);

  rack_set_to_string(ld, rack, "C");
  leave_list_add_leave(leave_list, rack, 11.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 7);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 45.0 / 7));
  assert_leave_list_item(ld, klv, leave_list, "C", 1, 11.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 1);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 15.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 8);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 60.0 / 8));
  assert_leave_list_item(ld, klv, leave_list, "D", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "E", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "F", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DF", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "EF", 1, 15.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 1, 15.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 1);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 9);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 77.0 / 9));
  assert_leave_list_item(ld, klv, leave_list, "D", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "E", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "F", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DE", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DF", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "EF", 2, 32.0 / 2);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 2, 32.0 / 2);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 1);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 10);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 94.0 / 10));
  assert_leave_list_item(ld, klv, leave_list, "D", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "E", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "F", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DE", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DF", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "EF", 3, 49.0 / 3);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 3, 49.0 / 3);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 8);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 11);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 95.0 / 11));
  assert_leave_list_item(ld, klv, leave_list, "D", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "E", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "F", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DE", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DF", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "EF", 4, 50.0 / 4);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 4, 50.0 / 4);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 8);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 12);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 96.0 / 12));
  assert_leave_list_item(ld, klv, leave_list, "D", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "E", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DE", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 5, 51.0 / 5);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 8);

  rack_set_to_string(ld, rack, "DEG");
  leave_list_add_leave(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 13);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 99.0 / 13));
  assert_leave_list_item(ld, klv, leave_list, "D", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "E", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "G", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DG", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "EG", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEG", 1, 3.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 8);

  rack_set_to_string(ld, rack, "HII");
  leave_list_add_leave(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 14);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 106.0 / 14));
  assert_leave_list_item(ld, klv, leave_list, "D", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "E", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "G", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DE", 6, 54.0 / 6);
  assert_leave_list_item(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DG", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "EG", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "DEF", 5, 51.0 / 5);
  assert_leave_list_item(ld, klv, leave_list, "DEG", 1, 3.0);
  assert_leave_list_item(ld, klv, leave_list, "H", 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "I", 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "HI", 1, 7.0);
  assert_leave_list_item(ld, klv, leave_list, "II", 1, 7.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 8);

  leave_list_reset(leave_list);

  assert_leave_list_item(ld, klv, leave_list, "A", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "B", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "Z", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VWWXYZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AA", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "??AABB", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABCD", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XYYZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "XZ", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "VX", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "AGHM", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "ABC", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "?ABCD", 0, 0.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);

  Bag *bag = bag_create(ld);
  Rack *expected_rack = rack_create(ld_get_size(ld));
  Rack *player_rack = rack_create(ld_get_size(ld));
  clear_bag(bag);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "A");
  assert(racks_are_equal(expected_rack, rack));
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 0);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "F");
  assert(racks_are_equal(expected_rack, rack));
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 2);

  rack_reset(player_rack);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "E");
  assert(racks_are_equal(expected_rack, rack));
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 1);

  rack_reset(player_rack);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "D");
  assert(racks_are_equal(expected_rack, rack));
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 0);

  clear_bag(bag);

  rack_set_to_string(ld, rack, "DE");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  rack_set_to_string(ld, rack, "DG");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);
  rack_set_to_string(ld, rack, "EG");
  leave_list_add_leave(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 2);
  rack_set_to_string(ld, expected_rack, "G");
  assert(racks_are_equal(expected_rack, rack));

  rack_set_to_string(ld, rack, "DEG");
  leave_list_add_leave(leave_list, rack, 1.0);
  // The leaves D, E, and G, all reached the min count
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 3);

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
  // The leave I reached the min count
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 4);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 5);
  rack_set_to_string(ld, expected_rack, "II");
  assert(racks_are_equal(expected_rack, rack));

  rack_set_to_string(ld, rack, "II");

  // Add the leave II 3 times to reach the min count
  leave_list_add_leave(leave_list, rack, 2.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 4);
  leave_list_add_leave(leave_list, rack, 2.0);
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 4);
  leave_list_add_leave(leave_list, rack, 2.0);
  // The leave II reached the min count
  assert(leave_list_get_leaves_under_target_min_count(leave_list) ==
         number_of_leaves - 5);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 5);
  rack_set_to_string(ld, expected_rack, "EI");
  assert(racks_are_equal(expected_rack, rack));

  for (int i = 0; i < target_min_leave_count; i++) {
    rack_set_to_string(ld, rack, "ABCDII");
    leave_list_add_leave(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ABCEII");
    leave_list_add_leave(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ABDEII");
    leave_list_add_leave(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ACDEII");
    leave_list_add_leave(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "BCDEII");
    leave_list_add_leave(leave_list, rack, 2.0);
  }

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 2);
  rack_set_to_string(ld, expected_rack, "ABCDE");
  assert(racks_are_equal(expected_rack, rack));

  for (int i = 0; i < target_min_leave_count; i++) {
    rack_set_to_string(ld, rack, "ABCDE");
    leave_list_add_leave(leave_list, rack, 2.0);
  }

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 1);
  rack_set_to_string(ld, expected_rack, "ABCDEI");
  assert(racks_are_equal(expected_rack, rack));

  // Test with nonempty player leaves
  leave_list_reset(leave_list);
  clear_bag(bag);

  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);

  rack_set_to_string(ld, player_rack, "ABCH");
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 4);
  rack_set_to_string(ld, expected_rack, "H");
  assert(racks_are_equal(expected_rack, rack));
  rack_set_to_string(ld, expected_rack, "ABCH");
  assert(racks_are_equal(expected_rack, player_rack));

  int num_letters = 8;
  const char *letters[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
  rack_reset(rack);

  // Remove all 3 letters leaves and below from
  // consideration by making all of their counts at
  // or above the minimum target of 3.
  for (int i = 0; i < num_letters; i++) {
    rack_add_letter(rack, ld_hl_to_ml(ld, letters[i]));
    for (int j = i + 1; j < num_letters; j++) {
      rack_add_letter(rack, ld_hl_to_ml(ld, letters[j]));
      for (int k = j + 1; k < num_letters; k++) {
        rack_add_letter(rack, ld_hl_to_ml(ld, letters[k]));
        leave_list_add_leave(leave_list, rack, 1.0);
        leave_list_add_leave(leave_list, rack, 1.0);
        leave_list_add_leave(leave_list, rack, 1.0);
        rack_take_letter(rack, ld_hl_to_ml(ld, letters[k]));
      }
      rack_take_letter(rack, ld_hl_to_ml(ld, letters[j]));
    }
    rack_take_letter(rack, ld_hl_to_ml(ld, letters[i]));
  }

  rack_set_to_string(ld, player_rack, "ABCH");
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "EFGH");
  assert(racks_are_equal(expected_rack, rack));
  rack_set_to_string(ld, expected_rack, "ABCEFGH");
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 1);

  // Remove all 4 letters leaves and below from
  // consideration by making all of their counts at
  // or above the minimum target of 3 except for
  // a few for testing purposes.
  rack_reset(rack);
  for (int h = 0; h < num_letters; h++) {
    rack_add_letter(rack, ld_hl_to_ml(ld, letters[h]));
    for (int i = h + 1; i < num_letters; i++) {
      rack_add_letter(rack, ld_hl_to_ml(ld, letters[i]));
      for (int j = i + 1; j < num_letters; j++) {
        rack_add_letter(rack, ld_hl_to_ml(ld, letters[j]));
        for (int k = j + 1; k < num_letters; k++) {
          rack_add_letter(rack, ld_hl_to_ml(ld, letters[k]));
          rack_set_to_string(ld, expected_rack, "DEGH");
          bool exempt = racks_are_equal(expected_rack, rack);
          rack_set_to_string(ld, expected_rack, "DEFG");
          exempt |= racks_are_equal(expected_rack, rack);
          if (!exempt) {
            leave_list_add_leave(leave_list, rack, 1.0);
            leave_list_add_leave(leave_list, rack, 1.0);
            leave_list_add_leave(leave_list, rack, 1.0);
          }
          rack_take_letter(rack, ld_hl_to_ml(ld, letters[k]));
        }
        rack_take_letter(rack, ld_hl_to_ml(ld, letters[j]));
      }
      rack_take_letter(rack, ld_hl_to_ml(ld, letters[i]));
    }
    rack_take_letter(rack, ld_hl_to_ml(ld, letters[h]));
  }
  assert_leave_list_item(ld, klv, leave_list, "DEGH", 0, 0.0);
  assert_leave_list_item(ld, klv, leave_list, "DEFG", 0, 0.0);

  // Add EFG back to the bag
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);

  rack_set_to_string(ld, player_rack, "ABCH");
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 1);
  rack_set_to_string(ld, expected_rack, "DEGH");
  assert(racks_are_equal(expected_rack, rack));
  rack_set_to_string(ld, expected_rack, "ABCDEGH");
  assert(racks_are_equal(expected_rack, player_rack));

  // Add DEG back to the bag
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);

  rack_set_to_string(ld, rack, "DEGH");
  leave_list_add_leave(leave_list, rack, 1.0);
  leave_list_add_leave(leave_list, rack, 1.0);
  leave_list_add_leave(leave_list, rack, 1.0);
  assert_leave_list_item(ld, klv, leave_list, "DEGH", 3, 1.0);
  assert_leave_list_item(ld, klv, leave_list, "DEFG", 0, 0.0);

  // DEGH was the last available 4 letter leave and has just met
  // the minimum count, so there should be no more available
  // 4 letter leaves that fit on the rack. The leave DEFG is in the pool,
  // but does not fit on the rack, so it should not be considered.
  rack_set_to_string(ld, player_rack, "ABCH");
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  rack_set_to_string(ld, expected_rack, "CEFGH");
  assert(racks_are_equal(expected_rack, rack));
  rack_set_to_string(ld, expected_rack, "ABCEFGH");
  assert(racks_are_equal(expected_rack, player_rack));
  assert(bag_get_tiles(bag) == 1);

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

  assert_leave_list_item(ld, klv, leave_list, "S", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "ST", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STUV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "P", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "Q", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "R", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "PQRSTU", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "QRSTUV", 1, full_rack_value);

  // Test adding a subleave

  const double subleave_value = 2000.0;
  rack_set_to_string(ld, rack, "STUV");
  leave_list_add_subleave(leave_list, rack, subleave_value);

  assert_leave_list_item(ld, klv, leave_list, "S", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "T", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "U", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "V", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "ST", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SU", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TU", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "UV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STU", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "SUV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "TUV", 1, full_rack_value);
  assert_leave_list_item(ld, klv, leave_list, "STUV", 2,
                         (full_rack_value + subleave_value) / 2);

  rack_destroy(player_rack);
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

void test_leave_list(void) { test_leave_list_add_leave_and_draw_rarest(); }
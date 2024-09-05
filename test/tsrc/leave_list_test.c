#include <assert.h>

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
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, leave_str);
  int klv_index = klv_get_word_index(klv, leave);
  rack_destroy(leave);
  assert(leave_list_get_count(leave_list, klv_index) == count);
  assert(within_epsilon(leave_list_get_mean(leave_list, klv_index), mean));
}

void assert_leave_list_item_superset_count(const LetterDistribution *ld,
                                           const KLV *klv,
                                           LeaveList *leave_list,
                                           const char *leave_str,
                                           int superset_count) {
  Rack *leave = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, leave, leave_str);
  int klv_index = klv_get_word_index(klv, leave);
  rack_destroy(leave);
  assert(leave_list_get_superset_count(leave_list, klv_index) ==
         superset_count);
}

void test_leave_list_normal_leaves(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  KLV *klv = players_data_get_data(config_get_players_data(config),
                                   PLAYERS_DATA_TYPE_KLV, 0);
  int target_leave_count = 3;
  LeaveList *leave_list = leave_list_create(ld, klv, target_leave_count);

  const int number_of_leaves = klv_get_number_of_leaves(klv);
  const int player_draw_index = 0;

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

  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAAAA", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAAA", 28);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAA", 401);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ABC", 3856);

  assert_leave_list_item_superset_count(ld, klv, leave_list, "A", 177314);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "B", 173804);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "E", 177314);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "I", 177314);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "O", 177314);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AA", 29164);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "EE", 29164);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "II", 29164);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "OO", 29164);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAA", 3910);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "EEE", 3910);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "III", 3910);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "OOO", 3910);
  // The 400 possible 2-tile leaves and +1 for the leave itself
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAA", 401);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "EEEE", 401);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "IIII", 401);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "OOOO", 401);
  // dist size +1 for the leave itself
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAAA", 28);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "EEEEE", 28);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "IIIII", 28);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "OOOOO", 28);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAAAA", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "EEEEEE", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "OOOOOO", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "TTTTTT", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ABCDEF", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "VVXYYZ", 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "??EEFF", 1);

  assert_leave_list_item_superset_count(ld, klv, leave_list, "AAAAZ", 27);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ABC", 3856);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ABCF", 398);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "FG", 28764);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "BFFG", 372);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "??A", 3510);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "YYZ", 3163);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ATZ", 3536);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "ACHTZ", 27);

  Rack *rack = rack_create(ld_get_size(ld));

  // Adding the empty leave should have no effect
  rack_set_to_string(ld, rack, "");
  leave_list_add_leaves_for_rack(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 1);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leaves_for_rack(leave_list, rack, 4.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 2);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 7.0 / 2));
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "A", 1, 4.0);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "A", 177314);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leaves_for_rack(leave_list, rack, 5.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 3);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 12.0 / 3));
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "B", 1, 5.0);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "B", 173804);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leaves_for_rack(leave_list, rack, 6.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 4);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 18.0 / 4));
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "A", 2, 10.0 / 2);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "A", 177314);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leaves_for_rack(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 5);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 25.0 / 5));
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "B", 2, 12.0 / 2);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "B", 173804);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leaves_for_rack(leave_list, rack, 9.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 6);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 34.0 / 6));
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 1);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "B", 3, 21.0 / 3);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "B", 173803);

  rack_set_to_string(ld, rack, "C");
  leave_list_add_leaves_for_rack(leave_list, rack, 11.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 7);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 45.0 / 7));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "C", 1, 11.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "B", 173803);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leaves_for_rack(leave_list, rack, 15.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 8);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 60.0 / 8));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 1, 15.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 1, 15.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "DEF", 3883);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leaves_for_rack(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 9);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 77.0 / 9));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 2, 32.0 / 2);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 2,
                                        32.0 / 2);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 1);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "DEF", 3883);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leaves_for_rack(leave_list, rack, 17.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 10);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 94.0 / 10));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 3, 49.0 / 3);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 3,
                                        49.0 / 3);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 8);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "DEF", 3882);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 11);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 95.0 / 11));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 4, 50.0 / 4);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 4,
                                        50.0 / 4);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 8);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "DEF", 3882);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 12);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 96.0 / 12));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 5,
                                        51.0 / 5);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 8);
  assert_leave_list_item_superset_count(ld, klv, leave_list, "DEF", 3882);

  rack_set_to_string(ld, rack, "DEG");
  leave_list_add_leaves_for_rack(leave_list, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 13);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 99.0 / 13));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "G", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DG", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EG", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 5,
                                        51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEG", 1, 3.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 8);

  rack_set_to_string(ld, rack, "HII");
  leave_list_add_leaves_for_rack(leave_list, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 14);
  assert(
      within_epsilon(leave_list_get_empty_leave_mean(leave_list), 106.0 / 14));
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "D", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "E", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "F", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "G", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DE", 6, 54.0 / 6);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EF", 5, 51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DG", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "EG", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEF", 5,
                                        51.0 / 5);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEG", 1, 3.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "H", 1, 7.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "I", 1, 7.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "HI", 1, 7.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "II", 1, 7.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 8);

  leave_list_reset(leave_list);

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
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
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
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  rack_set_to_string(ld, rack, "DG");
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves);
  rack_set_to_string(ld, rack, "EG");
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
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
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  // The leaves D, E, and G, all reached the min count
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 3);

  rack_set_to_string(ld, rack, "H");
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  rack_set_to_string(ld, rack, "I");
  leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
  rack_set_to_string(ld, rack, "HI");
  leave_list_add_leaves_for_rack(leave_list, rack, 4.0);
  rack_set_to_string(ld, rack, "ABC");
  leave_list_add_leaves_for_rack(leave_list, rack, 6.0);
  rack_set_to_string(ld, rack, "ABCI");
  leave_list_add_leaves_for_rack(leave_list, rack, 8.0);
  // The leave I reached the min count
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
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
  leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 4);
  leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 4);
  leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
  // The leave II reached the min count
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         number_of_leaves - 5);

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 5);
  rack_set_to_string(ld, expected_rack, "EI");
  assert(racks_are_equal(expected_rack, rack));

  for (int i = 0; i < target_leave_count; i++) {
    rack_set_to_string(ld, rack, "ABCDII");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ABCEII");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ABDEII");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "ACDEII");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
    rack_set_to_string(ld, rack, "BCDEII");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
  }

  rack_reset(player_rack);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  assert(leave_list_draw_rare_leave(leave_list, ld, bag, player_rack,
                                    player_draw_index, rack));
  assert(bag_get_tiles(bag) == 2);
  rack_set_to_string(ld, expected_rack, "ABCDE");
  assert(racks_are_equal(expected_rack, rack));

  for (int i = 0; i < target_leave_count; i++) {
    rack_set_to_string(ld, rack, "ABCDE");
    leave_list_add_leaves_for_rack(leave_list, rack, 2.0);
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
        leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
        leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
        leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
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
            leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
            leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
            leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
          }
          rack_take_letter(rack, ld_hl_to_ml(ld, letters[k]));
        }
        rack_take_letter(rack, ld_hl_to_ml(ld, letters[j]));
      }
      rack_take_letter(rack, ld_hl_to_ml(ld, letters[i]));
    }
    rack_take_letter(rack, ld_hl_to_ml(ld, letters[h]));
  }
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEGH", 0, 0.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEFG", 0, 0.0);

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
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  leave_list_add_leaves_for_rack(leave_list, rack, 1.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEGH", 3, 1.0);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "DEFG", 0, 0.0);

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
  leave_list_add_leaves_for_rack(leave_list, rack, 4.0);
  rack_set_to_string(ld, rack, "JK");
  leave_list_add_leaves_for_rack(leave_list, rack, 5.0);
  rack_set_to_string(ld, rack, "JKL");
  leave_list_add_leaves_for_rack(leave_list, rack, 6.0);
  rack_set_to_string(ld, rack, "KLM");
  leave_list_add_leaves_for_rack(leave_list, rack, 7.0);

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
  leave_list_add_leaves_for_rack(leave_list, rack, full_rack_value);

  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "S", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "ST", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STUV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "P", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "Q", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "R", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "PQRSTU", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "QRSTUV", 1,
                                        full_rack_value);

  // Test adding a subleave

  const double subleave_value = 2000.0;
  rack_set_to_string(ld, rack, "STUV");
  leave_list_add_single_leave(leave_list, rack, subleave_value);

  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "S", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "T", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "U", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "V", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "ST", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SU", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TU", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "UV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STU", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "SUV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "TUV", 1,
                                        full_rack_value);
  assert_leave_list_item_count_and_mean(ld, klv, leave_list, "STUV", 2,
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
                       int expected_leaves_below_target_count, double equity) {
  rack_reset(subleave);
  rack_set_to_string(ld, subleave, subleave_str);
  int lowest_leave_count =
      leave_list_add_single_leave(leave_list, subleave, equity);
  assert(leave_list_get_leaves_below_target_count(leave_list) ==
         expected_leaves_below_target_count);
  return lowest_leave_count;
}

void assert_leave_list_valid_leaves_count(LeaveList *leave_list,
                                          const LetterDistribution *ld,
                                          const Bag *bag,
                                          const Rack *player_rack,
                                          int expected_leaves) {
  int tmc = leave_list_get_target_leave_count(leave_list);
  Rack *rare_leave = rack_duplicate(player_rack);
  rack_reset(rare_leave);
  Bag *test_bag = bag_duplicate(bag);
  Rack *test_player_rack = rack_duplicate(player_rack);
  int available_leaves = 0;
  while (leave_list_draw_rare_leave(leave_list, ld, test_bag, test_player_rack,
                                    0, rare_leave)) {
    available_leaves++;
    for (int i = 0; i < tmc; i++) {
      leave_list_add_single_leave(leave_list, rare_leave, 0.0);
    }
    bag_copy(test_bag, bag);
    rack_copy(test_player_rack, player_rack);
  }
  assert(available_leaves == expected_leaves);
  rack_destroy(rare_leave);
  rack_destroy(test_player_rack);
  bag_destroy(test_bag);
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
  LeaveList *ll = leave_list_create(ld, klv, tmc);

  const int number_of_leaves = klv_get_number_of_leaves(klv);
  const int player_draw_index = 0;

  int lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    rack_set_to_string(ld, player_rack, "A");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    assert(leave_list_get_attempted_rare_draws(ll) == 0);
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "AAAABB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    assert(leave_list_get_attempted_rare_draws(ll) == 0);
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "BBBBBB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    assert(leave_list_get_attempted_rare_draws(ll) == 0);
    bag_reset(ld, bag);

    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "A", 21);
    assert(leave_list_add_sas(ll, ld, sl, "B", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "B", 21);
    assert(leave_list_add_sas(ll, ld, sl, "AA", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "AA", 15);
    assert(leave_list_add_sas(ll, ld, sl, "AB", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "AB", 15);
    assert(leave_list_add_sas(ll, ld, sl, "BB", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "BB", 15);
    assert(leave_list_add_sas(ll, ld, sl, "AAA", lutmc, 1.0) == i);
    assert_leave_list_item_superset_count(ld, klv, ll, "AAA", 10);
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

    rack_set_to_string(ld, player_rack, "A");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "AAAABB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "BBBBBB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);

    assert(leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc, 1.0) == i);
    assert(leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc, 1.0) == i + 1);

    rack_set_to_string(ld, player_rack, "A");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "AAAABB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);
    rack_set_to_string(ld, player_rack, "BBBBBB");
    assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                      player_draw_index, rare_leave));
    bag_reset(ld, bag);
  }

  rack_set_to_string(ld, player_rack, "A");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "AAAABB");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "BBBBBB");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  bag_reset(ld, bag);

  lutmc--;
  assert(leave_list_add_sas(ll, ld, sl, "A", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "A", 20);
  assert(leave_list_add_sas(ll, ld, sl, "B", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "B", 20);
  assert(leave_list_add_sas(ll, ld, sl, "AA", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "AA", 14);
  assert(leave_list_add_sas(ll, ld, sl, "AB", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "AB", 14);
  assert(leave_list_add_sas(ll, ld, sl, "BB", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "BB", 14);
  assert(leave_list_add_sas(ll, ld, sl, "AAA", lutmc--, 1.0) == tmc - 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "AAA", 9);
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

  // Remaining leaves under count:
  // AABBBB
  // ABBBBB
  // BBBBBB
  assert_leave_list_item_superset_count(ld, klv, ll, "A", 2);
  assert_leave_list_item_superset_count(ld, klv, ll, "AA", 1);
  assert_leave_list_item_superset_count(ld, klv, ll, "AAA", 0);
  assert_leave_list_item_superset_count(ld, klv, ll, "AAAA", 0);
  assert_leave_list_item_superset_count(ld, klv, ll, "B", 3);
  assert_leave_list_item_superset_count(ld, klv, ll, "BB", 3);
  assert_leave_list_item_superset_count(ld, klv, ll, "BBB", 3);
  assert_leave_list_item_superset_count(ld, klv, ll, "BBBB", 3);
  assert_leave_list_item_superset_count(ld, klv, ll, "BBBBB", 2);
  assert_leave_list_item_superset_count(ld, klv, ll, "AB", 2);
  assert_leave_list_item_superset_count(ld, klv, ll, "ABB", 2);
  assert_leave_list_item_superset_count(ld, klv, ll, "ABBB", 2);

  rack_set_to_string(ld, player_rack, "A");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 5);
  bag_reset(ld, bag);

  rack_set_to_string(ld, player_rack, "AAAABB");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 11);

  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "BBBBBB");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 5);
  bag_reset(ld, bag);

  assert(leave_list_add_sas(ll, ld, sl, "ABBBBB", lutmc--, 1.0) == tmc - 1);
  assert(leave_list_add_sas(ll, ld, sl, "BBBBBB", lutmc--, 1.0) == tmc - 1);

  rack_set_to_string(ld, player_rack, "A");
  assert(leave_list_draw_rare_leave(ll, ld, bag, player_rack, player_draw_index,
                                    rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 16);
  bag_reset(ld, bag);

  rack_set_to_string(ld, player_rack, "AAAABB");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 11);
  bag_reset(ld, bag);

  rack_set_to_string(ld, player_rack, "BBBBBB");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 12);
  bag_reset(ld, bag);

  assert(leave_list_add_sas(ll, ld, sl, "AABBBB", lutmc--, 1.0) == tmc);

  rack_set_to_string(ld, player_rack, "A");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 6);
  bag_reset(ld, bag);

  rack_set_to_string(ld, player_rack, "AAAABB");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 3);
  bag_reset(ld, bag);

  rack_set_to_string(ld, player_rack, "BBBBBB");
  assert(!leave_list_draw_rare_leave(ll, ld, bag, player_rack,
                                     player_draw_index, rare_leave));
  assert(leave_list_get_attempted_rare_draws(ll) == 6);
  bag_reset(ld, bag);

  leave_list_reset(ll);

  lutmc = number_of_leaves;

  for (int i = 0; i < tmc - 1; i++) {
    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc, 1.0) == 0);
  }
  for (int i = 0; i < 5; i++) {
    assert(leave_list_add_sas(ll, ld, sl, "A", lutmc - 1, 1.0) == 0);
  }

  leave_list_reset(ll);
  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "A");
  // All leaves are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 27);

  leave_list_reset(ll);
  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "B");
  // All leaves are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 27);

  leave_list_reset(ll);
  bag_reset(ld, bag);
  rack_set_to_string(ld, player_rack, "AA");
  // The leave of BBBBBB is not available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 26);

  leave_list_reset(ll);
  clear_bag(bag);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  rack_set_to_string(ld, player_rack, "AA");
  // The leave of BBBBBB is not available because it doesn't fit in the rack
  // The leave of AABBBBB and ABBBBB are not available because the bag only has
  // 4 Bs.
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 24);

  leave_list_reset(ll);
  clear_bag(bag);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  rack_set_to_string(ld, player_rack, "");
  // No leaves with a B are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 6);

  leave_list_reset(ll);
  clear_bag(bag);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  rack_set_to_string(ld, player_rack, "AAA");
  // No leaves with a B are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 6);

  leave_list_reset(ll);
  clear_bag(bag);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  rack_set_to_string(ld, player_rack, "");
  // No leaves with 4 or more of a single tile are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 15);

  leave_list_reset(ll);
  clear_bag(bag);
  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);
  rack_set_to_string(ld, player_rack, "AAB");
  // No leaves with 4 or more of a single tile are available
  assert_leave_list_valid_leaves_count(ll, ld, bag, player_rack, 15);

  bag_destroy(bag);
  rack_destroy(player_rack);
  rack_destroy(rare_leave);
  rack_destroy(sl);
  leave_list_destroy(ll);
  config_destroy(config);
}

void test_leave_list(void) {
  // FIXME: uncomment
  // test_leave_list_normal_leaves();
  test_leave_list_small_leaves();
}
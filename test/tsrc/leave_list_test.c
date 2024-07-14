#include <assert.h>

#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

// FIXME: remove
#include "../../src/str/rack_string.h"

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
  if (expected_count_index >= 0) {
    assert(actual_count_index == expected_count_index);
  }
  const Rack *actual_rack = leave_list_get_rack(leave_list, actual_count_index);

  // FIXME: remove
  // StringBuilder *sb = string_builder_create();
  // string_builder_add_rack(sb, actual_rack, ld);
  // string_builder_add_string(sb, "\n");
  // string_builder_add_rack(sb, expected_rack, ld);
  // string_builder_add_string(sb, "\n");
  // printf("%s\n", string_builder_peek(sb));
  // string_builder_destroy(sb);

  assert(racks_are_equal(expected_rack, actual_rack));
  rack_destroy(expected_rack);
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
  LeaveList *leave_list = leave_list_create(ld, klv);

  int number_of_leaves = klv_get_number_of_leaves(klv);

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
  leave_list_add_leave(leave_list, klv, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 1);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, klv, rack, 4.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 2);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 7.0 / 2));
  assert_leave_list_item(ld, klv, leave_list, "A", number_of_leaves - 1, 1,
                         4.0);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, klv, rack, 5.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 3);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 12.0 / 3));
  assert_leave_list_item(ld, klv, leave_list, "B", number_of_leaves - 2, 1,
                         5.0);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, klv, rack, 6.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 4);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 18.0 / 4));
  assert_leave_list_item(ld, klv, leave_list, "A", number_of_leaves - 1, 2,
                         10.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, klv, rack, 7.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 5);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 25.0 / 5));
  assert_leave_list_item(ld, klv, leave_list, "B", number_of_leaves - 2, 2,
                         12.0 / 2);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, klv, rack, 9.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 6);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 34.0 / 6));
  assert_leave_list_item(ld, klv, leave_list, "B", number_of_leaves - 1, 3,
                         21.0 / 3);

  rack_set_to_string(ld, rack, "C");
  leave_list_add_leave(leave_list, klv, rack, 11.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 7);
  assert(within_epsilon(leave_list_get_empty_leave_mean(leave_list), 45.0 / 7));
  assert_leave_list_item(ld, klv, leave_list, "C", number_of_leaves - 3, 1,
                         11.0);

  rack_set_to_string(ld, rack, "DEF");
  leave_list_add_leave(leave_list, klv, rack, 15.0);
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
  leave_list_add_leave(leave_list, klv, rack, 17.0);
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
  leave_list_add_leave(leave_list, klv, rack, 17.0);
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
  leave_list_add_leave(leave_list, klv, rack, 1.0);
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
  leave_list_add_leave(leave_list, klv, rack, 1.0);
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
  leave_list_add_leave(leave_list, klv, rack, 3.0);
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
  leave_list_add_leave(leave_list, klv, rack, 7.0);
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

  rack_destroy(rack);
  leave_list_destroy(leave_list);
  config_destroy(config);
}

void test_leave_list(void) { test_leave_list_add_leave(); }
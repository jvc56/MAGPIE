#include <assert.h>

#include "../../src/ent/klv.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/leave_list.h"

#include "../../src/impl/config.h"

#include "test_util.h"

// FIXME: remove
#include "../../src/str/rack_string.h"

void assert_leave_list_item_rack(const LetterDistribution *ld, const KLV *klv,
                                 LeaveList *leave_list,
                                 const char *expected_rack_str,
                                 int count_index) {
  Rack *expected_rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, expected_rack, expected_rack_str);
  // We can use the count index as the KLV index for a new leave list
  // because the leave list items are copied from the klv ordering
  // to the count ordering upon creation.
  if (count_index < 0) {
    count_index = klv_get_word_index(klv, expected_rack);
  }
  const Rack *actual_rack =
      leave_list_get_rack_by_count_index(leave_list, count_index);

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

  assert_leave_list_item_rack(ld, klv, leave_list, "A", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "B", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "?", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "Z", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "VWWXYZ", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "??", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "??AA", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "??AABB", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "ABCD", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "XYYZ", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "XZ", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "VX", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "AGHM", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "ABC", -1);
  assert_leave_list_item_rack(ld, klv, leave_list, "?ABCD", -1);

  Rack *rack = rack_create(ld_get_size(ld));

  // Adding the empty leave should have no effect
  rack_set_to_string(ld, rack, "");
  leave_list_add_leave(leave_list, klv, rack, 3.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 1);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, klv, rack, 4.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 2);
  assert_leave_list_item_rack(ld, klv, leave_list, "A", number_of_leaves - 1);

  rack_set_to_string(ld, rack, "B");
  leave_list_add_leave(leave_list, klv, rack, 5.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 3);
  assert_leave_list_item_rack(ld, klv, leave_list, "B", number_of_leaves - 2);

  rack_set_to_string(ld, rack, "A");
  leave_list_add_leave(leave_list, klv, rack, 6.0);
  assert(leave_list_get_empty_leave_count(leave_list) == 4);
  assert_leave_list_item_rack(ld, klv, leave_list, "A", number_of_leaves - 1);

  rack_destroy(rack);
  leave_list_destroy(leave_list);
  config_destroy(config);
}

void test_leave_list(void) { test_leave_list_add_leave(); }
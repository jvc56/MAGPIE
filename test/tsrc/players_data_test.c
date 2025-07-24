#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "../../src/def/move_defs.h"
#include "../../src/def/players_data_defs.h"

#include "../../src/ent/equity.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/players_data.h"

#include "../../src/util/io_util.h"
#include "../../src/util/string_util.h"

#include "test_util.h"

void assert_players_data(const PlayersData *players_data,
                         players_data_t players_data_type,
                         const char *p1_data_name, const char *p2_data_name) {
  bool data_is_shared = strings_equal(p1_data_name, p2_data_name);
  assert(players_data_get_is_shared(players_data, players_data_type) ==
         data_is_shared);
  assert_strings_equal(
      players_data_get_data_name(players_data, players_data_type, 0),
      p1_data_name);
  assert_strings_equal(
      players_data_get_data_name(players_data, players_data_type, 1),
      p2_data_name);
  assert((players_data_get_data(players_data, players_data_type, 0) ==
          players_data_get_data(players_data, players_data_type, 1)) ==
         data_is_shared);
}

void test_for_data_type(const char **data_names, const char *data_paths,
                        players_data_t players_data_type,
                        int number_of_data_names, ErrorStack *error_stack) {
  PlayersData *players_data = players_data_create();
  // Verify initial NULL values
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    assert(!players_data_get_is_shared(players_data, (players_data_t)i));
    for (int j = 0; j < 2; j++) {
      assert(!players_data_get_data_name(players_data, (players_data_t)i, j));
    }
  }

  const void *previous_data_1 = NULL;
  const char *previous_data_name_1 = NULL;
  const void *previous_data_2 = NULL;
  const char *previous_data_name_2 = NULL;
  for (int i = 0; i < number_of_data_names; i += 2) {
    players_data_set(players_data, players_data_type, data_paths, data_names[i],
                     data_names[i + 1], error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      assert(false);
    }
    assert_players_data(players_data, players_data_type, data_names[i],
                        data_names[i + 1]);
    if (i > 0) {
      for (int player_index = 0; player_index < 2; player_index++) {
        if (strings_equal(data_names[i + player_index], previous_data_name_1)) {
          assert(players_data_get_data(players_data, players_data_type,
                                       player_index) == previous_data_1);
        }
        if (strings_equal(data_names[i + player_index], previous_data_name_2)) {
          assert(players_data_get_data(players_data, players_data_type,
                                       player_index) == previous_data_2);
        }
      }
    }

    previous_data_1 = players_data_get_data(players_data, players_data_type, 0);
    previous_data_name_1 = data_names[i];
    previous_data_2 = players_data_get_data(players_data, players_data_type, 1);
    previous_data_name_2 = data_names[i + 1];
  }
  players_data_destroy(players_data);
}

void test_unshared_data(void) {
  PlayersData *players_data = players_data_create();

  const char *p1_name = "Alice";
  move_sort_t p1_move_sort_type = MOVE_SORT_SCORE;
  move_record_t p1_move_record_type = MOVE_RECORD_ALL;
  players_data_set_name(players_data, 0, p1_name);
  players_data_set_move_sort_type(players_data, 0, p1_move_sort_type);
  players_data_set_move_record_type(players_data, 0, p1_move_record_type);

  assert_strings_equal(players_data_get_name(players_data, 0), p1_name);
  assert(players_data_get_move_sort_type(players_data, 0) == p1_move_sort_type);
  assert(players_data_get_move_record_type(players_data, 0) ==
         p1_move_record_type);

  const char *p2_name = "Bob";
  move_sort_t p2_move_sort_type = MOVE_SORT_SCORE;
  move_record_t p2_move_record_type = MOVE_RECORD_ALL;
  players_data_set_name(players_data, 1, p2_name);
  players_data_set_move_sort_type(players_data, 1, p2_move_sort_type);
  players_data_set_move_record_type(players_data, 1, p2_move_record_type);

  assert_strings_equal(players_data_get_name(players_data, 1), p2_name);
  assert(players_data_get_move_sort_type(players_data, 1) == p2_move_sort_type);
  assert(players_data_get_move_record_type(players_data, 1) ==
         p2_move_record_type);

  p2_name = "Charlie";
  players_data_set_name(players_data, 1, p2_name);
  assert_strings_equal(players_data_get_name(players_data, 1), p2_name);

  players_data_destroy(players_data);
}

void test_reloaded_data(void) {
  PlayersData *players_data = players_data_create();
  ErrorStack *error_stack = error_stack_create();

  players_data_set_name(players_data, 0, "Alice");
  players_data_set_move_sort_type(players_data, 0, MOVE_SORT_SCORE);
  players_data_set_move_record_type(players_data, 0, MOVE_RECORD_ALL);

  players_data_set_name(players_data, 1, "Bob");
  players_data_set_move_sort_type(players_data, 1, MOVE_SORT_SCORE);
  players_data_set_move_record_type(players_data, 1, MOVE_RECORD_ALL);

  players_data_set(players_data, PLAYERS_DATA_TYPE_KLV, DEFAULT_TEST_DATA_PATH,
                   "CSW21", "CSW21", error_stack);
  assert(error_stack_is_empty(error_stack));

  const KLV *klv1 = players_data_get_klv(players_data, 0);

  const int leave_index = 7;
  const Equity old_leave_value = klv_get_indexed_leave_value(klv1, leave_index);
  const Equity new_leave_value = double_to_equity(17.0);

  klv_set_indexed_leave_value(klv1, leave_index, new_leave_value);

  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_KLV, 1) == klv1);
  assert(klv_get_indexed_leave_value(klv1, leave_index) == new_leave_value);

  players_data_reload(players_data, PLAYERS_DATA_TYPE_KLV,
                      DEFAULT_TEST_DATA_PATH, error_stack);
  assert(error_stack_is_empty(error_stack));

  const KLV *klv2 = players_data_get_klv(players_data, 0);

  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_KLV, 0) != klv1);
  assert(players_data_get_data(players_data, PLAYERS_DATA_TYPE_KLV, 1) != klv1);
  assert(klv_get_indexed_leave_value(klv2, leave_index) == old_leave_value);
  error_stack_destroy(error_stack);
  players_data_destroy(players_data);
}

void test_null_data(void) {
  PlayersData *players_data = players_data_create();
  ErrorStack *error_stack = error_stack_create();

  // Confirm that WMP can be set to NULL without errors.

  players_data_set(players_data, PLAYERS_DATA_TYPE_WMP, DEFAULT_TEST_DATA_PATH,
                   "CSW21", "CSW21", error_stack);
  assert_players_data(players_data, PLAYERS_DATA_TYPE_WMP, "CSW21", "CSW21");
  players_data_set(players_data, PLAYERS_DATA_TYPE_WMP, DEFAULT_TEST_DATA_PATH,
                   NULL, NULL, error_stack);
  assert_players_data(players_data, PLAYERS_DATA_TYPE_WMP, NULL, NULL);

  players_data_set(players_data, PLAYERS_DATA_TYPE_WMP, DEFAULT_TEST_DATA_PATH,
                   "CSW21", "CSW21", error_stack);
  assert_players_data(players_data, PLAYERS_DATA_TYPE_WMP, "CSW21", "CSW21");
  players_data_set(players_data, PLAYERS_DATA_TYPE_WMP, DEFAULT_TEST_DATA_PATH,
                   NULL, NULL, error_stack);
  assert_players_data(players_data, PLAYERS_DATA_TYPE_WMP, NULL, NULL);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  error_stack_destroy(error_stack);
  players_data_destroy(players_data);
}

void test_players_data(void) {
  ErrorStack *error_stack = error_stack_create();
  const char *data_names[] = {
      "CSW21", "CSW21", "NWL20",  "NWL20", "CSW21",  "NWL20", "CSW21",
      "CSW21", "CSW21", "CSW21",  "CSW21", "NWL20",  "NWL20", "NWL20",
      "CSW21", "NWL20", "NWL20",  "CSW21", "OSPS49", "DISC2", "DISC2",
      "NWL20", "DISC2", "OSPS49", "CSW21", "DISC2",  "NWL20", "DISC2"};
  int number_of_data_names = sizeof(data_names) / sizeof(data_names[0]);
  test_for_data_type(data_names, DEFAULT_TEST_DATA_PATH, PLAYERS_DATA_TYPE_KWG,
                     number_of_data_names, error_stack);
  test_for_data_type(data_names, DEFAULT_TEST_DATA_PATH, PLAYERS_DATA_TYPE_KLV,
                     number_of_data_names, error_stack);
  test_unshared_data();
  test_reloaded_data();
  test_null_data();
  error_stack_destroy(error_stack);
}

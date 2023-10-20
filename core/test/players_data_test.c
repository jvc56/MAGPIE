#include <assert.h>

#include "../src/log.h"
#include "../src/players_data.h"

#include "test_util.h"

void assert_players_data(PlayersData *players_data,
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

void test_for_data_type(players_data_t players_data_type,
                        const char **data_names, int number_of_data_names) {
  PlayersData *players_data = create_players_data();
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
    set_players_data(players_data, players_data_type, data_names[i],
                     data_names[i + 1]);
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
  destroy_players_data(players_data);
}

void test_players_data() {
  const char *data_names[] = {
      "CSW21", "CSW21", "NWL20",  "NWL20", "CSW21",  "NWL20", "CSW21",
      "CSW21", "CSW21", "CSW21",  "CSW21", "NWL20",  "NWL20", "NWL20",
      "CSW21", "NWL20", "NWL20",  "CSW21", "OSPS44", "DISC2", "DISC2",
      "NWL20", "DISC2", "OSPS44", "CSW21", "DISC2",  "NWL20", "DISC2"};
  int number_of_data_names = sizeof(data_names) / sizeof(data_names[0]);
  assert(number_of_data_names % 2 == 0);
  test_for_data_type(PLAYERS_DATA_TYPE_KWG, data_names, number_of_data_names);
  test_for_data_type(PLAYERS_DATA_TYPE_KLV, data_names, number_of_data_names);
}

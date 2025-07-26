#include "../src/ent/equity.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>

void take_set(const LetterDistribution *ld, LeaveMap *leave_map, Rack *rack,
              const char *letter, const int expected_take_index,
              const double value) {
  leave_map_take_letter_and_update_current_index(leave_map, rack,
                                                 ld_hl_to_ml(ld, letter));
  int current_index = leave_map_get_current_index(leave_map);
  assert(current_index == expected_take_index);
  leave_map_set_current_value(leave_map, double_to_equity(value));
}

void take_set_add(const LetterDistribution *ld, LeaveMap *leave_map, Rack *rack,
                  const char *letter, const int expected_take_index,
                  const int expected_add_index, const double value,
                  const bool set) {
  leave_map_take_letter_and_update_current_index(leave_map, rack,
                                                 ld_hl_to_ml(ld, letter));
  assert(leave_map_get_current_index(leave_map) == expected_take_index);
  Equity value_equity = double_to_equity(value);
  if (set) {
    leave_map_set_current_value(leave_map, value_equity);
  } else {
    assert(leave_map_get_current_value(leave_map) == value_equity);
  }
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, letter));
  assert(leave_map_get_current_index(leave_map) == expected_add_index);
}

void test_leave_map(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  LeaveMap *leave_map = leave_map_create(rack_get_dist_size(rack));

  rack_set_to_string(ld, rack, "ABCDEFG");
  leave_map_init(rack, leave_map);

  // Should be (number of tiles ^ 2) - 1
  // 1111111
  assert(leave_map_get_current_index(leave_map) == 127);

  take_set_add(ld, leave_map, rack, "A", 126, 127, 7.0, true);
  take_set_add(ld, leave_map, rack, "B", 125, 127, 8.0, true);
  take_set_add(ld, leave_map, rack, "E", 111, 127, 9.0, true);
  take_set_add(ld, leave_map, rack, "G", 63, 127, 10.0, true);

  take_set_add(ld, leave_map, rack, "A", 126, 127, 7.0, false);
  take_set_add(ld, leave_map, rack, "B", 125, 127, 8.0, false);
  take_set_add(ld, leave_map, rack, "E", 111, 127, 9.0, false);
  take_set_add(ld, leave_map, rack, "G", 63, 127, 10.0, false);

  // Multiple letters
  rack_set_to_string(ld, rack, "DDDIIUU");
  leave_map_init(rack, leave_map);

  leave_map_set_current_value(leave_map, double_to_equity(100.0));
  // 1111011
  // DDIIUU
  take_set(ld, leave_map, rack, "D", 123, 11.0);
  // 0111011
  // DDIIU
  take_set(ld, leave_map, rack, "U", 59, 12.0);
  // 0111001
  // DIIU
  take_set(ld, leave_map, rack, "D", 57, 13.0);
  // 0101001
  // DIU
  take_set(ld, leave_map, rack, "I", 41, 14.0);
  // 0101000
  // IU
  take_set(ld, leave_map, rack, "D", 40, 15.0);
  // 0100000
  // U
  take_set(ld, leave_map, rack, "I", 32, 16.0);
  // 0000000
  // empty leave
  take_set(ld, leave_map, rack, "U", 0, 17.0);

  // Add back in a different order and check the value
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "D"));
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "I"));
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "U"));
  assert(leave_map_get_current_index(leave_map) == 41);
  assert(leave_map_get_current_value(leave_map) == double_to_equity(14.0));

  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "D"));
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "I"));
  assert(leave_map_get_current_index(leave_map) == 59);
  assert(leave_map_get_current_value(leave_map) == double_to_equity(12.0));

  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "U"));
  leave_map_add_letter_and_update_current_index(leave_map, rack,
                                                ld_hl_to_ml(ld, "D"));
  assert(leave_map_get_current_index(leave_map) == 127);
  assert(leave_map_get_current_value(leave_map) == double_to_equity(100.0));

  rack_destroy(rack);
  leave_map_destroy(leave_map);
  config_destroy(config);
}

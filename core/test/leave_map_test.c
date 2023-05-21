#include <assert.h>
#include <stdio.h>

#include "../src/alphabet.h"
#include "../src/leave_map.h"
#include "../src/rack.h"

#include "rack_test.h"
#include "superconfig.h"
#include "test_util.h"

void take_set_add(LeaveMap * leave_map, Rack * rack, Alphabet * alphabet, const char letter, int expected_take_index, int expected_add_index, float value) {
    take_letter_and_update_current_value(leave_map, rack, alphabet->vals[letter]);
    assert(leave_map->current_index == expected_take_index);
    if (value > 0) {
    set_current_value(leave_map, value);

    }

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals[letter]);
    assert(leave_map->current_index == expected_add_index);
}

void test_leave_map(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Alphabet * alphabet = config->kwg->alphabet;
    Rack * rack = create_rack(config->letter_distribution->size);

    LeaveMap * leave_map = create_leave_map(rack->array_size);

    set_rack_to_string(rack, "ABCDEFG", alphabet);
    init_leave_map(leave_map, rack);

    // Should be (number of tiles ^ 2) - 1
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['A']);
    // 1111110
    assert(leave_map->current_index == 126);
    set_current_value(leave_map, 7.0);

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['A']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['B']);
    // 1111101
    assert(leave_map->current_index == 125);
    set_current_value(leave_map, 8.0);

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['B']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['E']);
    // 1101111
    assert(leave_map->current_index == 111);
    set_current_value(leave_map, 9.0);

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['E']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['G']);
    // 0111111
    assert(leave_map->current_index == 63);
    set_current_value(leave_map, 10.0);

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['G']);
    // 1111111
    assert(leave_map->current_index == 127);


    float value;
    // Get the leave values
    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['A']);
    // 1111110
    assert(leave_map->current_index == 126);
    value = get_current_value(leave_map);
    assert(within_epsilon_float(value, 7.0));

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['A']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['B']);
    // 1111101
    assert(leave_map->current_index == 125);
    value = get_current_value(leave_map);
    assert(within_epsilon_float(value, 8.0));

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['B']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['E']);
    // 1101111
    assert(leave_map->current_index == 111);
    value = get_current_value(leave_map);
    assert(within_epsilon_float(value, 9.0));

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['E']);
    // 1111111
    assert(leave_map->current_index == 127);

    take_letter_and_update_current_value(leave_map, rack, alphabet->vals['G']);
    // 0111111
    assert(leave_map->current_index == 63);
    value = get_current_value(leave_map);
    assert(within_epsilon_float(value, 10.0));

    add_letter_and_update_current_value(leave_map, rack, alphabet->vals['G']);
    // 1111111
    assert(leave_map->current_index == 127);

    // Multiple letters
    set_rack_to_string(rack, "DDDIIUU", alphabet);
    init_leave_map(leave_map, rack);
}

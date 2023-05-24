#include <assert.h>
#include <stdio.h>

#include "../src/alphabet.h"
#include "../src/leave_map.h"
#include "../src/rack.h"

#include "rack_test.h"
#include "superconfig.h"
#include "test_util.h"

void take_set(LeaveMap * leave_map, Rack * rack, Alphabet * alphabet, uint64_t letter, int expected_take_index, float value) {
    take_letter_and_update_current_index(leave_map, rack, alphabet->vals[letter]);
    if (leave_map->current_index != expected_take_index) {
        printf("%d != %d\n", leave_map->current_index, expected_take_index);
    }
    assert(leave_map->current_index == expected_take_index);
    set_current_value(leave_map, value);
}

void take_set_add(LeaveMap * leave_map, Rack * rack, Alphabet * alphabet, uint64_t letter, int expected_take_index, int expected_add_index, float value, int set) {
    take_letter_and_update_current_index(leave_map, rack, alphabet->vals[letter]);
    assert(leave_map->current_index == expected_take_index);
    if (set) {
        set_current_value(leave_map, value);
    } else {
        assert(within_epsilon_float(get_current_value(leave_map), value));
    }
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals[letter]);
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

    take_set_add(leave_map, rack, alphabet, 'A', 126, 127, 7.0, 1);
    take_set_add(leave_map, rack, alphabet, 'B', 125, 127, 8.0, 1);
    take_set_add(leave_map, rack, alphabet, 'E', 111, 127, 9.0, 1);
    take_set_add(leave_map, rack, alphabet, 'G', 63, 127, 10.0, 1);

    take_set_add(leave_map, rack, alphabet, 'A', 126, 127, 7.0, 0);
    take_set_add(leave_map, rack, alphabet, 'B', 125, 127, 8.0, 0);
    take_set_add(leave_map, rack, alphabet, 'E', 111, 127, 9.0, 0);
    take_set_add(leave_map, rack, alphabet, 'G', 63, 127, 10.0, 0);

    // Multiple letters
    set_rack_to_string(rack, "DDDIIUU", alphabet);
    init_leave_map(leave_map, rack);

    set_current_value(leave_map, 100.0);
    // 1111011
    // DDIIUU
    take_set(leave_map, rack, alphabet, 'D', 123, 11.0);
    // 0111011
    // DDIIU
    take_set(leave_map, rack, alphabet, 'U', 59, 12.0);
    // 0111001
    // DIIU
    take_set(leave_map, rack, alphabet, 'D', 57, 13.0);
    // 0101001
    // DIU
    take_set(leave_map, rack, alphabet, 'I', 41, 14.0);
    // 0101000
    // IU
    take_set(leave_map, rack, alphabet, 'D', 40, 15.0);
    // 0100000
    // U
    take_set(leave_map, rack, alphabet, 'I', 32, 16.0);
    // 0000000
    // empty leave
    take_set(leave_map, rack, alphabet, 'U', 0, 17.0);

    // Add back in a different order and check the value
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['D']);
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['I']);
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['U']);
    assert(leave_map->current_index == 41);
    assert(within_epsilon_float(get_current_value(leave_map), 14.0));

    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['D']);
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['I']);
    assert(leave_map->current_index == 59);
    assert(within_epsilon_float(get_current_value(leave_map), 12.0));

    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['U']);
    add_letter_and_update_current_index(leave_map, rack, alphabet->vals['D']);
    assert(leave_map->current_index == 127);
    assert(within_epsilon_float(get_current_value(leave_map), 100.0));

    destroy_rack(rack);
    destroy_leave_map(leave_map);
}
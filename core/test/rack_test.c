#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/alphabet.h"
#include "../src/config.h"
#include "../src/constants.h"
#include "../src/gaddag.h"
#include "../src/letter_distribution.h"
#include "../src/rack.h"

#include "rack_test.h"
#include "test_util.h"
#include "superconfig.h"

int equal_rack(Rack * expected_rack, Rack * actual_rack) {
    if (expected_rack->empty != actual_rack->empty) {
        return 0;
    }
    if (expected_rack->number_of_letters != actual_rack->number_of_letters) {
        return 0;
    }
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        if (expected_rack->array[i] != actual_rack->array[i]) {
            return 0;
        }
    }
    return 1;
}


void test_rack_main(SuperConfig * superconfig) {
    Config * config = get_america_config(superconfig);
    Rack * rack = create_rack();
    Rack * expected_rack = create_rack();

    // Test score on rack
    set_rack_to_string(rack, "ABCDEFG", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 16);
    set_rack_to_string(rack, "XYZ", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 22);
    set_rack_to_string(rack, "??", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 0);
    set_rack_to_string(rack, "?QWERTY", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 21);
    set_rack_to_string(rack, "RETINAO", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 7);
    set_rack_to_string(rack, "AABBEWW", config->gaddag->alphabet);
    assert(score_on_rack(config->letter_distribution, rack) == 17);

    set_rack_to_string(rack, "AENPPSW", config->gaddag->alphabet);

    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        expected_rack->array[i] = 0;
    }
    expected_rack->array[0] = 1;
    expected_rack->array[4] = 1;
    expected_rack->array[13] = 1;
    expected_rack->array[15] = 2;
    expected_rack->array[18] = 1;
    expected_rack->array[22] = 1;
    expected_rack->empty = 0;
    expected_rack->number_of_letters = 7;

    assert(equal_rack(expected_rack, rack));

    take_letter_from_rack(rack, 15);
    expected_rack->array[15] = 1;
    expected_rack->number_of_letters = 6;
    assert(equal_rack(expected_rack, rack));

    take_letter_from_rack(rack, 15);
    expected_rack->array[15] = 0;
    expected_rack->number_of_letters = 5;
    assert(equal_rack(expected_rack, rack));

    take_letter_from_rack(rack, 0);
    assert(!rack->empty);
    take_letter_from_rack(rack, 4);
    assert(!rack->empty);
    take_letter_from_rack(rack, 13);
    assert(!rack->empty);
    take_letter_from_rack(rack, 18);
    assert(!rack->empty);
    take_letter_from_rack(rack, 22);
    assert(rack->empty);

    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        expected_rack->array[i] = 0;
    }
    expected_rack->empty = 1;
    expected_rack->number_of_letters = 0;

    assert(equal_rack(expected_rack, rack));


    add_letter_to_rack(rack, 13, rack->number_of_nonzero_indexes);
    take_letter_from_rack(rack, 13);
    add_letter_to_rack(rack, 13, rack->number_of_nonzero_indexes);
    add_letter_to_rack(rack, 13, rack->number_of_nonzero_indexes);

    expected_rack->array[13] = 2;
    expected_rack->empty = 0;
    expected_rack->number_of_letters = 2;

    assert(equal_rack(expected_rack, rack));

    destroy_rack(rack);
    destroy_rack(expected_rack);
}

void test_rack_nonzero_indexes_take_and_add(Config * config, Rack * rack) {
    int nonzero_array_index;
    // Test adding letter to rack.
    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'C'), 0);
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->number_of_nonzero_indexes == 1);
    assert(rack->number_of_letters == 1);
    
    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'D'), 1);
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->letter_to_array_nonzero_index[3] == 1);
    assert(rack->number_of_nonzero_indexes == 2);
    assert(rack->number_of_letters == 2);

    // Nonzero array index should not matter since there is
    // already a C on the rack.
    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'C'), -100);
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->letter_to_array_nonzero_index[3] == 1);
    assert(rack->number_of_nonzero_indexes == 2);
    assert(rack->number_of_letters == 3);

    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'E'), 1);
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 3);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->letter_to_array_nonzero_index[3] == 2);
    assert(rack->letter_to_array_nonzero_index[4] == 1);
    assert(rack->number_of_nonzero_indexes == 3);
    assert(rack->number_of_letters == 4);

    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'F'), 0);
    assert(rack->array_nonzero_indexes[0] == 5);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 3);
    assert(rack->array_nonzero_indexes[3] == 2);
    assert(rack->letter_to_array_nonzero_index[2] == 3);
    assert(rack->letter_to_array_nonzero_index[3] == 2);
    assert(rack->letter_to_array_nonzero_index[4] == 1);
    assert(rack->letter_to_array_nonzero_index[5] == 0);
    assert(rack->number_of_nonzero_indexes == 4);
    assert(rack->number_of_letters == 5);

    add_letter_to_rack(rack, val(config->gaddag->alphabet, '?'), 2);
    assert(rack->array_nonzero_indexes[0] == 5);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->array_nonzero_indexes[3] == 2);
    assert(rack->array_nonzero_indexes[4] == 3);
    assert(rack->number_of_nonzero_indexes == 5);
    assert(rack->number_of_letters == 6);

    // Should matter for same reasons as above.
    add_letter_to_rack(rack, val(config->gaddag->alphabet, 'E'), 10000);
    assert(rack->array_nonzero_indexes[0] == 5);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->array_nonzero_indexes[3] == 2);
    assert(rack->array_nonzero_indexes[4] == 3);
    assert(rack->number_of_nonzero_indexes == 5);
    assert(rack->number_of_letters == 7);

    // Test taking letter from rack.
    take_letter_from_rack(rack, val(config->gaddag->alphabet, 'E'));
    assert(rack->array_nonzero_indexes[0] == 5);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->array_nonzero_indexes[3] == 2);
    assert(rack->array_nonzero_indexes[4] == 3);
    assert(rack->number_of_nonzero_indexes == 5);
    assert(rack->number_of_letters == 6);

    nonzero_array_index = take_letter_from_rack(rack, val(config->gaddag->alphabet, 'E'));
    assert(rack->array_nonzero_indexes[0] == 5);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->array_nonzero_indexes[3] == 2);
    assert(rack->number_of_nonzero_indexes == 4);
    assert(rack->number_of_letters == 5);
    assert(nonzero_array_index == 1);

    take_letter_from_rack(rack, val(config->gaddag->alphabet, 'F'));
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->letter_to_array_nonzero_index[3] == 1);
    assert(rack->letter_to_array_nonzero_index[50] == 2);
    assert(rack->number_of_nonzero_indexes == 3);
    assert(rack->number_of_letters == 4);

    take_letter_from_rack(rack, val(config->gaddag->alphabet, 'C'));
    assert(rack->array_nonzero_indexes[0] == 2);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->array_nonzero_indexes[2] == 50);
    assert(rack->letter_to_array_nonzero_index[2] == 0);
    assert(rack->letter_to_array_nonzero_index[3] == 1);
    assert(rack->letter_to_array_nonzero_index[50] == 2);
    assert(rack->number_of_nonzero_indexes == 3);
    assert(rack->number_of_letters == 3);

    take_letter_from_rack(rack, val(config->gaddag->alphabet, 'C'));
    assert(rack->array_nonzero_indexes[0] == 50);
    assert(rack->array_nonzero_indexes[1] == 3);
    assert(rack->letter_to_array_nonzero_index[3] == 1);
    assert(rack->letter_to_array_nonzero_index[50] == 0);
    assert(rack->number_of_nonzero_indexes == 2);
    assert(rack->number_of_letters == 2);

    take_letter_from_rack(rack, val(config->gaddag->alphabet, '?'));
    assert(rack->array_nonzero_indexes[0] == 3);
    assert(rack->letter_to_array_nonzero_index[3] == 0);
    assert(rack->number_of_nonzero_indexes == 1);
    assert(rack->number_of_letters == 1);

    take_letter_from_rack(rack, val(config->gaddag->alphabet, 'D'));
    assert(rack->number_of_nonzero_indexes == 0);
    assert(rack->number_of_letters == 0);
}

void test_rack_set_to_string(Config * config, Rack * rack) {
    set_rack_to_string(rack, "AENPPSW", config->gaddag->alphabet);
    assert(rack->array_nonzero_indexes[0] == 0);
    assert(rack->array_nonzero_indexes[1] == 4);
    assert(rack->array_nonzero_indexes[2] == 13);
    assert(rack->array_nonzero_indexes[3] == 15);
    assert(rack->array_nonzero_indexes[4] == 18);
    assert(rack->array_nonzero_indexes[5] == 22);
    assert(rack->letter_to_array_nonzero_index[0] == 0);
    assert(rack->letter_to_array_nonzero_index[4] == 1);
    assert(rack->letter_to_array_nonzero_index[13] == 2);
    assert(rack->letter_to_array_nonzero_index[15] == 3);
    assert(rack->letter_to_array_nonzero_index[18] == 4);
    assert(rack->letter_to_array_nonzero_index[22] == 5);
    assert(rack->number_of_nonzero_indexes == 6);
    assert(rack->number_of_letters == 7);

    set_rack_to_string(rack, "AEINRST", config->gaddag->alphabet);
    assert(rack->letter_to_array_nonzero_index[0] == 0);
    assert(rack->letter_to_array_nonzero_index[4] == 1);
    assert(rack->letter_to_array_nonzero_index[8] == 2);
    assert(rack->letter_to_array_nonzero_index[13] == 3);
    assert(rack->letter_to_array_nonzero_index[17] == 4);
    assert(rack->letter_to_array_nonzero_index[18] == 5);
    assert(rack->letter_to_array_nonzero_index[19] == 6);
    assert(rack->number_of_nonzero_indexes == 7);
    assert(rack->number_of_letters == 7);
}

void test_rack_nonzero_indexes(SuperConfig * superconfig) {
    Config * config = get_america_config(superconfig);
    Rack * rack = create_rack();

    // Repeated filling and emptying the rack
    // should not change the behavior.
    test_rack_nonzero_indexes_take_and_add(config, rack);
    test_rack_nonzero_indexes_take_and_add(config, rack);
    test_rack_nonzero_indexes_take_and_add(config, rack);
    test_rack_nonzero_indexes_take_and_add(config, rack);

    test_rack_set_to_string(config, rack);

    destroy_rack(rack);
}

void test_rack(SuperConfig * superconfig) {
    test_rack_main(superconfig);
    test_rack_nonzero_indexes(superconfig);
}

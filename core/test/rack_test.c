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

void test_rack() {
    Config * config = create_america_sort_by_score_config();
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


    add_letter_to_rack(rack, 13);
    take_letter_from_rack(rack, 13);
    add_letter_to_rack(rack, 13);
    add_letter_to_rack(rack, 13);

    expected_rack->array[13] = 2;
    expected_rack->empty = 0;
    expected_rack->number_of_letters = 2;

    assert(equal_rack(expected_rack, rack));

    destroy_rack(rack);
    destroy_rack(expected_rack);
    destroy_config(config);
}

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
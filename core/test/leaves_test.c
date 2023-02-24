#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "../src/alphabet.h"
#include "../src/config.h"
#include "../src/constants.h"
#include "../src/gaddag.h"
#include "../src/letter_distribution.h"
#include "../src/leaves.h"

#include "test_util.h"
#include "superconfig.h"

void test_english_leaves(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Laddag * laddag = config->player_1_strategy_params->laddag;
    // Check the empty leave
    assert(within_epsilon(laddag->values[0], 0));
    assert(laddag->edges[get_take_edge_index(0, 0)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(0, 1)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(0, (RACK_ARRAY_SIZE)-1)] == (uint32_t)laddag->number_of_nodes);

    assert(laddag->edges[get_add_edge_index(0, BLANK_MACHINE_LETTER)] == 1); // ?
    assert(laddag->edges[get_add_edge_index(0, 0)] == 2); // A
    assert(laddag->edges[get_add_edge_index(0, 1)] == 3); // B
    assert(laddag->edges[get_add_edge_index(0, 25)] == 27); // Z

    // Check one tile leaves
    assert(laddag->edges[get_take_edge_index(1, BLANK_MACHINE_LETTER)] == 0);
    assert(laddag->edges[get_take_edge_index(2, 0)] == 0);
    assert(laddag->edges[get_take_edge_index(3, 1)] == 0);
    assert(laddag->edges[get_take_edge_index(3, 16)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(27, 25)] == 0);

    // Two tile leave
    assert(laddag->edges[get_take_edge_index(28, BLANK_MACHINE_LETTER)] == 1);
    assert(laddag->edges[get_take_edge_index(55, 0)] == 2);
    assert(laddag->edges[get_take_edge_index(81, 1)] == 3);
    assert(laddag->edges[get_add_edge_index(1, BLANK_MACHINE_LETTER)] == 28); // ??
    assert(laddag->edges[get_add_edge_index(2, 0)] == 55); // AA
    assert(laddag->edges[get_add_edge_index(3, 1)] == 81); // BB
    assert(laddag->edges[get_add_edge_index(27, 25)] == (uint32_t)laddag->number_of_nodes); // ZZ (does not exist)

    // 370 = SV
    assert(laddag->edges[get_take_edge_index(370, 18)] == 23); // take the S
    assert(laddag->edges[get_take_edge_index(370, 21)] == 20); // take the V
    assert(laddag->edges[get_take_edge_index(370, 0)] == (uint32_t)laddag->number_of_nodes); // take A, which is not in the rack
    assert(laddag->edges[get_take_edge_index(370, 1)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(370, 2)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(370, 25)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(370, 0)] == 1088);
    assert(laddag->edges[get_add_edge_index(370, 19)] == 3819);
    assert(laddag->edges[get_add_edge_index(370, 25)] == 3834);

    // Three tile leave
    // 3222 == KPU
    assert(laddag->edges[get_take_edge_index(3222, 10)] == 343); // take the K
    assert(laddag->edges[get_take_edge_index(3222, 15)] == 278); // take the P
    assert(laddag->edges[get_take_edge_index(3222, 20)] == 273); // take the U
    assert(laddag->edges[get_take_edge_index(3222, 0)] == (uint32_t)laddag->number_of_nodes); // take A, which is not in the rack
    assert(laddag->edges[get_take_edge_index(3222, 1)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(3222, 2)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(3222, 25)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(3222, 0)] == 9842);
    assert(laddag->edges[get_add_edge_index(3222, 11)] == 25965);
    assert(laddag->edges[get_add_edge_index(3222, 16)] == 26300);
    assert(laddag->edges[get_add_edge_index(3222, 21)] == 26331);

    // Four tile leave
    // 25965 == KPU
    assert(laddag->edges[get_take_edge_index(25965, 10)] == 3339); // take the K
    assert(laddag->edges[get_take_edge_index(25965, 11)] == 3222); // take the L
    assert(laddag->edges[get_take_edge_index(25965, 15)] == 3172); // take the P
    assert(laddag->edges[get_take_edge_index(25965, 20)] == 3167); // take the U
    assert(laddag->edges[get_take_edge_index(25965, 16)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 23)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 12)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 25)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(25965, BLANK_MACHINE_LETTER)] == 50873);
    assert(laddag->edges[get_add_edge_index(25965, 25)] == 166070);

    // Five tile leave
    // 50873 == ?KLPU
    assert(laddag->edges[get_take_edge_index(50873, BLANK_MACHINE_LETTER)] == 25965); // take the ?
    assert(laddag->edges[get_take_edge_index(50873, 10)] == 6822); // take the K
    assert(laddag->edges[get_take_edge_index(50873, 11)] == 6705); // take the L
    assert(laddag->edges[get_take_edge_index(50873, 15)] == 6655); // take the P
    assert(laddag->edges[get_take_edge_index(50873, 20)] == 6650); // take the U
    assert(laddag->edges[get_take_edge_index(50873, 16)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 23)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 12)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 25)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(50873, 2)] == 251592);
    assert(laddag->edges[get_add_edge_index(50873, 12)] == 310814);

    // Six tile leave
    // 310814 == ?KLMPU
    assert(laddag->edges[get_take_edge_index(310814, BLANK_MACHINE_LETTER)] == 165801); // take the ?
    assert(laddag->edges[get_take_edge_index(310814, 10)] == 51604); // take the K
    assert(laddag->edges[get_take_edge_index(310814, 11)] == 50974); // take the L
    assert(laddag->edges[get_take_edge_index(310814, 12)] == 50873); // take the M
    assert(laddag->edges[get_take_edge_index(310814, 15)] == 50837); // take the P
    assert(laddag->edges[get_take_edge_index(310814, 20)] == 50832); // take the U
    assert(laddag->edges[get_take_edge_index(310814, 16)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(310814, 23)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(310814, 25)] == (uint32_t)laddag->number_of_nodes);

    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        assert(laddag->edges[get_add_edge_index(310814, i)] == (uint32_t)laddag->number_of_nodes);
    }
    
    // Check the Es
    // 4 == E
    assert(laddag->edges[get_add_edge_index(6, 4)] == 153);
    assert(laddag->edges[get_add_edge_index(153, 4)] == 2003);
    assert(laddag->edges[get_add_edge_index(2003, 4)] == 17924);
    assert(laddag->edges[get_add_edge_index(17924, 4)] == 123195);
    assert(laddag->edges[get_add_edge_index(123195, 4)] == 693036);
    assert(laddag->edges[get_add_edge_index(693036, 4)] == (uint32_t)laddag->number_of_nodes);

    // Test set start leave
    Rack * rack = create_rack();
    int index;

    // The leave is 6 tiles, which should not result
    // in any changes to the leave graph.
    set_rack_to_string(rack, "?ABCDE", config->gaddag->alphabet);
    char rack_string[7] = "";
    write_rack_to_end_of_buffer(rack_string, config->gaddag->alphabet, rack);
    set_start_leave(laddag, rack);
    index = laddag->current_index;
    assert(index == 202541);
    assert(within_epsilon(laddag->values[laddag->number_of_nodes - 1], 0));

    // The leave is RACK_SIZE (in this case 7) tiles, which should populate last leave node
    // in the array that represents the leave graph.
    set_rack_to_string(rack, "?FGKLMO", config->gaddag->alphabet);
    set_start_leave(laddag, rack);
    index = laddag->current_index;
    assert(laddag->current_index == (laddag->number_of_nodes - 1));
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        assert(laddag->edges[get_add_edge_index(index, i)] == (uint32_t)laddag->number_of_nodes);
    }
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        if (i == 5) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 293010);
        } else if (i == 6) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 285335);
        } else if (i == 10) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 281702);
        } else if (i == 11) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 281585);
        } else if (i == 12) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 281571);
        } else if (i == 14) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 281569);
        } else if (i == 50) {
            assert(laddag->edges[get_take_edge_index(index, i)] == 759283);
        } else {
            assert(laddag->edges[get_take_edge_index(index, i)] == (uint32_t)laddag->number_of_nodes);
        }
    }

    // Check some leave values
    assert(within_epsilon(laddag->values[0], 0));
    assert(within_epsilon(laddag->values[1], 24.674892042377053));
    assert(within_epsilon(laddag->values[2], 1.4148584236112));
    assert(within_epsilon(laddag->values[4925], 24.472998152982427));
    assert(within_epsilon(laddag->values[759284], -20.67402535566822));
    assert(within_epsilon(laddag->values[914624], -10.563130413252125));

    destroy_rack(rack);
}

void test_leaves(SuperConfig * superconfig) {
    test_english_leaves(superconfig);
}
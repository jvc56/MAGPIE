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
    assert(laddag->edges[get_take_edge_index(0, 0, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(0, 1, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(0, (laddag->number_of_unique_tiles)-1, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);

    assert(laddag->edges[get_add_edge_index(0, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 1); // ?
    assert(laddag->edges[get_add_edge_index(0, 1, laddag->number_of_unique_tiles)] == 2); // A
    assert(laddag->edges[get_add_edge_index(0, 2, laddag->number_of_unique_tiles)] == 3); // B
    assert(laddag->edges[get_add_edge_index(0, 26, laddag->number_of_unique_tiles)] == 27); // Z

    // Check one tile leaves
    assert(laddag->edges[get_take_edge_index(1, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 0);
    assert(laddag->edges[get_take_edge_index(2, 1, laddag->number_of_unique_tiles)] == 0);
    assert(laddag->edges[get_take_edge_index(3, 2, laddag->number_of_unique_tiles)] == 0);
    assert(laddag->edges[get_take_edge_index(3, 17, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(27, 26, laddag->number_of_unique_tiles)] == 0);

    // Two tile leave
    assert(laddag->edges[get_take_edge_index(28, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 1);
    assert(laddag->edges[get_take_edge_index(55, 1, laddag->number_of_unique_tiles)] == 2);
    assert(laddag->edges[get_take_edge_index(81, 2, laddag->number_of_unique_tiles)] == 3);
    assert(laddag->edges[get_add_edge_index(1, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 28); // ??
    assert(laddag->edges[get_add_edge_index(2, 1, laddag->number_of_unique_tiles)] == 55); // AA
    assert(laddag->edges[get_add_edge_index(3, 2, laddag->number_of_unique_tiles)] == 81); // BB
    assert(laddag->edges[get_add_edge_index(27, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes); // ZZ (does not exist)

    // 370 = SV
    assert(laddag->edges[get_take_edge_index(370, 19, laddag->number_of_unique_tiles)] == 23); // take the S
    assert(laddag->edges[get_take_edge_index(370, 22, laddag->number_of_unique_tiles)] == 20); // take the V
    assert(laddag->edges[get_take_edge_index(370, 1, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes); // take A, which is not in the rack
    assert(laddag->edges[get_take_edge_index(370, 2, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(370, 3, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(370, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(370, 1, laddag->number_of_unique_tiles)] == 1088);
    assert(laddag->edges[get_add_edge_index(370, 20, laddag->number_of_unique_tiles)] == 3819);
    assert(laddag->edges[get_add_edge_index(370, 26, laddag->number_of_unique_tiles)] == 3834);

    // Three tile leave
    // 3222 == KPU
    assert(laddag->edges[get_take_edge_index(3222, 11, laddag->number_of_unique_tiles)] == 343); // take the K
    assert(laddag->edges[get_take_edge_index(3222, 16, laddag->number_of_unique_tiles)] == 278); // take the P
    assert(laddag->edges[get_take_edge_index(3222, 21, laddag->number_of_unique_tiles)] == 273); // take the U
    assert(laddag->edges[get_take_edge_index(3222, 1, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes); // take A, which is not in the rack
    assert(laddag->edges[get_take_edge_index(3222, 2, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(3222, 3, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(3222, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(3222, 1, laddag->number_of_unique_tiles)] == 9842);
    assert(laddag->edges[get_add_edge_index(3222, 12, laddag->number_of_unique_tiles)] == 25965);
    assert(laddag->edges[get_add_edge_index(3222, 17, laddag->number_of_unique_tiles)] == 26300);
    assert(laddag->edges[get_add_edge_index(3222, 22, laddag->number_of_unique_tiles)] == 26331);

    // Four tile leave
    // 25965 == KPU
    assert(laddag->edges[get_take_edge_index(25965, 11, laddag->number_of_unique_tiles)] == 3339); // take the K
    assert(laddag->edges[get_take_edge_index(25965, 12, laddag->number_of_unique_tiles)] == 3222); // take the L
    assert(laddag->edges[get_take_edge_index(25965, 16, laddag->number_of_unique_tiles)] == 3172); // take the P
    assert(laddag->edges[get_take_edge_index(25965, 21, laddag->number_of_unique_tiles)] == 3167); // take the U
    assert(laddag->edges[get_take_edge_index(25965, 17, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 24, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 13, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(25965, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(25965, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 50873);
    assert(laddag->edges[get_add_edge_index(25965, 26, laddag->number_of_unique_tiles)] == 166070);

    // Five tile leave
    // 50873 == ?KLPU
    assert(laddag->edges[get_take_edge_index(50873, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 25965); // take the ?
    assert(laddag->edges[get_take_edge_index(50873, 11, laddag->number_of_unique_tiles)] == 6822); // take the K
    assert(laddag->edges[get_take_edge_index(50873, 12, laddag->number_of_unique_tiles)] == 6705); // take the L
    assert(laddag->edges[get_take_edge_index(50873, 16, laddag->number_of_unique_tiles)] == 6655); // take the P
    assert(laddag->edges[get_take_edge_index(50873, 21, laddag->number_of_unique_tiles)] == 6650); // take the U
    assert(laddag->edges[get_take_edge_index(50873, 17, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 24, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 13, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(50873, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_add_edge_index(50873, 3, laddag->number_of_unique_tiles)] == 251592);
    assert(laddag->edges[get_add_edge_index(50873, 13, laddag->number_of_unique_tiles)] == 310814);

    // Six tile leave
    // 310814 == ?KLMPU
    assert(laddag->edges[get_take_edge_index(310814, BLANK_MACHINE_LETTER, laddag->number_of_unique_tiles)] == 165801); // take the ?
    assert(laddag->edges[get_take_edge_index(310814, 11, laddag->number_of_unique_tiles)] == 51604); // take the K
    assert(laddag->edges[get_take_edge_index(310814, 12, laddag->number_of_unique_tiles)] == 50974); // take the L
    assert(laddag->edges[get_take_edge_index(310814, 13, laddag->number_of_unique_tiles)] == 50873); // take the M
    assert(laddag->edges[get_take_edge_index(310814, 16, laddag->number_of_unique_tiles)] == 50837); // take the P
    assert(laddag->edges[get_take_edge_index(310814, 21, laddag->number_of_unique_tiles)] == 50832); // take the U
    assert(laddag->edges[get_take_edge_index(310814, 17, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(310814, 24, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    assert(laddag->edges[get_take_edge_index(310814, 26, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);

    for (int i = 0; i < (laddag->number_of_unique_tiles); i++) {
        assert(laddag->edges[get_add_edge_index(310814, i, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    }
    
    // Check the Es
    // 4 == E
    assert(laddag->edges[get_add_edge_index(6, 5, laddag->number_of_unique_tiles)] == 153);
    assert(laddag->edges[get_add_edge_index(153, 5, laddag->number_of_unique_tiles)] == 2003);
    assert(laddag->edges[get_add_edge_index(2003, 5, laddag->number_of_unique_tiles)] == 17924);
    assert(laddag->edges[get_add_edge_index(17924, 5, laddag->number_of_unique_tiles)] == 123195);
    assert(laddag->edges[get_add_edge_index(123195, 5, laddag->number_of_unique_tiles)] == 693036);
    assert(laddag->edges[get_add_edge_index(693036, 5, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);

    // Test set start leave
    Rack * rack = create_rack(config->letter_distribution->size);
    int index;

    // The leave is 6 tiles, which should not result
    // in any changes to the leave graph.
    set_rack_to_string(rack, "?ABCDE", config->kwg->alphabet);
    char rack_string[7] = "";
    write_rack_to_end_of_buffer(rack_string, config->kwg->alphabet, rack);
    set_start_leave(laddag, rack);
    index = laddag->current_index;
    assert(index == 202541);
    assert(within_epsilon(laddag->values[laddag->number_of_nodes - 1], 0));

    // The leave is RACK_SIZE (in this case 7) tiles, which should populate last leave node
    // in the array that represents the leave graph.
    set_rack_to_string(rack, "?FGKLMO", config->kwg->alphabet);
    set_start_leave(laddag, rack);
    index = laddag->current_index;
    assert(laddag->current_index == (laddag->number_of_nodes - 1));
    for (int i = 0; i < (laddag->number_of_unique_tiles); i++) {
        assert(laddag->edges[get_add_edge_index(index, i, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
    }
    for (int i = 0; i < (laddag->number_of_unique_tiles); i++) {
        if (i == 6) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 293010);
        } else if (i == 7) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 285335);
        } else if (i == 11) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 281702);
        } else if (i == 12) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 281585);
        } else if (i == 13) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 281571);
        } else if (i == 15) {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 281569);
        } else if (i == 0) {
            // printf("%d, %d != thing\n", get_take_edge_index(index, i, laddag->number_of_unique_tiles), laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)]);
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == 759283);
        } else {
            assert(laddag->edges[get_take_edge_index(index, i, laddag->number_of_unique_tiles)] == (uint32_t)laddag->number_of_nodes);
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
#include <stdlib.h>
#include <assert.h>

#include "../src/game.h"
#include "../src/infer.h"
#include "../src/move.h"

#include "inference_print.h"
#include "test_util.h"
#include "superconfig.h"

void test_infer_game_racks_not_empty(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    add_letter_to_rack(game->players[0]->rack, BLANK_MACHINE_LETTER);
    Rack * rack = create_rack(game->players[0]->rack->array_size);
    add_letter_to_rack(rack, BLANK_MACHINE_LETTER);
    Inference * inference = infer(game, rack, 0, 0);
    assert(inference->status == INFERENCE_STATUS_RACKS_NOT_EMPTY);
    destroy_inference(inference);

    destroy_game(game);
}

void test_infer_empty_bag(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    int number_of_tiles_in_bag = (RACK_SIZE) - 1;
    int start_machine_letter = 3;
    for (int i = start_machine_letter; i < number_of_tiles_in_bag; i++) {
        game->gen->bag->tiles[i] = i;
    }
    game->gen->bag->last_tile_index = number_of_tiles_in_bag - 1;
    
    Rack * rack = create_rack(game->players[0]->rack->array_size);
    add_letter_to_rack(rack, BLANK_MACHINE_LETTER);
    Inference * inference = infer(game, rack, 0, 0);
    assert(inference->status == INFERENCE_STATUS_BAG_EMPTY);
    assert(inference->total_possible_leaves == 1);
    for (int i = start_machine_letter; i < (RACK_SIZE) - 1; i++) {
        inference->leaves_including_letter[i] = 1;
    }

    destroy_inference(inference);

    destroy_game(game);
}

void test_infer_negative_tiles_remaining(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    Rack * rack = create_rack(game->players[0]->rack->array_size);

    set_rack_to_string(rack, "ABCDEFGH", game->gen->letter_distribution);
    Inference * inference = infer(game, rack, 0, 0);
    assert(inference->status == INFERENCE_STATUS_NEGATIVE_TILES_REMAINING);
    assert(inference->total_possible_leaves == 0);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}


void test_infer_no_tiles_played(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    Rack * rack = create_rack(game->players[0]->rack->array_size);
    Inference * inference = infer(game, rack, 0, 0);
    assert(inference->status == INFERENCE_STATUS_NOTHING_TO_INFER);
    assert(inference->total_possible_leaves == 0);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}


void test_infer_nonempty_bag(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Rack * rack = create_rack(game->players[0]->rack->array_size);

    set_rack_to_string(rack, "MUZAKS", game->gen->letter_distribution);
    Inference * inference = infer(game, rack, 52, 0);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // With this rack, only keeping an S is possible, and
    // there are 3 S remaining.
    assert(inference->total_possible_leaves == 3);

    set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    inference = infer(game, rack, 58, 0);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    //
    assert(inference->total_possible_leaves == 3);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}

void test_infer(SuperConfig * superconfig) {
    test_infer_game_racks_not_empty(superconfig);
    test_infer_empty_bag(superconfig);
    test_infer_negative_tiles_remaining(superconfig);
    test_infer_no_tiles_played(superconfig);
    test_infer_nonempty_bag(superconfig);
}
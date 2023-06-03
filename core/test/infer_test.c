#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "../src/game.h"
#include "../src/infer.h"
#include "../src/move.h"

#include "inference_print.h"
#include "test_util.h"
#include "test_constants.h"
#include "superconfig.h"

void test_infer_rack_overflow(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    Rack * rack = create_rack(game->players[0]->rack->array_size);

    set_rack_to_string(rack, "ABCDEFGH", game->gen->letter_distribution);
    Inference * inference = create_inference(game->gen->letter_distribution->size);
    infer(inference, game, rack, 0, 0, 0);
    assert(inference->status == INFERENCE_STATUS_RACK_OVERFLOW);

    inference->status = INFERENCE_STATUS_INITIALIZED;

    set_rack_to_string(game->players[0]->rack, "ABC", game->gen->letter_distribution);
    set_rack_to_string(rack, "DEFGH", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 0, 0);
    assert(inference->status == INFERENCE_STATUS_RACK_OVERFLOW);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}


void test_infer_no_tiles_played(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    Rack * rack = create_rack(game->players[0]->rack->array_size);
    Inference * inference = create_inference(game->gen->letter_distribution->size);
    infer(inference, game, rack, 0, 0, 0);
    assert(inference->status == INFERENCE_STATUS_NO_TILES_PLAYED);
    assert(inference->total_possible_leaves == 0);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}

void test_infer_tiles_played_not_in_bag(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    Rack * rack = create_rack(game->players[0]->rack->array_size);
    set_rack_to_string(rack, "ABCYEYY", game->gen->letter_distribution);
    Inference * inference = create_inference(game->gen->letter_distribution->size);
    infer(inference, game, rack, 0, 0, 0);
    assert(inference->status == INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG);
    assert(inference->total_possible_leaves == 0);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}


void test_infer_nonerror_cases(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Rack * rack = create_rack(game->players[0]->rack->array_size);
    Inference * inference = create_inference(game->gen->letter_distribution->size);

    set_rack_to_string(rack, "MUZAKS", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 52, 0);
    printf("status: %d\n", inference->status);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // With this rack, only keeping an S is possible, and
    // there are 3 S remaining.
    assert(inference->total_possible_leaves == 3);
    reset_game(game);

    set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 58, 0);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // Letters not possible:
    // A - YAKUZA
    // B - ZAMBUK
    // K - none in bag
    // Q - QUAKY
    // Z - none in bag
    assert(inference->total_possible_leaves == 83);
    reset_game(game);

    set_rack_to_string(rack, "MUZAK", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 50, 0);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // Can't have B or Y because of ZAMBUK and MUZAKY
    // Can't have K or Z because there are none in the bag
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['B']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['K']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['Y']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['Z']
        ) {
            assert(inference->leaves_including_letter[i] == 0);
        } else {
            assert(inference->leaves_including_letter[i] != 0);
        }
    }
    reset_game(game);

    load_cgp(game, VS_JEREMY_WITH_P2_RACK);
    // Score doesn't matter since the bag
    // is empty and the inference should just be
    // the remaining tiles exactly. Since the played
    // tiles contain an E, the inferred leave should not
    // contain an E.
    set_rack_to_string(rack, "E", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 22, 0);
    print_inference(inference, rack);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    assert(inference->total_possible_leaves == 1);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['D']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['S']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['W']
        || i == game->gen->letter_distribution->human_readable_letter_to_machine_letter['?']
        ) {
            assert(inference->leaves_including_letter[i] == 1);
        } else {
            assert(inference->leaves_including_letter[i] == 0);
        }
    }
    reset_game(game);

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}

void test_infer(SuperConfig * superconfig) {
    test_infer_rack_overflow(superconfig);
    test_infer_no_tiles_played(superconfig);
    test_infer_tiles_played_not_in_bag(superconfig);
    test_infer_nonerror_cases(superconfig);
}
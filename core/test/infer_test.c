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
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // With this rack, only keeping an S is possible, and
    // there are 3 S remaining.
    assert(inference->total_possible_leaves == 3);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'S')) {
            assert(inference->leaves_including_letter[i] == 3);
        } else {
            assert(inference->leaves_including_letter[i] == 0);
        }
    }
    reset_game(game);

    set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 58, 0);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // Letters not possible:
    // A - YAKUZA
    // B - ZAMBUK
    // K - none in bag
    // Q - QUAKY
    // Z - none in bag
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'A')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Q')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
        ) {
            assert(inference->leaves_including_letter[i] == 0);
        } else {
            assert(inference->leaves_including_letter[i] != 0);
        }
    }
    assert(inference->total_possible_leaves == 83);
    reset_game(game);

    set_rack_to_string(rack, "MUZAK", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 50, 0);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // Can't have B or Y because of ZAMBUK and MUZAKY
    // Can't have K or Z because there are none in the bag
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Y')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
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
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    assert(inference->total_possible_leaves == 1);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'D')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'S')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'W')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?')
        ) {
            assert(inference->leaves_including_letter[i] == 1);
        } else {
            assert(inference->leaves_including_letter[i] == 0);
        }
    }
    reset_game(game);

    set_rack_to_string(rack, "RENT", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 8, 0);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // There are only 3 racks for which playing RENT for 8 on the opening is top equity:
    // 1) ?ENNRRT keeping ?NR = 2 * 5 * 5  = 50 possible draws
    // 2) EENRRTT keeping ERT = 11 * 5 * 5 = 275 possible draws
    // 3) ENNRRTT keeping NRT = 5 * 5 * 5  = 125 possible draws
    // which sums to 450 total draws.
    // We use this case to easily check that the combinatorial math is correct
    assert(inference->total_possible_leaves == 450);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?')) {
            // The blank was only in leave 1
            assert(inference->leaves_including_letter[i] == 50);
        } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E')) {
            // The E was only in leave 2
            assert(inference->leaves_including_letter[i] == 275);
        } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'N')) {
            // The N was in leaves 1 and 3
            assert(inference->leaves_including_letter[i] == 175);
        } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'R')) {
            // The R was found in all of the leaves
            assert(inference->leaves_including_letter[i] == 450);
        } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'T')) {
            assert(inference->leaves_including_letter[i] == 400);
        } else {
            assert(inference->leaves_including_letter[i] == 0);
        }
    }
    reset_game(game);

    // Contrive an impossible situation to easily test
    // more combinatorics
    load_cgp(game, OOPSYCHOLOGY_CGP);
    set_rack_to_string(rack, "IIII", game->gen->letter_distribution);
    // Empty the bag
    game->gen->bag->last_tile_index = -1;
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z'));

    // Z(OOPSYCHOLOGY) is over 100 points so keeping the Z will never be inferred
    // for plays scoring 50.
    infer(inference, game, rack, 0, 50, 0);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // There are only 4 racks for which not playing Z(OOPSYCHOLOGY) is correct:
    // EEEIIII = 4 possible draws for E = 4 total
    // EEIIIII = 6 possible draws for the Es * 3 possible draws for the Is = 18 total
    // EIIIIII = 4 possible draws for E = 4  * 3 possible draws for the Is = 12
    // IIIIIII = 1 possible draw for the Is = 1
    // For a total of 35 possible draws
    assert(inference->total_possible_leaves == 35);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E')) {
            assert(inference->leaves_including_letter[i] == 34);
        } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I')) {
            assert(inference->leaves_including_letter[i] == 31);
        } else {
            assert(inference->leaves_including_letter[i] == 0);
        }
    }
    reset_game(game);

    // Check that the equity margin works
    set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    infer(inference, game, rack, 0, 58, 5);
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // Letters not possible with equity margin of 5:
    // B - ZAMBUK
    // K - none in bag
    // Q - QUAKY
    // Z - none in bag
    // Letters now possible because of the additional 5 equity buffer:
    // A - YAKUZA
    assert(inference->total_possible_leaves == 91);
    for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
        if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Q')
        ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
        ) {
            assert(inference->leaves_including_letter[i] == 0);
        } else {
            assert(inference->leaves_including_letter[i] != 0);
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
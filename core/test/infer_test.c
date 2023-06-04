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

    destroy_rack(rack);
    destroy_inference(inference);
    destroy_game(game);
}


void test_infer_nonerror_cases(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Rack * rack = create_rack(game->players[0]->rack->array_size);
    Inference * inference = create_inference(game->gen->letter_distribution->size);

    // set_rack_to_string(rack, "MUZAKS", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 52, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // With this rack, only keeping an S is possible, and
    // // there are 3 S remaining.
    // assert(inference->total_draws == 3);
    // assert(inference->total_leaves == 1);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'S')) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 3);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //     }
    // }
    // reset_game(game);

    // set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 58, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // Letters not possible:
    // // A - YAKUZA
    // // B - ZAMBUK
    // // K - none in bag
    // // Q - QUAKY
    // // Z - none in bag
    // assert(inference->total_draws == 83);
    // assert(inference->total_leaves == 22);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'A')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Q')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
    //     ) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    //     }
    // }
    // reset_game(game);

    // set_rack_to_string(rack, "MUZAK", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 50, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // Can't have B or Y because of ZAMBUK and MUZAKY
    // // Can't have K or Z because there are none in the bag
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Y')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
    //     ) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    //     }
    // }
    // reset_game(game);

    // load_cgp(game, VS_JEREMY_WITH_P2_RACK);
    // // Score doesn't matter since the bag
    // // is empty and the inference should just be
    // // the remaining tiles exactly. Since the played
    // // tiles contain an E, the inferred leave should not
    // // contain an E.
    // set_rack_to_string(rack, "E", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 22, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // assert(inference->total_draws == 1);
    // assert(inference->total_leaves == 1);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'S')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'W')
    //     ) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'D')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?')
    //     ) {
    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //     }
    // }
    // reset_game(game);

    // set_rack_to_string(rack, "RENT", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 8, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // There are only 3 racks for which playing RENT for 8 on the opening is top equity:
    // // 1) ?ENNRRT keeping ?NR = 2 * 5 * 5  = 50 possible draws
    // // 2) EENRRTT keeping ERT = 11 * 5 * 5 = 275 possible draws
    // // 3) ENNRRTT keeping NRT = 5 * 5 * 5  = 125 possible draws
    // // which sums to 450 total draws.
    // // We use this case to easily check that the combinatorial math is correct
    // assert(inference->total_draws == 450);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?')) {
    //         // The blank was only in leave 1
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 50);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E')) {
    //         // The E was only in leave 2
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 275);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'N')) {
    //         // The N was in leaves 1 and 3
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 175);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'R')) {
    //         // The R was found in all of the leaves
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 450);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'T')) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 400);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    //     }
    // }
    // reset_game(game);

    // // Contrive an impossible situation to easily test
    // // more combinatorics
    // load_cgp(game, OOPSYCHOLOGY_CGP);
    // set_rack_to_string(rack, "IIII", game->gen->letter_distribution);
    // // Empty the bag
    // game->gen->bag->last_tile_index = -1;
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E'));
    // add_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z'));

    // // Z(OOPSYCHOLOGY) is over 100 points so keeping the Z will never be inferred
    // // for plays scoring 50.
    // infer(inference, game, rack, 0, 50, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // There are only 4 racks for which not playing Z(OOPSYCHOLOGY) is correct:
    // // EEEIIII = 4 possible draws for E = 4 total
    // // EEIIIII = 6 possible draws for the Es * 3 possible draws for the Is = 18 total
    // // EIIIIII = 4 possible draws for E = 4  * 3 possible draws for the Is = 12
    // // IIIIIII = 1 possible draw for the Is = 1
    // // For a total of 35 possible draws
    // assert(inference->total_draws == 35);
    // assert(inference->total_leaves == 4);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'E')) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 12);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 34);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);

    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 18);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 22);
    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);

    //         assert(get_subtotal(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 4);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 4);
    //         assert(get_subtotal(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'I')) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 18);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 31);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);

    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 12);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 13);
    //         assert(get_subtotal(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 2, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);

    //         assert(get_subtotal(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
    //         assert(get_subtotal(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 3, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    //         assert(get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    //     }
    // }
    // reset_game(game);

    // // Check that the equity margin works
    // set_rack_to_string(rack, "MUZAKY", game->gen->letter_distribution);
    // infer(inference, game, rack, 0, 58, 5);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // Letters not possible with equity margin of 5:
    // // B - ZAMBUK
    // // K - none in bag
    // // Q - QUAKY
    // // Z - none in bag
    // // Letters now possible because of the additional 5 equity buffer:
    // // A - YAKUZA
    // assert(inference->total_draws == 91);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'B')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'K')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Q')
    //     ||  i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'Z')
    //     ) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    //     }
    // }
    // reset_game(game);

    // // Test partial leaves
    // // play GRIND with partial leave of ?
    // set_rack_to_string(rack, "GRIND", game->gen->letter_distribution);
    // // Partially known leaves that are on the player's rack
    // // before the inference are not removed from the bag, so
    // // we have to remove it here.
    // set_rack_to_string(game->players[0]->rack, "?", game->gen->letter_distribution);
    // draw_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?'));
    // infer(inference, game, rack, 0, 18, 0);
    // assert(inference->status == INFERENCE_STATUS_SUCCESS);
    // // If GRIND is played keeping ?, the only
    // // possible other tile is an X
    // assert(inference->total_draws == 2);
    // assert(inference->total_leaves == 1);
    // for (uint32_t i = 0; i < game->gen->letter_distribution->size; i++) {
    //     if (i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, '?')
    //     || i == human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'X')) {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 2);
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    //     } else {
    //         assert(get_subtotal(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    //     }
    // }
    // reset_game(game);


    set_rack_to_string(rack, "RIN", game->gen->letter_distribution);
    set_rack_to_string(game->players[0]->rack, "H", game->gen->letter_distribution);
    draw_letter(game->gen->bag, human_readable_letter_to_machine_letter(game->gen->letter_distribution, 'H'));
    infer(inference, game, rack, 0, 6, 0);
    // If the player opens with RIN for 6 keeping an H, there are only 3
    // possible racks where this would be correct:
    // 1) ?HIINRR keeping ?HIR = 2 * 2 * 8 * 5 = 160
    // 2) ?HINNRR keeping ?HNR = 2 * 2 * 5 * 5 = 100
    // 3) HIINNRR keeping HINR = 2 * 8 * 5 * 5 = 400
    // For a total of 660 possible draws
    assert(inference->status == INFERENCE_STATUS_SUCCESS);
    assert(inference->total_draws == 660);
    assert(inference->total_leaves == 3);
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
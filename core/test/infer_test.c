#include <stdlib.h>
#include <assert.h>

#include "../src/game.h"
#include "../src/infer.h"
#include "../src/move.h"

#include "test_util.h"
#include "superconfig.h"

void test_infer_nonempty_rack(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    add_letter_to_rack(game->players[0]->rack, BLANK_MACHINE_LETTER);
    Inference * inference = infer(game, NULL, 0, 0);
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
    
    Inference * inference = infer(game, NULL, 0, 0);
    assert(inference->status == INFERENCE_STATUS_BAG_EMPTY);
    assert(inference->total_possible_leaves == 1);
    for (int i = start_machine_letter; i < (RACK_SIZE) - 1; i++) {
        inference->leaves_including_letter[i] = 1;
    }

    destroy_inference(inference);

    destroy_game(game);
}

void test_infer(SuperConfig * superconfig) {
    test_infer_nonempty_rack(superconfig);
    test_infer_empty_bag(superconfig);
}
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#include "../src/config.h"
#include "../src/cross_set.h"
#include "../src/game.h"

#include "board_test.h"
#include "test_constants.h"
#include "test_util.h"
#include "superconfig.h"

void test_board(SuperConfig * superconfig) {
    Config * config = get_america_config(superconfig);
    Game * game = create_game(config);
    load_cgp(game, VS_ED);

    assert(!get_anchor(game->gen->board, 3, 3, 0) && !get_anchor(game->gen->board, 3, 3, 1));
    assert(get_anchor(game->gen->board, 12, 12, 0) && get_anchor(game->gen->board, 12, 12, 1));
    assert(get_anchor(game->gen->board, 4, 3, 1) && !get_anchor(game->gen->board, 4, 3, 0));

    // Test cross set
    clear_cross_set(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION);
    set_cross_set_letter(get_cross_set_pointer(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION), 13);
    assert(get_cross_set(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION) == 8192);
    set_cross_set_letter(get_cross_set_pointer(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION), 0);
    assert(get_cross_set(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION) == 8193);

    uint64_t cs = get_cross_set(game->gen->board, 0, 0, BOARD_HORIZONTAL_DIRECTION);
    assert(!allowed(cs, 1));
    assert(allowed(cs, 0));
    assert(!allowed(cs, 14));
    assert(allowed(cs, 13));
    assert(!allowed(cs, 12));

    destroy_game(game);
}
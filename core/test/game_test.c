#include <assert.h>
#include <stdio.h>

#include "game_test.h"
#include "rack_test.h"
#include "../src/config.h"
#include "../src/game.h"
#include "test_constants.h"
#include "test_util.h"

void reset_and_load_game(Game * game, const char* cgp) {
    reset_game(game);
    load_cgp(game, cgp);
}

void test_load_cgp(TestConfig * test_config) {
    Config * config = get_america_config(test_config);
    Game * game = create_game(config);
    // Test that loading various CGPs doesn't result in
    // any errors
    reset_and_load_game(game, EMPTY_CGP);
    reset_and_load_game(game, EMPTY_PLAYER0_RACK_CGP);
    reset_and_load_game(game, EMPTY_PLAYER1_RACK_CGP);
    reset_and_load_game(game, OPENING_CGP);
    reset_and_load_game(game, DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP);
    reset_and_load_game(game, DOUG_V_EMELY_CGP);
    reset_and_load_game(game, GUY_VS_BOT_ALMOST_COMPLETE_CGP);
    reset_and_load_game(game, GUY_VS_BOT_CGP);
    reset_and_load_game(game, INCOMPLETE_3_CGP);
    reset_and_load_game(game, INCOMPLETE4_CGP);
    reset_and_load_game(game, INCOMPLETE_ELISE_CGP);
    reset_and_load_game(game, INCOMPLETE_CGP);
    reset_and_load_game(game, JOSH2_CGP);
    reset_and_load_game(game, NAME_ISO8859_1_CGP);
    reset_and_load_game(game, NAME_UTF8_NOHEADER_CGP);
    reset_and_load_game(game, NAME_UTF8_WITH_HEADER_CGP);
    reset_and_load_game(game, NOAH_VS_MISHU_CGP);
    reset_and_load_game(game, NOAH_VS_PETER_CGP);
    reset_and_load_game(game, SOME_ISC_GAME_CGP);
    reset_and_load_game(game, UTF8_DOS_CGP);
    reset_and_load_game(game, VS_ANDY_CGP);
    reset_and_load_game(game, VS_FRENTZ_CGP);
    destroy_game(game);
    
}

void test_game_main(TestConfig * test_config) {
    Config * config = get_america_config(test_config);
    Game * game = create_game(config);
    Rack * rack = create_rack();

    // Test Reset
    game->consecutive_scoreless_turns = 3;
    game->game_end_reason = GAME_END_REASON_STANDARD;
    reset_game(game);
    assert(game->consecutive_scoreless_turns == 0);
    assert(game->game_end_reason == GAME_END_REASON_NONE);

    // Test opening racks
    load_cgp(game, OPENING_CGP);
    set_rack_to_string(rack, "ABCDEFG", config->gaddag->alphabet);
    assert(equal_rack(rack, game->players[0]->rack));
    set_rack_to_string(rack, "HIJKLM?", config->gaddag->alphabet);
    assert(equal_rack(rack, game->players[1]->rack));
    reset_game(game);

    // Test CGP with excessive whitespace
    load_cgp(game, EXCESSIVE_WHITESPACE_CGP);
    set_rack_to_string(rack, "ABCDEFG", config->gaddag->alphabet);
    assert(equal_rack(rack, game->players[0]->rack));
    set_rack_to_string(rack, "HIJKLM?", config->gaddag->alphabet);
    assert(equal_rack(rack, game->players[1]->rack));
    assert(game->consecutive_scoreless_turns == 4);
    reset_game(game);

    // Test CGP with one consecutive zero
    load_cgp(game, ONE_CONSECUTIVE_ZERO_CGP);
    assert(game->consecutive_scoreless_turns == 1);
    reset_game(game);

    destroy_rack(rack);
    destroy_game(game);
}

void test_game(TestConfig * test_config) {
    test_game_main(test_config);
    test_load_cgp(test_config);
}
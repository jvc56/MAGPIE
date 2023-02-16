#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/constants.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/leaves.h"

#include "test_constants.h"
#include "test_util.h"
#include "test_config.h"

void test_macondo_opening_equity_adjustments(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);
    Game * game = create_game(config);
    Rack * rack = game->players[0]->rack;
    Laddag * laddag = game->players[0]->strategy_params->laddag;
    set_rack_to_string(rack, "EORSTVX", game->gen->gaddag->alphabet);
    generate_moves_for_game(game);
    // Should be 8G VORTEX
    Move * top_move = game->gen->move_list->moves[0];
    assert(top_move->col_start == 6);
    assert(top_move->tiles_played == 6);
    assert(top_move->score == 48);
    assert(within_epsilon((double)(top_move->score + get_leave_value_for_move(laddag, top_move, rack)), top_move->equity));
    reset_game(game);

    set_rack_to_string(rack, "BDEIIIJ", game->gen->gaddag->alphabet);
    generate_moves_for_game(game);
    // Should be 8D JIBED
    top_move = game->gen->move_list->moves[0];
    assert(top_move->col_start == 3);
    assert(top_move->tiles_played == 5);
    assert(top_move->score == 46);
    assert(within_epsilon((double)(top_move->score + get_leave_value_for_move(laddag, top_move, rack) + OPENING_HOTSPOT_PENALTY), top_move->equity));
    reset_game(game);

    set_rack_to_string(rack, "ACEEEFT", game->gen->gaddag->alphabet);
    generate_moves_for_game(game);
    // Should be 8D FACETE
    top_move = game->gen->move_list->moves[0];
    assert(top_move->col_start == 3);
    assert(top_move->tiles_played == 6);
    assert(top_move->score == 30);
    assert(within_epsilon((double)(top_move->score + get_leave_value_for_move(laddag, top_move, rack) + (2 * OPENING_HOTSPOT_PENALTY)), top_move->equity));
    reset_game(game);

    set_rack_to_string(rack, "AAAALTY", game->gen->gaddag->alphabet);
    generate_moves_for_game(game);
    // Should be 8G ATALAYA
    top_move = game->gen->move_list->moves[0];
    assert(top_move->col_start == 6);
    assert(top_move->tiles_played == 7);
    assert(top_move->score == 78);
    assert(within_epsilon((double)(top_move->score + get_leave_value_for_move(laddag, top_move, rack) + (3 * OPENING_HOTSPOT_PENALTY)), top_move->equity));

    destroy_game(game);
}

void test_macondo_endgame_equity_adjustments(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);
    Game * game = create_game(config);

    load_cgp(game, "4RUMMAGED2C/7A6A/2H1G2T6V/2O1O2I6E/2WAB2PREBENDS/2ER3O3n3/2SI6COW2/3L2HUE2KANE/3LI3FILII2/J1TANGENT2T1Z1/A2TA5FA1OP/R2EN5Ok1OU/VILDE5YEX1D/I3R6SUQS/E13Y INR/OT 440/448 0 lex CSW21;");

    generate_moves_for_game(game);

    Move * move0 = game->gen->move_list->moves[0];
    assert(move0->score == 8);
    assert(move0->row_start == 1);
    assert(move0->col_start == 10);
    assert(move0->tiles_played == 3);
    assert(within_epsilon(move0->equity, 12));

    Move * move1 = game->gen->move_list->moves[1];
    assert(move1->score == 5);
    assert(move1->row_start == 2);
    assert(move1->col_start == 7);
    assert(move1->tiles_played == 3);
    assert(within_epsilon(move1->equity, 9));

    Move * move2 = game->gen->move_list->moves[2];
    assert(move2->score == 13);
    assert(move2->row_start == 1);
    assert(move2->col_start == 5);
    assert(move2->tiles_played == 2);
    assert(within_epsilon(move2->equity, 1));

    Move * move3 = game->gen->move_list->moves[3];
    assert(move3->score == 12);
    assert(move3->row_start == 1);
    assert(move3->col_start == 7);
    assert(move3->tiles_played == 2);
    assert(within_epsilon(move3->equity, 0));

    Move * move4 = game->gen->move_list->moves[4];
    assert(move4->score == 11);
    assert(move4->row_start == 1);
    assert(move4->col_start == 9);
    assert(move4->tiles_played == 2);
    assert(within_epsilon(move4->equity, -1));

    Move * move5 = game->gen->move_list->moves[5];
    assert(move5->score == 10);
    assert(move5->row_start == 9);
    assert(move5->col_start == 2);
    assert(move5->tiles_played == 2);
    assert(within_epsilon(move5->equity, -2));

    destroy_game(game);
    
}

void test_equity_adjustments(TestConfig * test_config) {
    test_macondo_opening_equity_adjustments(test_config);
    test_macondo_endgame_equity_adjustments(test_config);
}
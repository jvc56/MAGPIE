#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/constants.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/klv.h"

#include "test_constants.h"
#include "test_util.h"
#include "superconfig.h"

void test_macondo_opening_equity_adjustments(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Rack * rack = game->players[0]->rack;
    KLV * klv = game->players[0]->strategy_params->klv;
    set_rack_to_string(rack, "EORSTVX", game->gen->kwg->alphabet);
    generate_moves_for_game(game);

    // Should be 8G VORTEX

    SortedMoveList * vortex_sorted_move_list = create_sorted_move_list(game->gen->move_list);

    Move * top_move = vortex_sorted_move_list->moves[0];
    assert(top_move->col_start == 6);
    assert(top_move->tiles_played == 6);
    assert(top_move->score == 48);
    assert(within_epsilon_float((float)(top_move->score + get_leave_value_for_move(klv, top_move, rack)), top_move->equity));

    destroy_sorted_move_list(vortex_sorted_move_list);
    reset_game(game);

    set_rack_to_string(rack, "BDEIIIJ", game->gen->kwg->alphabet);
    generate_moves_for_game(game);
    // Should be 8D JIBED

    SortedMoveList * jibed_sorted_move_list = create_sorted_move_list(game->gen->move_list);

    top_move = jibed_sorted_move_list->moves[0];
    assert(top_move->col_start == 3);
    assert(top_move->tiles_played == 5);
    assert(top_move->score == 46);
    assert(within_epsilon_float((float)(top_move->score + get_leave_value_for_move(klv, top_move, rack) + OPENING_HOTSPOT_PENALTY), top_move->equity));
    
    destroy_sorted_move_list(jibed_sorted_move_list);
    reset_game(game);

    set_rack_to_string(rack, "ACEEEFT", game->gen->kwg->alphabet);
    generate_moves_for_game(game);
    // Should be 8D FACETE
    SortedMoveList * facete_sorted_move_list = create_sorted_move_list(game->gen->move_list);
    top_move = facete_sorted_move_list->moves[0];
    assert(top_move->col_start == 3);
    assert(top_move->tiles_played == 6);
    assert(top_move->score == 30);
    assert(within_epsilon_float((float)(top_move->score + get_leave_value_for_move(klv, top_move, rack) + (2 * OPENING_HOTSPOT_PENALTY)), top_move->equity));
    destroy_sorted_move_list(facete_sorted_move_list);
    reset_game(game);

    set_rack_to_string(rack, "AAAALTY", game->gen->kwg->alphabet);
    generate_moves_for_game(game);
    // Should be 8G ATALAYA
    SortedMoveList * atalaya_sorted_move_list = create_sorted_move_list(game->gen->move_list);
    top_move = atalaya_sorted_move_list->moves[0];
    assert(top_move->col_start == 6);
    assert(top_move->tiles_played == 7);
    assert(top_move->score == 78);
    assert(within_epsilon_float((float)(top_move->score + get_leave_value_for_move(klv, top_move, rack) + (3 * OPENING_HOTSPOT_PENALTY)), top_move->equity));

    destroy_sorted_move_list(atalaya_sorted_move_list);
    destroy_game(game);
}

void test_macondo_endgame_equity_adjustments(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);

    load_cgp(game, "4RUMMAGED2C/7A6A/2H1G2T6V/2O1O2I6E/2WAB2PREBENDS/2ER3O3n3/2SI6COW2/3L2HUE2KANE/3LI3FILII2/J1TANGENT2T1Z1/A2TA5FA1OP/R2EN5Ok1OU/VILDE5YEX1D/I3R6SUQS/E13Y INR/OT 440/448 0 lex CSW21;");

    generate_moves_for_game(game);
    
    SortedMoveList * endgame_sorted_move_list = create_sorted_move_list(game->gen->move_list);

    Move * move0 = endgame_sorted_move_list->moves[0];
    assert(move0->score == 8);
    assert(move0->row_start == 1);
    assert(move0->col_start == 10);
    assert(move0->tiles_played == 3);
    assert(within_epsilon_float(move0->equity, 12));

    Move * move1 = endgame_sorted_move_list->moves[1];
    assert(move1->score == 5);
    assert(move1->row_start == 2);
    assert(move1->col_start == 7);
    assert(move1->tiles_played == 3);
    assert(within_epsilon_float(move1->equity, 9));

    Move * move2 = endgame_sorted_move_list->moves[2];
    assert(move2->score == 13);
    assert(move2->row_start == 1);
    assert(move2->col_start == 5);
    assert(move2->tiles_played == 2);
    assert(within_epsilon_float(move2->equity, 1));

    Move * move3 = endgame_sorted_move_list->moves[3];
    assert(move3->score == 12);
    assert(move3->row_start == 1);
    assert(move3->col_start == 7);
    assert(move3->tiles_played == 2);
    assert(within_epsilon_float(move3->equity, 0));

    Move * move4 = endgame_sorted_move_list->moves[4];
    assert(move4->score == 11);
    assert(move4->row_start == 1);
    assert(move4->col_start == 9);
    assert(move4->tiles_played == 2);
    assert(within_epsilon_float(move4->equity, -1));

    Move * move5 = endgame_sorted_move_list->moves[5];
    assert(move5->score == 10);
    assert(move5->row_start == 9);
    assert(move5->col_start == 2);
    assert(move5->tiles_played == 2);
    assert(within_epsilon_float(move5->equity, -2));

    destroy_sorted_move_list(endgame_sorted_move_list);
    destroy_game(game);
}

void test_equity_adjustments(SuperConfig * superconfig) {
    test_macondo_opening_equity_adjustments(superconfig);
    test_macondo_endgame_equity_adjustments(superconfig);
}
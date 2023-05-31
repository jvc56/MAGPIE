#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/game.h"

#include "game_print.h"
#include "superconfig.h"
#include "test_constants.h"
#include "test_util.h"

void load_and_generate(Game * game, Player * player, const char * cgp, const char * rack, int add_exchange) {
    reset_game(game);
    load_cgp(game, cgp);
    set_rack_to_string(player->rack, rack, game->gen->letter_distribution);
    generate_moves(game->gen, player, NULL, add_exchange);
    float previous_equity;
    for (int i = 0; i < game->gen->anchor_list->count; i++) {
        if (i == 0) {
            previous_equity = game->gen->anchor_list->anchors[i]->highest_possible_equity;
        } 
        assert(game->gen->anchor_list->anchors[i]->highest_possible_equity <= previous_equity);
    }
}

void test_shadow_score(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Player * player = game->players[0];

    // This test checks scores only, so set the player strategy param
    // to move sorting of type score.
    int original_move_sorting = player->strategy_params->move_sorting;
    player->strategy_params->move_sorting = SORT_BY_SCORE;

    load_and_generate(game, player, EMPTY_CGP, "OU", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

    load_and_generate(game, player, EMPTY_CGP, "ID", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 6));

    load_and_generate(game, player, EMPTY_CGP, "AX", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 18));

    load_and_generate(game, player, EMPTY_CGP, "BD", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));

    load_and_generate(game, player, EMPTY_CGP, "QK", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

    load_and_generate(game, player, EMPTY_CGP, "AESR", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

    load_and_generate(game, player, EMPTY_CGP, "TNCL", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

    load_and_generate(game, player, EMPTY_CGP, "AAAAA", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

    load_and_generate(game, player, EMPTY_CGP, "CAAAA", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

    load_and_generate(game, player, EMPTY_CGP, "CAKAA", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

    load_and_generate(game, player, EMPTY_CGP, "AIERZ", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 48));

    load_and_generate(game, player, EMPTY_CGP, "AIERZN", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 50));

    load_and_generate(game, player, EMPTY_CGP, "AIERZNL", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));

    load_and_generate(game, player, EMPTY_CGP, "?", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

    load_and_generate(game, player, EMPTY_CGP, "??", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

    load_and_generate(game, player, EMPTY_CGP, "??OU", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

    load_and_generate(game, player, EMPTY_CGP, "??OUA", 0);
    assert(game->gen->anchor_list->count == 1);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

    load_and_generate(game, player, KA_OPENING_CGP, "EE", 0);
    // KAE and EE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));
    // EKE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 9));
    // KAEE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 8));
    // EE and E(A)
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 5));
    // EE and E(A)
    assert(within_epsilon_float(game->gen->anchor_list->anchors[4]->highest_possible_equity, 5));
    // EEE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[5]->highest_possible_equity, 3));
    // The rest are prevented by invalid cross sets
    assert(within_epsilon_float(game->gen->anchor_list->anchors[6]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[7]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[8]->highest_possible_equity, 0));

    load_and_generate(game, player, KA_OPENING_CGP, "E?", 0);
    // oK, oE, EA
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));
    // KA, aE, AE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 10));
    // KAe, Ee
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 8));
    // EKA, Ea
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 8));
    // KAEe
    assert(within_epsilon_float(game->gen->anchor_list->anchors[4]->highest_possible_equity, 7));
    // E(K)e
    assert(within_epsilon_float(game->gen->anchor_list->anchors[5]->highest_possible_equity, 7));
    // Ea, EA
    assert(within_epsilon_float(game->gen->anchor_list->anchors[6]->highest_possible_equity, 3));
    // Ae, eE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[7]->highest_possible_equity, 3));
    // E(A)a
    assert(within_epsilon_float(game->gen->anchor_list->anchors[8]->highest_possible_equity, 2));

    load_and_generate(game, player, KA_OPENING_CGP, "J", 0);
    // J(K) veritcally
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 21));
    // J(KA) or (KA)J
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 14));
    // J(A) horitizontally
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 9));
    // J(A) vertically
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 9));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[4]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[5]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[6]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[7]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[8]->highest_possible_equity, 0));

    load_and_generate(game, player, AA_OPENING_CGP, "JF", 0);
    // JF, JA, and FA
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 42));
    // JA and JF or FA and FJ
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 25));
    // JAF with J and F doubled
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 25));
    // FAA is in cross set, so JAA and JF are used to score.
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 22));
    // AAJF
    assert(within_epsilon_float(game->gen->anchor_list->anchors[4]->highest_possible_equity, 14));
    // AJF
    assert(within_epsilon_float(game->gen->anchor_list->anchors[5]->highest_possible_equity, 13));
    // Remaining anchors are prevented by invalid cross sets
    assert(within_epsilon_float(game->gen->anchor_list->anchors[6]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[7]->highest_possible_equity, 0));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[8]->highest_possible_equity, 0));

    // Makeing JA, FA, and JFU, doubling the U on the double letter
    load_and_generate(game, player, AA_OPENING_CGP, "JFU", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 44));

    // Making KAU (allowed by F in rack cross set) and JUF, doubling the F and J.
    load_and_generate(game, player, KA_OPENING_CGP, "JFU", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

    load_and_generate(game, player, AA_OPENING_CGP, "JFUG", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 47));

    load_and_generate(game, player, AA_OPENING_CGP, "JFUGX", 0); 
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 61));

    // Reaches the triple word
    load_and_generate(game, player, AA_OPENING_CGP, "JFUGXL", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "Q", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BD", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 17));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOH", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 60));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGX", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZ", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 120));

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 230));

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "A", 0);
    
    // WINDYA
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 13));
    // PROTEANA
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 11));
    // ANY horizontally
    // ANY vertically
    // A(P) vertically
    // A(OW) vertically
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 6));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 6));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[4]->highest_possible_equity, 6));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[5]->highest_possible_equity, 6));
    // A(EN)
    // AD(A)
    assert(within_epsilon_float(game->gen->anchor_list->anchors[6]->highest_possible_equity, 5));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[7]->highest_possible_equity, 5));

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "Z", 0);
    // Z(P) vertically
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 33));
    // Z(EN) vert
    // Z(EN) horiz
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 32));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 32));
    // (PROTEAN)Z
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 29));

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW", 0);
    // ZEN, ZW, WAD
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 73));
    // ZENLW
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 45));
    // ZLWOW
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 40));

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW?", 0);
    // The blank makes all cross sets valid
    // LZW(WINDY)s
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 99));

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "QZLW", 0);
    // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 85));

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "K", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 23));

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT", 0);
    // KPAVT
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 26));

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT?", 0);
    // The blank makes PAVE, allowed all letters in the cross set
    // PAVK, KT?
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "M", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MN", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 16));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNA", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAU", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z", 0);
    // (L)Z and (R)Z
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 31));
    assert(within_epsilon_float(game->gen->anchor_list->anchors[1]->highest_possible_equity, 31));
    // (LATER)Z
    assert(within_epsilon_float(game->gen->anchor_list->anchors[2]->highest_possible_equity, 30));
    // Z(T)
    assert(within_epsilon_float(game->gen->anchor_list->anchors[3]->highest_possible_equity, 21));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 64));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 68));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 72));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 77));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 80));

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 212));

    load_and_generate(game, player, VS_OXY, "A", 0);
    // APACIFYING
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 63));

    load_and_generate(game, player, VS_OXY, "PB", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 156));

    load_and_generate(game, player, VS_OXY, "PA", 0);
    // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 76));

    load_and_generate(game, player, VS_OXY, "PBA", 0);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 174));

    load_and_generate(game, player, VS_OXY, "Z", 0);
    // ZPACIFYING
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

    load_and_generate(game, player, VS_OXY, "ZE", 0);
    // ZONE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 160));

    load_and_generate(game, player, VS_OXY, "AZE", 0);
    // UTAZONE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 184));

    load_and_generate(game, player, VS_OXY, "AZEB", 0);
    // HENBUTAZONE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 484));

    load_and_generate(game, player, VS_OXY, "AZEBP", 0);
    // YPHENBUTAZONE
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 604));

    load_and_generate(game, player, VS_OXY, "AZEBPX", 0);
    // A2 A(Y)X(HEN)P(UT)EZ(ON)B
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 740));

    load_and_generate(game, player, VS_OXY, "AZEBPXO", 0);
    // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 1924));

    load_and_generate(game, player, VS_OXY, "AZEBPQO", 0);
    // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
    // Only the letters AZEBPO are required to form acceptable
    // plays in all cross sets
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 2036));

    player->strategy_params->move_sorting = original_move_sorting;

    destroy_game(game);
}

void test_shadow_equity(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Player * player = game->players[0];

    // This test checks scores only, so set the player strategy param
    // to move sorting of type score.
    int original_move_sorting = player->strategy_params->move_sorting;
    player->strategy_params->move_sorting = SORT_BY_EQUITY;

    // Check best leave values for a give rack.
    Rack * leave_rack = create_rack(game->gen->letter_distribution->size);
    load_and_generate(game, player, EMPTY_CGP, "ERSVQUW", 0);

    set_rack_to_string(leave_rack, "", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[0], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "S", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[1], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "ES", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[2], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "ERS", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[3], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "EQSU", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[4], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "EQRSU", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[5], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    set_rack_to_string(leave_rack, "EQRSUV", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->best_leaves[6], get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    load_and_generate(game, player, EMPTY_CGP, "ESQW", 1);
    set_rack_to_string(leave_rack, "ES", game->gen->letter_distribution);
    assert(within_epsilon_float(game->gen->anchor_list->anchors[0]->highest_possible_equity, 28 + get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

    player->strategy_params->move_sorting = original_move_sorting;

    destroy_game(game);
    destroy_rack(leave_rack);
}

void test_shadow_top_move(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Player * player = game->players[0];

    int original_move_sorting = player->strategy_params->move_sorting;
    int original_recorder_type = player->strategy_params->play_recorder_type;
    player->strategy_params->move_sorting = SORT_BY_EQUITY;
    player->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;

    // Top play should be L1 Q(I)
    load_and_generate(game, player, UEY_CGP, "ACEQOOV", 1);
    assert(within_epsilon_float(game->gen->move_list->moves[0]->score, 21));

    player->strategy_params->move_sorting = original_move_sorting;
    player->strategy_params->play_recorder_type = original_recorder_type;

    destroy_game(game);
}

void test_shadow(SuperConfig * superconfig) {
    test_shadow_score(superconfig);
    test_shadow_equity(superconfig);
    test_shadow_top_move(superconfig);
}
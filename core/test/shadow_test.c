#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/game.h"

#include "superconfig.h"
#include "test_constants.h"

void load_and_generate(Game * game, Player * player, const char * cgp, const char * rack) {
    printf("Generating for %s: %s\n", rack, cgp);
    reset_game(game);
    load_cgp(game, cgp);
    set_rack_to_string(player->rack, rack, game->gen->kwg->alphabet);
    generate_moves(game->gen, player, NULL, 0);
    int previous_score;
    for (int i = 0; i < game->gen->anchor_list->count; i++) {
        if (i == 0) {
            previous_score = game->gen->anchor_list->anchors[i]->highest_possible_score;
        } 
        assert(game->gen->anchor_list->anchors[i]->highest_possible_score <= previous_score);
    }
    
}

void quick_test(SuperConfig * superconfig) {
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Player * player = game->players[0];

    printf("\n\n\nGEN START\n\n\n");

    load_and_generate(game, player, KA_OPENING_CGP, "E?");
    print_game(game);
    transpose(game->gen->board);
    print_game(game);
    transpose(game->gen->board);
    printf("\n\n\nDEBUG START\n\n\n");
    for (int i = 0; i < game->gen->anchor_list->count; i++) {
        printf("%d: %d, %d, %d, %d, %d\n", i, game->gen->anchor_list->anchors[i]->row, 
        game->gen->anchor_list->anchors[i]->col, 
        game->gen->anchor_list->anchors[i]->vertical, 
        game->gen->anchor_list->anchors[i]->transpose_state, 
        game->gen->anchor_list->anchors[i]->highest_possible_score);
    }

    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 156);
}

void test_shadow(SuperConfig * superconfig) {
    // quick_test(superconfig);
    // return;
    Config * config = get_csw_config(superconfig);
    Game * game = create_game(config);
    Player * player = game->players[0];

    load_and_generate(game, player, EMPTY_CGP, "OU");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 4);

    load_and_generate(game, player, EMPTY_CGP, "ID");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 6);

    load_and_generate(game, player, EMPTY_CGP, "AX");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 18);

    load_and_generate(game, player, EMPTY_CGP, "BD");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 10);

    load_and_generate(game, player, EMPTY_CGP, "QK");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 30);

    load_and_generate(game, player, EMPTY_CGP, "AESR");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 8);

    load_and_generate(game, player, EMPTY_CGP, "TNCL");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 12);

    load_and_generate(game, player, EMPTY_CGP, "AAAAA");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 12);

    load_and_generate(game, player, EMPTY_CGP, "CAAAA");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 20);

    load_and_generate(game, player, EMPTY_CGP, "CAKAA");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 32);

    load_and_generate(game, player, EMPTY_CGP, "AIERZ");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 48);

    load_and_generate(game, player, EMPTY_CGP, "AIERZN");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 50);

    load_and_generate(game, player, EMPTY_CGP, "AIERZNL");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 102);

    load_and_generate(game, player, EMPTY_CGP, "?");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 0);

    load_and_generate(game, player, EMPTY_CGP, "??");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 0);

    load_and_generate(game, player, EMPTY_CGP, "??OU");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 4);

    load_and_generate(game, player, EMPTY_CGP, "??OUA");
    assert(game->gen->anchor_list->count == 1);
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 8);

    load_and_generate(game, player, KA_OPENING_CGP, "EE");
    // KAE and EE
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 10);
    // EKE
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 9);
    // KAEE
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 8);
    // EE and E(A)
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 5);
    // EE and E(A)
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 5);
    // EEE
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 3);
    // The rest are prevented by invalid cross sets
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 0);

    load_and_generate(game, player, KA_OPENING_CGP, "E?");
    // oK, oE, EA
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 10);
    // KA, aE, AE
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 10);
    // KAe, Ee
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 8);
    // EKA, Ea
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 8);
    // KAEe
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 7);
    // E(K)e
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 7);
    // Ea, EA
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 3);
    // Ae, eE
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 3);
    // E(A)a
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 2);

    load_and_generate(game, player, KA_OPENING_CGP, "J");
    // J(KA) or (KA)J
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 14);
    // JA
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 9);
    // The rest are invalid cross sets.
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 0);

    load_and_generate(game, player, AA_OPENING_CGP, "JF");
    // JF, JA, and FA
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 42);
    // JA and JF or FA and FJ
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 25);
    // JAF with J and F doubled
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 25);
    // FAA is in cross set, so JAA and JF are used to score.
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 22);
    // AAJF
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 14);
    // AJF
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 13);
    // Remaining anchors are prevented by invalid cross sets
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 0);
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 0);

    // Makeing JA, FA, and JFU, doubling the U on the double letter
    load_and_generate(game, player, AA_OPENING_CGP, "JFU");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 44);

    // Making KAU (allowed by F in rack cross set) and JUF, doubling the F and J.
    load_and_generate(game, player, KA_OPENING_CGP, "JFU");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 32);

    load_and_generate(game, player, AA_OPENING_CGP, "JFUG");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 47);

    load_and_generate(game, player, AA_OPENING_CGP, "JFUGX");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 61);

    // Reaches the triple word
    load_and_generate(game, player, AA_OPENING_CGP, "JFUGXL");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 102);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "Q");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 22);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BD");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 17);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOH");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 60);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGX");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 90);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZ");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 120);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 230);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "A");
    // WINDYA
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 13);
    // PROTEANA
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 11);
    // ANY
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 6);
    // PA
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 4);
    // AR
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 2);
    // The rest are prevented by invalid cross sets
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 0);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "Z");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 32);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW");
    // ZEN, ZW, WAD
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 73);
    // ZENLW
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 45);
    // ZLWOW
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 40);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW?");
    // The blank makes all cross sets valid
    // LZW(WINDY)s
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 99);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "QZLW");
    // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 85);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "K");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 23);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT");
    // KPAVT
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 26);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT?");
    // The blank makes PAVE, allowed all letters in the cross set
    // KPAVT
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 39);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "M");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 8);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MN");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 16);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNA");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 20);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAU");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 22);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 30);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 39);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z");
    // LATERZ
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 30);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 64);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 68);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 72);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 77);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 80);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 212);

    load_and_generate(game, player, VS_OXY, "A");
    // APACIFYING
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 63);

    load_and_generate(game, player, VS_OXY, "PB");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 156);

    load_and_generate(game, player, VS_OXY, "PA");
    // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 76);

    load_and_generate(game, player, VS_OXY, "PBA");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 174);

    load_and_generate(game, player, VS_OXY, "Z");
    // ZPACIFYING
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 90);

    load_and_generate(game, player, VS_OXY, "ZE");
    // ZONE
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 160);

    load_and_generate(game, player, VS_OXY, "AZE");
    // UTAZONE
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 184);

    load_and_generate(game, player, VS_OXY, "AZEB");
    // HENBUTAZONE
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 484);

    load_and_generate(game, player, VS_OXY, "AZEBP");
    // YPHENBUTAZONE
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 604);

    load_and_generate(game, player, VS_OXY, "AZEBPX");
    // A2 A(Y)X(HEN)P(UT)EZ(ON)B
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 740);

    load_and_generate(game, player, VS_OXY, "AZEBPXO");
    // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 1924);

    load_and_generate(game, player, VS_OXY, "AZEBPQO");
    // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
    // Only the letters AZEBPO are required to form acceptable
    // plays in all cross sets
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 2036);    
}
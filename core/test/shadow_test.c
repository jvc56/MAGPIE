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

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "EE");

    printf("\n\n\nDEBUG START\n\n\n");
    for (int i = 0; i < game->gen->anchor_list->count; i++) {
        printf("%d: %d, %d, %d, %d, %d\n", i, game->gen->anchor_list->anchors[i]->row, 
        game->gen->anchor_list->anchors[i]->col, 
        game->gen->anchor_list->anchors[i]->vertical, 
        game->gen->anchor_list->anchors[i]->transpose_state, 
        game->gen->anchor_list->anchors[i]->highest_possible_score);
    }

    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 7);
}

void test_shadow(SuperConfig * superconfig) {
    quick_test(superconfig);
    return;
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

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "EE");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 12);
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 12);
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 10);
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 9);
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 9);
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 8);
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 5);
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 5);
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 3);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "J");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 21);
    assert(game->gen->anchor_list->anchors[1]->highest_possible_score == 21);
    assert(game->gen->anchor_list->anchors[2]->highest_possible_score == 21);
    assert(game->gen->anchor_list->anchors[3]->highest_possible_score == 14);
    assert(game->gen->anchor_list->anchors[4]->highest_possible_score == 14);
    assert(game->gen->anchor_list->anchors[5]->highest_possible_score == 14);
    assert(game->gen->anchor_list->anchors[6]->highest_possible_score == 9);
    assert(game->gen->anchor_list->anchors[7]->highest_possible_score == 9);
    assert(game->gen->anchor_list->anchors[8]->highest_possible_score == 9);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "JF");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 46);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "JFU");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 48);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "JFUG");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 51);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "JFUGX");    
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 65);

    load_and_generate(game, player, TWO_LETTER_OPENING_CGP, "JFUGXL");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 114);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "Q");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 22);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BD");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 22);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOH");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 60);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGX");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 90);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZ");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 120);

    load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 230);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "A");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 15);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "Z");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 33);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 80);

    load_and_generate(game, player, TRIPLE_LETTERS_CGP, "QZLW");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 135);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "K");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 23);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 39);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KTU");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 40);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KTUI");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 41);

    load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KTUIA");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 61);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "M");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 8);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MN");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 18);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNA");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 20);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAU");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 22);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 30);

    load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 39);

    load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 31);

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
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 84);

    load_and_generate(game, player, VS_OXY, "AB");
    assert(game->gen->anchor_list->anchors[0]->highest_possible_score == 144);
}
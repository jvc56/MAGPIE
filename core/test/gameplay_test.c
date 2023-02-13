#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/bag.h"
#include "../src/board.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/player.h"

#include "game_print.h"
#include "test_util.h"
#include "test_config.h"

void draw_rack_to_string(Bag * bag, Rack * rack, char * letters, Alphabet * alphabet) {
    for (size_t i = 0; i < strnlen(letters, 7); i++) {
        draw_letter_to_rack(bag, rack, val(alphabet, letters[i]));
    }
}

void return_rack_to_bag(Rack * rack, Bag * bag) {
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        for (int j = 0; j < rack->array[i]; j++) {
            add_letter(bag, i);
        }
    }
    reset_rack(rack);
}

void return_racks_to_bag(Game * game) {
    return_rack_to_bag(game->players[0]->rack, game->gen->bag);
    return_rack_to_bag(game->players[1]->rack, game->gen->bag);
}

void assert_players_are_equal(Player * p1, Player * p2, int check_scores) {
    // For games ending in consecutive zeros, scores are checked elsewhere
    if (check_scores) {
        assert(p1->score == p2->score);
    }
}

void assert_boards_are_equal(Board * b1, Board * b2) {
    assert(b1->transposed == b2->transposed);
    assert(b1->tiles_played == b2->tiles_played);
    for (int i = 0; i < (BOARD_DIM * BOARD_DIM * 2); i++) {
        if (i < BOARD_DIM * BOARD_DIM) {
            assert(b1->letters[i] == b2->letters[i]);
            assert(b1->bonus_squares[i] == b2->bonus_squares[i]);
        }
        assert(b1->cross_sets[i] == b2->cross_sets[i]);
        assert(b1->cross_scores[i] == b2->cross_scores[i]);
        assert(b1->anchors[i] == b2->anchors[i]);
    }
}

void assert_bags_are_equal(Bag * b1, Bag * b2) {
    assert(b1->last_tile_index == b2->last_tile_index);

    uint8_t sb1[(RACK_ARRAY_SIZE)];
    uint8_t sb2[(RACK_ARRAY_SIZE)];

    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        sb1[i] = 0;
        sb2[i] = 0;
    }
    
    for (int i = 0; i <= b1->last_tile_index; i++) {
        sb1[b1->tiles[i]]++;
        sb2[b2->tiles[i]]++;
    }

    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        assert(sb1[i] == sb2[i]);
    }
}

void assert_games_are_equal(Game * g1, Game * g2, int check_scores) {
    assert(g1->consecutive_scoreless_turns == g2->consecutive_scoreless_turns);
    assert(g1->game_end_reason == g2->game_end_reason);

    Player * g1_player_on_turn = g1->players[g1->player_on_turn_index];
    Player * g1_player_not_on_turn = g1->players[1 - g1->player_on_turn_index];

    Player * g2_player_on_turn = g2->players[g2->player_on_turn_index];
    Player * g2_player_not_on_turn = g2->players[1 - g2->player_on_turn_index];

    assert_players_are_equal(g1_player_on_turn, g2_player_on_turn, check_scores);
    assert_players_are_equal(g1_player_not_on_turn, g2_player_not_on_turn, check_scores);

    assert_boards_are_equal(g1->gen->board, g2->gen->board);
    assert_bags_are_equal(g1->gen->bag, g2->gen->bag);
}

void test_gameplay_by_turn(Config * config, char * cgps[], char * racks[], int array_length) {
    Game * actual_game = create_game(config);
    Game * expected_game = create_game(config);

    int player0_last_score_on_rack = -1;
    int player1_last_score_on_rack = -1;
    int player0_final_score = -1;
    int player1_final_score = -1;
    int player0_score_before_last_move = -1;
    int player1_score_before_last_move = -1;

    for (int i = 0; i < array_length; i++) {
        assert(actual_game->game_end_reason == GAME_END_REASON_NONE);
        return_racks_to_bag(actual_game);
        draw_rack_to_string(actual_game->gen->bag ,actual_game->players[actual_game->player_on_turn_index]->rack, racks[i], actual_game->gen->gaddag->alphabet);
        // If it's the last turn, have the opponent draw the remaining tiles
        // so the end of actual_game subtractions are correct. If the bag has less
        // than RACK_SIZE tiles, have the opponent draw the remaining tiles
        // so the endgame adjustments are added to the move equity values.
        if (i == array_length - 1 || actual_game->gen->bag->last_tile_index + 1 < RACK_SIZE) {
            draw_at_most_to_rack(actual_game->gen->bag, actual_game->players[1 - actual_game->player_on_turn_index]->rack, RACK_SIZE);
        }

        if (i == array_length - 1) {
            player0_score_before_last_move = actual_game->players[0]->score;
            player1_score_before_last_move = actual_game->players[1]->score;
        }

        play_top_n_equity_move(actual_game, 0);

        if (i == array_length - 1) {
            player0_last_score_on_rack = score_on_rack(actual_game->gen->letter_distribution, actual_game->players[0]->rack);
            player1_last_score_on_rack = score_on_rack(actual_game->gen->letter_distribution, actual_game->players[1]->rack);
            player0_final_score = actual_game->players[0]->score;
            player1_final_score = actual_game->players[1]->score;
        }


        reset_game(expected_game);
        load_cgp(expected_game, cgps[i]);
        // If the game is still ongoing,
        // return the racks to the bag so that
        // the bag from the expected game and
        // the actual game match. If this is
        // the last position of a standard game, there
        // is no randomness for the rack draw
        // since there should be less than seven
        // tiles in the bag. If this is the last position
        // of a six pass game, we need to return the
        // tiles because those tiles could be random, and
        // so the bags probably won't match.
        if (i != array_length - 1 || expected_game->game_end_reason == GAME_END_REASON_CONSECUTIVE_ZEROS) {
            return_racks_to_bag(actual_game);
        }
        assert_games_are_equal(expected_game, actual_game, i != array_length - 1);
    }

    if (actual_game->game_end_reason == GAME_END_REASON_CONSECUTIVE_ZEROS) {
        assert(player0_score_before_last_move - player0_last_score_on_rack == player0_final_score);
        assert(player1_score_before_last_move - player1_last_score_on_rack == player1_final_score);
    }

    destroy_game(actual_game);
    destroy_game(expected_game);
}

void test_six_exchanges_game(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);

    char * racks[18] = {
		"UUUVVWW",
		"AEFRWYZ",
		"INOOQSU",
		"LUUUVVW",
		"EEEEEOO",
		"AEIKLMO",
		"GNOOOPR",
		"EGIJLRS",
		"EEEOTTT",
		"EIILRSX",
		"?CEEILT",
		"?AFERST",
		"AAAAAAI",
		"GUUUVVW",
		"AEEEEEO",
		"AAAAAII",
		"AEUUUVV",
		"AEEEEII"
	};

    char * cgps[18] = {
        "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 1 lex CSW21;",
        "15/15/15/15/15/15/15/7FRAWZEY1/15/15/15/15/15/15/15 / 0/120 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 120/150 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 150/120 1 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 120/150 2 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/7K7/7E7 / 150/224 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/7K7/GONOPORE7 / 224/236 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/1JIG3K7/GONOPORE7 / 236/269 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A7/7M7/7L7/7I7/1JIG3K7/GONOPORE7 / 269/265 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/7M7/7L7/7I7/1JIG3K7/GONOPORE7 / 265/297 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/7M7/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 297/341 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 0 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 1 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 2 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 3 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 4 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 5 lex CSW21;",
        "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/5ELICITEd2/1JIG3K7/GONOPORE7 / 332/384 6 lex CSW21;"
    };
    test_gameplay_by_turn(config, cgps, racks, 18);

    
}

void test_six_passes_game(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);

	char * racks[31] = {
		"AEGILPR",
		"ACELNTV",
		"DDEIOTY",
		"?ADIIUU",
		"?BEIINS",
		"EEEKMNO",
		"AAEHINT",
		"CDEGORZ",
		"EGNOQRS",
		"AFIQRRT",
		"ERSSTTX",
		"BGHNOOU",
		"AENRTUZ",
		"AFIMNRV",
		"AEELNOT",
		"?EORTUW",
		"ILNOOST",
		"EEINRUY",
		"?AENRTU",
		"EEINRUW",
		"AJNPRV",
		"INRU",
		"PRV",
		"U",
		"RV",
		"U",
		"V",
		"U",
		"V",
		"U",
		"V"
	};

    char * cgps[31] = {
        "15/15/15/15/15/15/15/3PAIGLE6/15/15/15/15/15/15/15 / 0/24 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/15/15/15/15 / 24/48 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/2ODDITY7/15/15/15 / 48/68 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/2ODDITY7/1DUI11/15/15 / 68/63 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4EBIoNISE3/2ODDITY7/1DUI11/15/15 / 63/146 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/2ODDITY7/1DUI11/15/15 / 146/110 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/2ODDITY7/1DUI11/15/15 / 110/172 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/2ODDITY7/1DUI11/CODGER9/15 / 172/149 0 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/2ODDITY7/1DUI11/CODGER9/15 / 149/172 1 lex CSW21;",
        "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N5FAQIR/4C2MOKE4/4EBIoNISE3/2ODDITY7/1DUI11/CODGER9/15 / 172/182 0 lex CSW21;",
        "15/15/15/15/4V10/4A6T3/4L1AAH2E3/3PAIGLE2X3/4N5FAQIR/4C2MOKES3/4EBIoNISE3/2ODDITY3S3/1DUI11/CODGER9/15 / 182/227 0 lex CSW21;",
        "15/15/15/15/4V10/4A6T3/4L1AAH2E2B/3PAIGLE2X2O/4N5FAQIR/4C2MOKES2O/4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 227/227 0 lex CSW21;",
        "15/15/15/15/4V10/4A6T3/4L1AAH2E2B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 227/292 0 lex CSW21;",
        "15/15/12F2/12R2/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 292/262 0 lex CSW21;",
        "15/15/12F2/12R2/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER9/15 / 262/316 0 lex CSW21;",
        "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER9/15 / 316/284 0 lex CSW21;",
        "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER2LOTIONS/15 / 284/400 0 lex CSW21;",
        "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / 400/314 0 lex CSW21;",
        "15/15/12F2/11TROW/4V7A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / 314/474 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / 474/338 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / 338/499 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 499/349 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 349/510 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 510/349 1 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 349/517 0 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 517/349 1 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 349/517 2 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 517/349 3 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 349/517 4 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 517/349 5 lex CSW21;",
        "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 348/513 6 lex CSW21;"
    };

    test_gameplay_by_turn(config, cgps, racks, 31);

    
}

void test_standard_game(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);

	char * racks[23] = {
		"EGIILNO",
		"DRRTYYZ",
		"CEIOTTU",
		"AADEEMT",
		"AACDEKS",
		"BEEIOOP",
		"DHLNORR",
		"BGIIJRV",
		"?DFMNPU",
		"EEEOQRW",
		"IINNSVW",
		"?ADEOPU",
		"EFOTTUV",
		"ADHIMNX",
		"CEFINQS",
		"?ADRSTT",
		"?CIRRSU",
		"AEEFGIL",
		"EEGHLMN",
		"AAAEELL",
		"DEEGLNN",
		"AEGILUY",
		"EN"
	};

    // Assign the rack on the last CGP so that the CGP load
    // function ends the game.
    char * cgps[23] = {
        "15/15/15/15/15/15/15/6OILING3/15/15/15/15/15/15/15 / 0/18 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/9Z5/9Y5/15/15/15/15 / 18/37 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZO4/9YU4/10T4/15/15/15 / 37/45 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZO4/9YU4/10T4/6EDEMATA2/15/15 / 45/115 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZOS3/9YUK3/10TA3/6EDEMATA2/11E3/11D3 / 115/97 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/7I3E3/7E3D3 / 97/145 0 lex CSW21;",
        "15/15/15/15/15/15/9R5/6OILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/5RHINO1E3/7E3D3 / 145/122 0 lex CSW21;",
        "15/15/15/15/15/5J9/5I3R5/5BOILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/5RHINO1E3/7E3D3 / 122/183 0 lex CSW21;",
        "15/15/15/15/15/5J9/5IF2R5/5BOILING3/6P2T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/5RHINO1E3/7E3D3 / 183/146 0 lex CSW21;",
        "15/15/15/15/15/5J9/5IF2R5/5BOILING3/6P2T5/5R1B1ZOS3/5E1O1YUK3/5W1O2TA3/5OEDEMATA2/5RHINO1E3/5E1E3D3 / 146/205 0 lex CSW21;",
        "15/15/11W3/11I3/11V3/5J5I3/5IF2R1N3/5BOILING3/6P2T5/5R1B1ZOS3/5E1O1YUK3/5W1O2TA3/5OEDEMATA2/5RHINO1E3/5E1E3D3 / 205/172 0 lex CSW21;",
        "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/5E1O1YUK3/5W1O2TA3/5OEDEMATA2/5RHINO1E3/5E1E3D3 / 172/236 0 lex CSW21;",
        "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/5E1O1YUKO2/5W1O2TAV2/5OEDEMATA2/5RHINO1ET2/5E1E3DE2 / 236/202 0 lex CSW21;",
        "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/5E1O1YUKO2/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 202/271 0 lex CSW21;",
        "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/6P1AT3N1/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 271/250 0 lex CSW21;",
        "15/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/6P1AT3N1/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 250/346 0 lex CSW21;",
        "1CURRIeS7/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/6P1AT3N1/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 346/335 0 lex CSW21;",
        "1CURRIeS7/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 335/378 0 lex CSW21;",
        "1CURRIeS7/1HM2TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 378/367 0 lex CSW21;",
        "1CURRIeS7/1HM2TeTRADS3/11W3/11I3/10AVALE/5J2P2I3/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 367/394 0 lex CSW21;",
        "1CURRIeS6L/1HM2TeTRADS2E/11W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 394/397 0 lex CSW21;",
        "1CURRIeS6L/1HM2TeTRADS2E/11W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEXY / 397/439 0 lex CSW21;",
        "1CURRIeS6L/1HM2TeTRADS2E/2EN7W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEXY AEGILU/ 439/425 0 lex CSW21;"
    };

    test_gameplay_by_turn(config, cgps, racks, 23);
}

void test_playmove(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);
    Game * game = create_game(config);

    // Test play
    draw_rack_to_string(game->gen->bag, game->players[0]->rack, "DEKNRTY", game->gen->gaddag->alphabet);
    play_top_n_equity_move(game, 0);

    assert(game->consecutive_scoreless_turns == 0);
    assert(game->game_end_reason == GAME_END_REASON_NONE);
    assert(game->players[0]->score == 36);
    assert(game->players[1]->score == 0);
    assert(!game->players[0]->rack->empty);
    assert(game->players[0]->rack->number_of_letters == 7);
    assert(get_letter(game->gen->board, 7, 3) == val(game->gen->gaddag->alphabet, 'K'));
    assert(get_letter(game->gen->board, 7, 4) == val(game->gen->gaddag->alphabet, 'Y'));
    assert(get_letter(game->gen->board, 7, 5) == val(game->gen->gaddag->alphabet, 'N'));
    assert(get_letter(game->gen->board, 7, 6) == val(game->gen->gaddag->alphabet, 'D'));
    assert(get_letter(game->gen->board, 7, 7) == val(game->gen->gaddag->alphabet, 'E'));
    assert(game->player_on_turn_index == 1);
    assert(game->gen->bag->last_tile_index + 1 == 88);
    assert(game->gen->board->tiles_played == 5);
    reset_game(game);

    // Test exchange
    draw_rack_to_string(game->gen->bag, game->players[0]->rack, "UUUVVWW", game->gen->gaddag->alphabet);
    play_top_n_equity_move(game, 0);

    assert(game->consecutive_scoreless_turns == 1);
    assert(game->game_end_reason == GAME_END_REASON_NONE);
    assert(game->players[0]->score == 0);
    assert(game->players[1]->score == 0);
    assert(!game->players[0]->rack->empty);
    assert(game->players[0]->rack->number_of_letters == 7);
    assert(game->player_on_turn_index == 1);
    assert(game->gen->bag->last_tile_index + 1 == 93);
    assert(game->gen->board->tiles_played == 0);
    assert(game->players[0]->rack->array[val(game->gen->gaddag->alphabet, 'V')] == 0);
    assert(game->players[0]->rack->array[val(game->gen->gaddag->alphabet, 'W')] == 0);
    assert(game->players[0]->rack->array[val(game->gen->gaddag->alphabet, 'U')] < 2);
    reset_game(game);

    // Test pass
    load_cgp(game, "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 517/349 5 lex CSW21;");
    draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, 1);
    draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, 1);

    int player0_score = game->players[0]->score;
    int player1_score = game->players[1]->score;
    assert(game->consecutive_scoreless_turns == 5);
    assert(game->game_end_reason == GAME_END_REASON_NONE);
    assert(player0_score == 517);
    assert(player1_score == 349);
    assert(!game->players[0]->rack->empty);
    assert(game->players[0]->rack->number_of_letters == 1);
    assert(!game->players[1]->rack->empty);
    assert(game->players[1]->rack->number_of_letters == 1);
    assert(game->player_on_turn_index == 0);
    assert(game->gen->bag->last_tile_index + 1 == 0);

    play_top_n_equity_move(game, 0);

    assert(game->consecutive_scoreless_turns == 6);
    assert(game->game_end_reason == GAME_END_REASON_CONSECUTIVE_ZEROS);
    assert(game->players[0]->score == player0_score - score_on_rack(game->gen->letter_distribution ,game->players[0]->rack));
    assert(game->players[1]->score == player1_score - score_on_rack(game->gen->letter_distribution ,game->players[1]->rack));
    assert(!game->players[0]->rack->empty);
    assert(game->players[0]->rack->number_of_letters == 1);
    assert(!game->players[1]->rack->empty);
    assert(game->players[1]->rack->number_of_letters == 1);
    assert(game->gen->bag->last_tile_index + 1 == 0);

    destroy_game(game);
    
}

void test_gameplay(TestConfig * test_config) {
    test_playmove(test_config);
    test_six_exchanges_game(test_config);
    test_six_passes_game(test_config);
    test_standard_game(test_config);
}
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../src/config.h"
#include "../src/cross_set.h"
#include "../src/game.h"

#include "test_constants.h"
#include "test_util.h"
#include "superconfig.h"

uint64_t cross_set_from_string(const char* letters, Alphabet* alph) {
    if (strcmp(letters, "TRIVIAL") == 0) {
        return TRIVIAL_CROSS_SET;
    }
    uint64_t c = 0;
    for (size_t i = 0; i < strlen(letters); i++) {
        set_cross_set_letter(&c,  val(alph, letters[i]));
    }
    return c;
}

void set_row(Game * game, int row, const char* row_content) {
    for (int i = 0; i < BOARD_DIM; i++) {
        set_letter(game->gen->board, row, i, ALPHABET_EMPTY_SQUARE_MARKER);
    }

    for (size_t i = 0; i < strlen(row_content); i++) {
        if (row_content[i] != ' ') {
            set_letter(game->gen->board, row, i, val(game->gen->gaddag->alphabet, row_content[i]));
            game->gen->board->tiles_played++;
        }
    }
}

void test_gen_cross_set(Game * game, int row, int col, int dir, const char* letters, int expected_cross_score, int run_gcs) {
    if (run_gcs) {
        gen_cross_set(game->gen->board, row, col, dir, game->gen->gaddag, game->gen->letter_distribution);
    }
    uint64_t expected_cross_set = cross_set_from_string(letters, game->gen->gaddag->alphabet);
    uint64_t actual_cross_set = get_cross_set(game->gen->board, row, col, dir);
    assert(expected_cross_set == actual_cross_set);
    int actual_cross_score = get_cross_score(game->gen->board, row, col, dir);
    assert(expected_cross_score == actual_cross_score);
}

void test_gen_cross_set_row(Game * game, int row, int col, const char* row_content, const char* letters, int expected_cross_score, int run_gcs) {
    set_row(game, row, row_content);
    test_gen_cross_set(game, row, col, 0, letters, expected_cross_score, run_gcs);
}

void test_cross_set(SuperConfig * superconfig) {
    Config * config = get_america_config(superconfig);
    Game * game = create_game(config);

    // TestGencross_setLoadedGame
    load_cgp(game, VS_MATT);
    test_gen_cross_set(game, 10, 10, BOARD_HORIZONTAL_DIRECTION, "E", 11, 1);
    test_gen_cross_set(game, 2, 4, BOARD_HORIZONTAL_DIRECTION, "DHKLRSV", 9, 1);
    test_gen_cross_set(game, 8, 7, BOARD_HORIZONTAL_DIRECTION, "S", 11, 1);
    test_gen_cross_set(game, 12, 8, BOARD_HORIZONTAL_DIRECTION, "", 11, 1);
    test_gen_cross_set(game, 3, 1, BOARD_HORIZONTAL_DIRECTION, "", 10, 1);
    test_gen_cross_set(game, 6, 8, BOARD_HORIZONTAL_DIRECTION, "", 5, 1);
    test_gen_cross_set(game, 2, 10, BOARD_HORIZONTAL_DIRECTION, "M", 2, 1);

    // TestGencross_setEdges
    reset_game(game);
    test_gen_cross_set_row(game, 4, 0, " A", "ABDFHKLMNPTYZ", 1, 1);
    test_gen_cross_set_row(game, 4, 1, "A", "ABDEGHILMNRSTWXY", 1, 1);
    test_gen_cross_set_row(game, 4, 13, "              F", "EIO", 4, 1);
    test_gen_cross_set_row(game, 4, 14, "             F ", "AE", 4, 1);
    test_gen_cross_set_row(game, 4, 14, "          WECH ", "T", 12, 1);
    test_gen_cross_set_row(game, 4, 14, "           ZZZ ", "", 30, 1);
    test_gen_cross_set_row(game, 4, 14, "       ZYZZYVA ", "S", 43, 1);
    test_gen_cross_set_row(game, 4, 14, "        ZYZZYV ", "A", 42, 1);
    test_gen_cross_set_row(game, 4, 14, "       Z Z Y A ", "ABDEGHILMNRSTWXY", 1, 1);
    test_gen_cross_set_row(game, 4, 12, "       z z Y A ", "E", 5, 1);
    test_gen_cross_set_row(game, 4, 14, "OxYpHeNbUTAzON ", "E", 15, 1);
    test_gen_cross_set_row(game, 4, 6, "OXYPHE BUTAZONE", "N", 40, 1);
    test_gen_cross_set_row(game, 4, 0, " YHJKTKHKTLV", "", 42, 1);
    test_gen_cross_set_row(game, 4, 14, "   YHJKTKHKTLV ", "", 42, 1);
    test_gen_cross_set_row(game, 4, 6, "YHJKTK HKTLV", "", 42, 1);

    // TestGenAllcross_sets
    reset_game(game);
    load_cgp(game, VS_ED);
    test_gen_cross_set(game, 8, 8, BOARD_HORIZONTAL_DIRECTION, "OS", 8, 0);
	test_gen_cross_set(game, 8, 8, BOARD_VERTICAL_DIRECTION, "S", 9, 0);
	test_gen_cross_set(game, 5, 11, BOARD_HORIZONTAL_DIRECTION, "S", 5, 0);
	test_gen_cross_set(game, 5, 11, BOARD_VERTICAL_DIRECTION, "AO", 2, 0);
	test_gen_cross_set(game, 8, 13, BOARD_HORIZONTAL_DIRECTION, "AEOU", 1, 0);
	test_gen_cross_set(game, 8, 13, BOARD_VERTICAL_DIRECTION, "AEIMOUY", 3, 0);
	test_gen_cross_set(game, 9, 13, BOARD_HORIZONTAL_DIRECTION, "HMNPST", 1, 0);
	test_gen_cross_set(game, 9, 13, BOARD_VERTICAL_DIRECTION, "TRIVIAL", 0, 0);
	test_gen_cross_set(game, 14, 14, BOARD_HORIZONTAL_DIRECTION, "TRIVIAL", 0, 0);
	test_gen_cross_set(game, 14, 14, BOARD_VERTICAL_DIRECTION, "TRIVIAL", 0, 0);
	test_gen_cross_set(game, 12, 12, BOARD_HORIZONTAL_DIRECTION, "", 0, 0);
	test_gen_cross_set(game, 12, 12, BOARD_VERTICAL_DIRECTION, "", 0, 0);

    // TestUpdateSinglecross_set
    reset_game(game);
    load_cgp(game, VS_MATT);
	set_letter(game->gen->board, 8, 10, 19);
	set_letter(game->gen->board, 9, 10, 0);
	set_letter(game->gen->board, 10, 10, 4);
	set_letter(game->gen->board, 11, 10, 11);
    gen_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION, game->gen->gaddag, game->gen->letter_distribution);
    transpose(game->gen->board);
    gen_cross_set(game->gen->board, 10, 7, BOARD_VERTICAL_DIRECTION, game->gen->gaddag, game->gen->letter_distribution);
    transpose(game->gen->board);
    assert(get_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION) == 0);
    assert(get_cross_set(game->gen->board, 7, 10, BOARD_VERTICAL_DIRECTION) == 0);

    destroy_game(game);
    
}
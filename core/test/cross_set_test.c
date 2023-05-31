#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../src/config.h"
#include "../src/cross_set.h"
#include "../src/game.h"
#include "../src/letter_distribution.h"

#include "test_constants.h"
#include "test_util.h"
#include "superconfig.h"

uint64_t cross_set_from_string(const char* letters, LetterDistribution * letter_distribution) {
    if (strcmp(letters, "TRIVIAL") == 0) {
        return TRIVIAL_CROSS_SET;
    }
    uint64_t c = 0;
    for (size_t i = 0; i < strlen(letters); i++) {
        set_cross_set_letter(&c,  human_readable_letter_to_machine_letter(letter_distribution, letters[i]));
    }
    return c;
}

void set_row(Game * game, int row, const char* row_content) {
    for (int i = 0; i < BOARD_DIM; i++) {
        set_letter(game->gen->board, row, i, ALPHABET_EMPTY_SQUARE_MARKER);
    }

    for (size_t i = 0; i < strlen(row_content); i++) {
        if (row_content[i] != ' ') {
            set_letter(game->gen->board, row, i, human_readable_letter_to_machine_letter(game->gen->letter_distribution, row_content[i]));
            game->gen->board->tiles_played++;
        }
    }
}

void set_col(Game * game, int col, const char* col_content) {
    for (int i = 0; i < BOARD_DIM; i++) {
        set_letter(game->gen->board, i, col, ALPHABET_EMPTY_SQUARE_MARKER);
    }

    for (size_t i = 0; i < strlen(col_content); i++) {
        if (col_content[i] != ' ') {
            set_letter(game->gen->board, i, col, human_readable_letter_to_machine_letter(game->gen->letter_distribution, col_content[i]));
            game->gen->board->tiles_played++;
        }
    }
}

void test_gen_cross_set(Game * game, int row, int col, int dir, const char* letters, int expected_cross_score, int run_gcs) {
    if (run_gcs) {
        gen_cross_set(game->gen->board, row, col, dir, game->gen->kwg, game->gen->letter_distribution);
    }
    uint64_t expected_cross_set = cross_set_from_string(letters, game->gen->letter_distribution);
    uint64_t actual_cross_set = get_cross_set(game->gen->board, row, col, dir);
    assert(expected_cross_set == actual_cross_set);
    int actual_cross_score = get_cross_score(game->gen->board, row, col, dir);
    assert(expected_cross_score == actual_cross_score);
}

void test_gen_cross_set_row(Game * game, int row, int col, int dir, const char* row_content, const char* letters, int expected_cross_score, int run_gcs) {
    set_row(game, row, row_content);
    test_gen_cross_set(game, row, col, dir, letters, expected_cross_score, run_gcs);
}

void test_gen_cross_set_col(Game * game, int row, int col, int dir, const char* col_content, const char* letters, int expected_cross_score, int run_gcs) {
    set_col(game, col, col_content);
    test_gen_cross_set(game, row, col, dir, letters, expected_cross_score, run_gcs);
}

void test_cross_set(SuperConfig * superconfig) {
    Config * config = get_nwl_config(superconfig);
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
    test_gen_cross_set_row(game, 4, 0, 0, " A", "ABDFHKLMNPTYZ", 1, 1);
    test_gen_cross_set_row(game, 4, 1, 0, "A", "ABDEGHILMNRSTWXY", 1, 1);
    test_gen_cross_set_row(game, 4, 13, 0, "              F", "EIO", 4, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "             F ", "AE", 4, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "          WECH ", "T", 12, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "           ZZZ ", "", 30, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "       ZYZZYVA ", "S", 43, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "        ZYZZYV ", "A", 42, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "       Z Z Y A ", "ABDEGHILMNRSTWXY", 1, 1);
    test_gen_cross_set_row(game, 4, 12, 0, "       z z Y A ", "E", 5, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "OxYpHeNbUTAzON ", "E", 15, 1);
    test_gen_cross_set_row(game, 4, 6, 0, "OXYPHE BUTAZONE", "N", 40, 1);
    test_gen_cross_set_row(game, 4, 0, 0, " YHJKTKHKTLV", "", 42, 1);
    test_gen_cross_set_row(game, 4, 14, 0, "   YHJKTKHKTLV ", "", 42, 1);
    test_gen_cross_set_row(game, 4, 6, 0, "YHJKTK HKTLV", "", 42, 1);

    // Test setting cross sets with tiles on either side
    test_gen_cross_set_row(game, 4, 1, 1, "D NATURES", "E", 9, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "D N", "AEIOU", 3, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "D NT", "EIU", 4, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "D NTS", "EIU", 5, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "R VOTED", "E", 10, 1);
    test_gen_cross_set_row(game, 4, 5, 1, "PHENY BUTAZONE", "L", 32, 1);
    test_gen_cross_set_row(game, 4, 6, 1, "OXYPHE BUTAZONE", "N", 40, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "R XED", "A", 12, 1);
    test_gen_cross_set_row(game, 4, 2, 1, "BA ED", "AKLNRSTY", 7, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "X Z", "", 18, 1);
    test_gen_cross_set_row(game, 4, 6, 1, "STRONG L", "Y", 8, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "W SIWYG", "Y", 16, 1);
    test_gen_cross_set_row(game, 4, 0, 1, " EMSTVO", "Z", 11, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "T UNFOLD", "", 11, 1);
    test_gen_cross_set_row(game, 4, 1, 1, "S OBCONIc", "", 11, 1);

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
    gen_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION, game->gen->kwg, game->gen->letter_distribution);
    transpose(game->gen->board);
    gen_cross_set(game->gen->board, 10, 7, BOARD_VERTICAL_DIRECTION, game->gen->kwg, game->gen->letter_distribution);
    transpose(game->gen->board);
    assert(get_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION) == 0);
    assert(get_cross_set(game->gen->board, 7, 10, BOARD_VERTICAL_DIRECTION) == 0);

    destroy_game(game);
    
}
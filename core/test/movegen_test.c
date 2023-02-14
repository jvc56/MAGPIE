#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../src/alphabet.h"
#include "../src/bag.h"
#include "../src/cross_set.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/move.h"
#include "../src/movegen.h"

#include "rack_test.h"
#include "cross_set_test.h"
#include "move_print.h"
#include "test_constants.h"
#include "test_util.h"
#include "test_config.h"

int count_scoring_plays(MoveList * ml) {
    int sum = 0;
    for (int i = 0; i < ml->count; i++) {
        if (ml->moves[i]->move_type == MOVE_TYPE_PLAY) {
            sum++;
        }
    }
    return sum;
}

int count_nonscoring_plays(MoveList * ml) {
    int sum = 0;
    for (int i = 0; i < ml->count; i++) {
        if (ml->moves[i]->move_type != MOVE_TYPE_PLAY) {
            sum++;
        }
    }
    return sum;
}

void boards_equal(Board * b1, Board * b2) {
    assert(b1->tiles_played == b2->tiles_played);
    for (int i = 0; i < BOARD_DIM; i++) {
        for (int j = 0; j < BOARD_DIM; j++) {
            assert(get_letter(b1, i, j) == get_letter(b2, i, j));
            assert(get_bonus_square(b1, i, j) == get_bonus_square(b2, i, j));
            assert(get_cross_score(b1, i, j, BOARD_HORIZONTAL_DIRECTION) == get_cross_score(b2, i, j, BOARD_HORIZONTAL_DIRECTION));
            assert(get_cross_score(b1, i, j, BOARD_VERTICAL_DIRECTION) == get_cross_score(b2, i, j, BOARD_VERTICAL_DIRECTION));
            assert(get_cross_set(b1, i, j, BOARD_HORIZONTAL_DIRECTION) == get_cross_set(b2, i, j, BOARD_HORIZONTAL_DIRECTION));
            assert(get_cross_set(b1, i, j, BOARD_VERTICAL_DIRECTION) == get_cross_set(b2, i, j, BOARD_VERTICAL_DIRECTION));
            assert(get_anchor(b1, i, j, 0) == get_anchor(b2, i, j, 0));
            assert(get_anchor(b1, i, j, 1) == get_anchor(b2, i, j, 1));
        }
    }
}

void execute_recursive_gen(Generator * gen, int col, Rack * rack, uint32_t node_index, int leftstrip, int rightstrip, int unique_play) {
    set_start_leave_index(gen, rack);
    recursive_gen(gen, col, rack, NULL, node_index, leftstrip, rightstrip, unique_play);
    sort_move_list(gen->move_list);
}

void test_simple_case(Game * game, Rack * rack, const char* rack_string, int current_anchor_col, int row, const char* row_string, int expected_plays) {
    reset_game(game);
    reset_rack(rack);
    game->gen->current_anchor_col = current_anchor_col;
    set_rack_to_string(rack, rack_string, game->gen->gaddag->alphabet);
    set_row(game, row, row_string);
    game->gen->current_row_index = row;
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);
    assert(expected_plays == game->gen->move_list->count);
    reset_game(game);
    reset_rack(rack);
}

void macondo_tests(TestConfig * test_config) {
    Config * config = get_america_config(test_config);
    Game * game = create_game(config);
    Rack * rack = create_rack();
    char test_string[100];
    reset_string(test_string);

    // TestGenBase
    clear_all_crosses (game->gen->board);
    game->gen->current_anchor_col = 0;
    game->gen->current_row_index = 4;

    set_rack_to_string(rack, "AEINRST", game->gen->gaddag->alphabet);
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);
    assert(game->gen->move_list->count == 0);

    // TestSimpleRowGen
    reset_board(game->gen->board);
    test_simple_case(game, rack, "P", 11, 2, "     REGNANT", 1);
	test_simple_case(game, rack, "O", 9, 2, "  PORTOLAN", 1);
	test_simple_case(game, rack, "S", 9, 2, "  PORTOLAN", 1);
	test_simple_case(game, rack, "?", 9, 2, "  PORTOLAN", 2);
	test_simple_case(game, rack, "TY", 7, 2, "  SOVRAN", 1);
	test_simple_case(game, rack, "ING", 6, 2, "  LAUGH", 1);
	test_simple_case(game, rack, "ZA", 3, 4, "  BE", 0);
	test_simple_case(game, rack, "AENPPSW", 14, 4, "        CHAWING", 1);
	test_simple_case(game, rack, "ABEHINT", 9, 4, "   THERMOS  A", 2);
	test_simple_case(game, rack, "ABEHITT", 8, 4, "  THERMOS A   ", 1);
	test_simple_case(game, rack, "TT", 10, 4, "  THERMOS A   ", 3);
	test_simple_case(game, rack, "A", 1, 4, " B", 1);
	test_simple_case(game, rack, "A", 1, 4, " b", 1);

    // TestGenThroughBothWaysAllowedLetters
    set_rack_to_string(rack, "ABEHINT", game->gen->gaddag->alphabet);
    game->gen->current_anchor_col = 9;
    set_row(game, 4, "   THERMOS  A");
    game->gen->current_row_index = 4;
    uint8_t ml = val(game->gen->gaddag->alphabet, 'I');
    clear_cross_set(game->gen->board, game->gen->current_row_index, 2, BOARD_VERTICAL_DIRECTION);
    set_cross_set_letter(get_cross_set_pointer(game->gen->board, game->gen->current_row_index, 2, BOARD_VERTICAL_DIRECTION), ml);
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);
    // it should generate HITHERMOST only
    assert(game->gen->move_list->count == 1);
    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "5B HI(THERMOS)T 36"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestRowGen
    load_cgp(game, VS_ED);
    set_rack_to_string(rack, "AAEIRST", game->gen->gaddag->alphabet);
    game->gen->current_row_index = 4;
    game->gen->current_anchor_col = 8;
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);

    assert(game->gen->move_list->count == 2);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "5B AIR(GLOWS) 12"));
    reset_string(test_string);
    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[1], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "5C RE(GLOWS) 11"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestOtherRowGen
    load_cgp(game, VS_MATT);
    set_rack_to_string(rack, "A", game->gen->gaddag->alphabet);
    game->gen->current_row_index = 14;
    game->gen->current_anchor_col = 8;
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);
    assert(game->gen->move_list->count == 1);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "15C A(VENGED) 12"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestOneMoreRowGen
    load_cgp(game, VS_MATT);
    set_rack_to_string(rack, "A", game->gen->gaddag->alphabet);
    game->gen->current_row_index = 0;
    game->gen->current_anchor_col = 11;
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 1);
    assert(game->gen->move_list->count == 1);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "1L (F)A 5"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestGenMoveJustOnce
    load_cgp(game, VS_MATT);
    transpose(game->gen->board);
    set_rack_to_string(rack, "AELT", game->gen->gaddag->alphabet);
    game->gen->current_row_index = 10;
    game->gen->vertical = 1;
    game->gen->last_anchor_col = 100;
    for (int anchor_col = 8; anchor_col < 13; anchor_col++) {
        game->gen->current_anchor_col = anchor_col;
        execute_recursive_gen(game->gen, game->gen->current_anchor_col, rack, 0, game->gen->current_anchor_col, game->gen->current_anchor_col, 0);
        game->gen->last_anchor_col = anchor_col;
    }
    assert(game->gen->move_list->count == 34);

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesSingleTile
    load_cgp(game, VS_MATT);
    set_rack_to_string(rack, "A", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);
    assert(game->gen->move_list->count == 25);

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesFullRack
    load_cgp(game, VS_MATT);
    set_rack_to_string(rack, "AABDELT", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 673);
    assert(count_nonscoring_plays(game->gen->move_list) == 96);

    int highest_scores[] = {38, 36, 36, 34, 34, 33, 30, 30, 30, 28};
    int number_of_highest_scores = sizeof(highest_scores) / sizeof(int);
    for (int i = 0; i < number_of_highest_scores; i++) {
        assert(game->gen->move_list->moves[i]->score == highest_scores[i]);
    }

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesFullRackAgain
    load_cgp(game, VS_ED);
    set_rack_to_string(rack, "AFGIIIS", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 219);
    assert(count_nonscoring_plays(game->gen->move_list) == 64);

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesSingleBlank
    load_cgp(game, VS_ED);
    set_rack_to_string(rack, "?", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 166);
    assert(count_nonscoring_plays(game->gen->move_list) == 2);

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesTwoBlanksOnly
    load_cgp(game, VS_ED);
    set_rack_to_string(rack, "??", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 1958);
    assert(count_nonscoring_plays(game->gen->move_list) == 3);

    reset_game(game);
    reset_rack(rack);

    // TestGenAllMovesWithBlanks
    load_cgp(game, VS_JEREMY);
    set_rack_to_string(rack, "DDESW??", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);
    assert(count_scoring_plays(game->gen->move_list) == 8297);
    assert(count_nonscoring_plays(game->gen->move_list) == 1);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "14B hEaDW(OR)DS 106"));
    reset_string(test_string);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[1], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "14B hEaDW(OR)D 38"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestGiantTwentySevenTimer
    load_cgp(game, VS_OXY);
    set_rack_to_string(rack, "ABEOPXZ", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);
    assert(count_scoring_plays(game->gen->move_list) == 519);
    assert(count_nonscoring_plays(game->gen->move_list) == 1);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    // TestGenerateEmptyBoard
    set_rack_to_string(rack, "DEGORV?", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 3313);
    assert(count_nonscoring_plays(game->gen->move_list) == 128);
    Move * move = game->gen->move_list->moves[0];
    assert(move->score == 80);
    assert(move->tiles_played == 7);
    assert(move->tiles_length == 7);
    assert(move->move_type == MOVE_TYPE_PLAY);
    assert(move->row_start == 7);
    assert(move->col_start == 7);

    reset_game(game);
    reset_rack(rack);

    // TestGenerateNoPlays
    load_cgp(game, VS_JEREMY);
    set_rack_to_string(rack, "V", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);
    assert(count_scoring_plays(game->gen->move_list) == 0);
    assert(count_nonscoring_plays(game->gen->move_list) == 1);
    assert(game->gen->move_list->moves[0]->move_type == MOVE_TYPE_PASS);

    reset_game(game);
    reset_rack(rack);

    // TestRowEquivalent
    load_cgp(game, TEST_DUPE);
    generate_all_cross_sets(game->gen->board, game->gen->gaddag, game->gen->letter_distribution);

    Game * game_two = create_game(config);

    set_row(game_two, 7, " INCITES");
	set_row(game_two, 8, "IS");
	set_row(game_two, 9, "T");
    update_all_anchors(game_two->gen->board);
    generate_all_cross_sets(game_two->gen->board, game_two->gen->gaddag, game_two->gen->letter_distribution);

    boards_equal(game->gen->board, game_two->gen->board);

    reset_game(game);
    reset_game(game_two);
    reset_rack(rack);

    // TestGenExchange
    set_rack_to_string(rack, "ABCDEF?", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_nonscoring_plays(game->gen->move_list) == 128);

    destroy_rack(rack);
    destroy_game(game);
    destroy_game(game_two);
    
}

void exchange_tests(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);
    Game * game = create_game(config);

    char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
    load_cgp(game, cgp);
    // The top equity plays uses 7 tiles,
    // so exchanges should not be possible.
    play_top_n_equity_move(game, 0);
    generate_moves_for_game(game);
    assert(game->gen->move_list->moves[0]->move_type == MOVE_TYPE_PLAY);
    reset_game(game);

    load_cgp(game, cgp);
    // The second top equity play only uses
    // 4 tiles, so exchanges should be the best play.
    play_top_n_equity_move(game, 1);
    generate_moves_for_game(game);
    assert(game->gen->move_list->moves[0]->move_type == MOVE_TYPE_EXCHANGE);

    destroy_game(game);
}

void many_moves_tests(TestConfig * test_config) {
    Config * config = get_csw_config(test_config);
    Game * game = create_game(config);

    load_cgp(game, MANY_MOVES);
    generate_moves_for_game(game);
    assert(count_scoring_plays(game->gen->move_list) == 238895);
    assert(count_nonscoring_plays(game->gen->move_list) == 96);

    destroy_game(game);
}

void equity_test(TestConfig * test_config) {
    Config * config = get_america_config(test_config);

    Game * game = create_game(config);
    set_gen_sorting_parameter(game->gen, SORT_BY_EQUITY);
    Rack * rack = create_rack();
    // A middlegame is chosen to avoid
    // the opening and endgame equity adjustments
    load_cgp(game, VS_ED);
    set_rack_to_string(rack, "AFGIIIS", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 1);
    assert(count_scoring_plays(game->gen->move_list) == 219);
    assert(count_nonscoring_plays(game->gen->move_list) == 64);

    double previous_equity = 1000000.0;
    Rack * move_rack = create_rack();
    int number_of_moves = game->gen->move_list->count;

    for (int i = 0; i < number_of_moves - 1; i++) {
        Move * move = game->gen->move_list->moves[i];
        assert(move->equity <= previous_equity);
        set_rack_to_string(move_rack, "AFGIIIS", game->gen->gaddag->alphabet);
        double leave_value = get_leave_value_for_move(config, move, move_rack);
        assert(within_epsilon(move->equity, (((double)move->score) + leave_value)));
        previous_equity = move->equity;
    }
    assert(game->gen->move_list->moves[number_of_moves - 1]->move_type == MOVE_TYPE_PASS);

    destroy_rack(rack);
    destroy_rack(move_rack);
    destroy_game(game);
    
}

void top_equity_play_recorder_test(TestConfig * test_config) {
    Config * config = get_america_config(test_config);

    Game * game = create_game(config);
    set_gen_play_recorder_type(game->gen, PLAY_RECORDER_TYPE_TOP_EQUITY);
    Rack * rack = create_rack();
    char test_string[100];
    reset_string(test_string);

    load_cgp(game, VS_JEREMY);
    set_rack_to_string(rack, "DDESW??", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "14B hEaDW(OR)DS 106"));
    reset_string(test_string);

    reset_game(game);
    reset_rack(rack);

    load_cgp(game, VS_OXY);
    set_rack_to_string(rack, "ABEOPXZ", game->gen->gaddag->alphabet);
    generate_moves(game->gen, rack, NULL, 0);

    write_user_visible_move_to_end_of_buffer(test_string, game->gen->board, game->gen->move_list->moves[0], game->gen->gaddag->alphabet);
    assert(!strcmp(test_string, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780"));
    reset_string(test_string);

    destroy_rack(rack);
    destroy_game(game);
    
}

void test_movegen(TestConfig * test_config) {
    macondo_tests(test_config);
    exchange_tests(test_config);
    // many_moves_tests(test_config);
    equity_test(test_config);
    top_equity_play_recorder_test(test_config);
}
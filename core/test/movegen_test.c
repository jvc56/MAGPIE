#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/bag.h"
#include "../src/cross_set.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/move.h"
#include "../src/movegen.h"
#include "../src/player.h"
#include "../src/util.h"

#include "cross_set_test.h"
#include "game_print.h"
#include "move_print.h"
#include "rack_test.h"
#include "superconfig.h"
#include "test_constants.h"
#include "test_util.h"

int count_scoring_plays(MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < ml->count; i++) {
    if (ml->moves[i]->move_type == MOVE_TYPE_PLAY) {
      sum++;
    }
  }
  return sum;
}

int count_nonscoring_plays(MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < ml->count; i++) {
    if (ml->moves[i]->move_type != MOVE_TYPE_PLAY) {
      sum++;
    }
  }
  return sum;
}

void boards_equal(Board *b1, Board *b2) {
  assert(b1->tiles_played == b2->tiles_played);
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      assert(get_letter(b1, i, j) == get_letter(b2, i, j));
      assert(get_bonus_square(b1, i, j) == get_bonus_square(b2, i, j));
      assert(get_cross_score(b1, i, j, BOARD_HORIZONTAL_DIRECTION, 0) ==
             get_cross_score(b2, i, j, BOARD_HORIZONTAL_DIRECTION, 0));
      assert(get_cross_score(b1, i, j, BOARD_VERTICAL_DIRECTION, 0) ==
             get_cross_score(b2, i, j, BOARD_VERTICAL_DIRECTION, 0));
      assert(get_cross_set(b1, i, j, BOARD_HORIZONTAL_DIRECTION, 0) ==
             get_cross_set(b2, i, j, BOARD_HORIZONTAL_DIRECTION, 0));
      assert(get_cross_set(b1, i, j, BOARD_VERTICAL_DIRECTION, 0) ==
             get_cross_set(b2, i, j, BOARD_VERTICAL_DIRECTION, 0));
      assert(get_anchor(b1, i, j, 0) == get_anchor(b2, i, j, 0));
      assert(get_anchor(b1, i, j, 1) == get_anchor(b2, i, j, 1));
    }
  }
}

void execute_recursive_gen(Generator *gen, int col, Player *player,
                           int leftstrip, int rightstrip, int unique_play) {
  init_leave_map(gen->leave_map, player->rack);
  load_row_letter_cache(gen, gen->current_row_index);
  recursive_gen(gen, col, player, NULL,
                kwg_get_root_node_index(player->strategy_params->kwg),
                leftstrip, rightstrip, unique_play);
}

void test_simple_case(Game *game, Player *player, const char *rack_string,
                      int current_anchor_col, int row, const char *row_string,
                      int expected_plays) {
  reset_game(game);
  reset_rack(player->rack);
  game->gen->current_anchor_col = current_anchor_col;
  set_rack_to_string(player->rack, rack_string, game->gen->letter_distribution);
  set_row(game, row, row_string);
  game->gen->current_row_index = row;
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  assert(expected_plays == game->gen->move_list->count);
  reset_game(game);
  reset_rack(player->rack);
}

void macondo_tests(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];
  KWG *kwg = player->strategy_params->kwg;
  char test_string[100];
  reset_string(test_string);

  // TestGenBase
  clear_all_crosses(game->gen->board);
  game->gen->current_anchor_col = 0;
  game->gen->current_row_index = 4;

  set_rack_to_string(player->rack, "AEINRST", game->gen->letter_distribution);
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  assert(game->gen->move_list->count == 0);

  // TestSimpleRowGen
  reset_board(game->gen->board);
  test_simple_case(game, player, "P", 11, 2, "     REGNANT", 1);
  test_simple_case(game, player, "O", 9, 2, "  PORTOLAN", 1);
  test_simple_case(game, player, "S", 9, 2, "  PORTOLAN", 1);
  test_simple_case(game, player, "?", 9, 2, "  PORTOLAN", 2);
  test_simple_case(game, player, "TY", 7, 2, "  SOVRAN", 1);
  test_simple_case(game, player, "ING", 6, 2, "  LAUGH", 1);
  test_simple_case(game, player, "ZA", 3, 4, "  BE", 0);
  test_simple_case(game, player, "AENPPSW", 14, 4, "        CHAWING", 1);
  test_simple_case(game, player, "ABEHINT", 9, 4, "   THERMOS  A", 2);
  test_simple_case(game, player, "ABEHITT", 8, 4, "  THERMOS A   ", 1);
  test_simple_case(game, player, "TT", 10, 4, "  THERMOS A   ", 3);
  test_simple_case(game, player, "A", 1, 4, " B", 1);
  test_simple_case(game, player, "A", 1, 4, " b", 1);

  // TestGenThroughBothWaysAllowedLetters
  set_rack_to_string(player->rack, "ABEHINT", game->gen->letter_distribution);
  game->gen->current_anchor_col = 9;
  set_row(game, 4, "   THERMOS  A");
  game->gen->current_row_index = 4;
  uint8_t ml = human_readable_letter_to_machine_letter(
      game->gen->letter_distribution, "I");
  clear_cross_set(game->gen->board, game->gen->current_row_index, 2,
                  BOARD_VERTICAL_DIRECTION, 0);
  set_cross_set_letter(get_cross_set_pointer(game->gen->board,
                                             game->gen->current_row_index, 2,
                                             BOARD_VERTICAL_DIRECTION, 0),
                       ml);
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  // it should generate HITHERMOST only
  assert(game->gen->move_list->count == 1);
  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "5B HI(THERMOS)T 36"));
  reset_string(test_string);

  reset_game(game);
  reset_rack(player->rack);

  // TestRowGen
  load_cgp(game, VS_ED);
  set_rack_to_string(player->rack, "AAEIRST", game->gen->letter_distribution);
  game->gen->current_row_index = 4;
  game->gen->current_anchor_col = 8;
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);

  assert(game->gen->move_list->count == 2);

  SortedMoveList *test_row_gen_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  write_user_visible_move_to_end_of_buffer(
      test_string, game->gen->board, test_row_gen_sorted_move_list->moves[0],
      game->gen->letter_distribution);
  assert(!strcmp(test_string, "5B AIR(GLOWS) 12"));
  reset_string(test_string);
  write_user_visible_move_to_end_of_buffer(
      test_string, game->gen->board, test_row_gen_sorted_move_list->moves[1],
      game->gen->letter_distribution);
  assert(!strcmp(test_string, "5C RE(GLOWS) 11"));
  reset_string(test_string);

  destroy_sorted_move_list(test_row_gen_sorted_move_list);
  reset_game(game);
  reset_rack(player->rack);

  // TestOtherRowGen
  load_cgp(game, VS_MATT);
  set_rack_to_string(player->rack, "A", game->gen->letter_distribution);
  game->gen->current_row_index = 14;
  game->gen->current_anchor_col = 8;
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  assert(game->gen->move_list->count == 1);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "15C A(VENGED) 12"));
  reset_string(test_string);

  reset_game(game);
  reset_rack(player->rack);

  // TestOneMoreRowGen
  load_cgp(game, VS_MATT);
  set_rack_to_string(player->rack, "A", game->gen->letter_distribution);
  game->gen->current_row_index = 0;
  game->gen->current_anchor_col = 11;
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  assert(game->gen->move_list->count == 1);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "1L (F)A 5"));
  reset_string(test_string);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenMoveJustOnce
  load_cgp(game, VS_MATT);
  transpose(game->gen->board);
  set_rack_to_string(player->rack, "AELT", game->gen->letter_distribution);
  game->gen->current_row_index = 10;
  game->gen->vertical = 1;
  game->gen->last_anchor_col = 100;
  for (int anchor_col = 8; anchor_col < 13; anchor_col++) {
    game->gen->current_anchor_col = anchor_col;
    execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                          game->gen->current_anchor_col,
                          game->gen->current_anchor_col, 0);
    game->gen->last_anchor_col = anchor_col;
  }
  assert(game->gen->move_list->count == 34);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesSingleTile
  load_cgp(game, VS_MATT);
  set_rack_to_string(player->rack, "A", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);
  assert(game->gen->move_list->count == 25);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesFullRack
  load_cgp(game, VS_MATT);
  set_rack_to_string(player->rack, "AABDELT", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 667);
  assert(count_nonscoring_plays(game->gen->move_list) == 96);

  SortedMoveList *test_gen_all_moves_full_rack_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  int highest_scores[] = {38, 36, 36, 34, 34, 33, 30, 30, 30, 28};
  int number_of_highest_scores = sizeof(highest_scores) / sizeof(int);
  for (int i = 0; i < number_of_highest_scores; i++) {
    assert(test_gen_all_moves_full_rack_sorted_move_list->moves[i]->score ==
           highest_scores[i]);
  }

  destroy_sorted_move_list(test_gen_all_moves_full_rack_sorted_move_list);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesFullRackAgain
  load_cgp(game, VS_ED);
  set_rack_to_string(player->rack, "AFGIIIS", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 219);
  assert(count_nonscoring_plays(game->gen->move_list) == 64);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesSingleBlank
  load_cgp(game, VS_ED);
  set_rack_to_string(player->rack, "?", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 169);
  assert(count_nonscoring_plays(game->gen->move_list) == 2);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesTwoBlanksOnly
  load_cgp(game, VS_ED);
  set_rack_to_string(player->rack, "??", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 1961);
  assert(count_nonscoring_plays(game->gen->move_list) == 3);

  reset_game(game);
  reset_rack(player->rack);

  // TestGenAllMovesWithBlanks
  load_cgp(game, VS_JEREMY);
  set_rack_to_string(player->rack, "DDESW??", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);
  assert(count_scoring_plays(game->gen->move_list) == 8285);
  assert(count_nonscoring_plays(game->gen->move_list) == 1);

  SortedMoveList *test_gen_all_moves_with_blanks_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  write_user_visible_move_to_end_of_buffer(
      test_string, game->gen->board,
      test_gen_all_moves_with_blanks_sorted_move_list->moves[0],
      game->gen->letter_distribution);
  assert(!strcmp(test_string, "14B hEaDW(OR)DS 106"));
  reset_string(test_string);

  write_user_visible_move_to_end_of_buffer(
      test_string, game->gen->board,
      test_gen_all_moves_with_blanks_sorted_move_list->moves[1],
      game->gen->letter_distribution);
  assert(!strcmp(test_string, "14B hEaDW(OR)D 38"));
  reset_string(test_string);

  destroy_sorted_move_list(test_gen_all_moves_with_blanks_sorted_move_list);

  reset_game(game);
  reset_rack(player->rack);

  // TestGiantTwentySevenTimer
  load_cgp(game, VS_OXY);
  set_rack_to_string(player->rack, "ABEOPXZ", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);
  assert(count_scoring_plays(game->gen->move_list) == 513);
  assert(count_nonscoring_plays(game->gen->move_list) == 1);

  SortedMoveList *test_giant_twenty_seven_timer_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  write_user_visible_move_to_end_of_buffer(
      test_string, game->gen->board,
      test_giant_twenty_seven_timer_sorted_move_list->moves[0],
      game->gen->letter_distribution);
  assert(!strcmp(test_string, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780"));
  reset_string(test_string);

  destroy_sorted_move_list(test_giant_twenty_seven_timer_sorted_move_list);
  reset_game(game);
  reset_rack(player->rack);

  // TestGenerateEmptyBoard
  set_rack_to_string(player->rack, "DEGORV?", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 3307);
  assert(count_nonscoring_plays(game->gen->move_list) == 128);

  SortedMoveList *test_generate_empty_board_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  Move *move = test_generate_empty_board_sorted_move_list->moves[0];
  assert(move->score == 80);
  assert(move->tiles_played == 7);
  assert(move->tiles_length == 7);
  assert(move->move_type == MOVE_TYPE_PLAY);
  assert(move->row_start == 7);

  destroy_sorted_move_list(test_generate_empty_board_sorted_move_list);
  reset_game(game);
  reset_rack(player->rack);

  // TestGenerateNoPlays
  load_cgp(game, VS_JEREMY);
  set_rack_to_string(player->rack, "V", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);
  assert(count_scoring_plays(game->gen->move_list) == 0);
  assert(count_nonscoring_plays(game->gen->move_list) == 1);
  assert(game->gen->move_list->moves[0]->move_type == MOVE_TYPE_PASS);

  reset_game(game);
  reset_rack(player->rack);

  // TestRowEquivalent
  load_cgp(game, TEST_DUPE);
  generate_all_cross_sets(game->gen->board, kwg, kwg,
                          game->gen->letter_distribution, 0);

  Game *game_two = create_game(config);

  set_row(game_two, 7, " INCITES");
  set_row(game_two, 8, "IS");
  set_row(game_two, 9, "T");
  update_all_anchors(game_two->gen->board);
  generate_all_cross_sets(game_two->gen->board, kwg, kwg,
                          game_two->gen->letter_distribution, 0);

  boards_equal(game->gen->board, game_two->gen->board);

  reset_game(game);
  reset_game(game_two);
  reset_rack(player->rack);

  // TestGenExchange
  set_rack_to_string(player->rack, "ABCDEF?", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_nonscoring_plays(game->gen->move_list) == 128);

  destroy_game(game);
  destroy_game(game_two);
}

void exchange_tests(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);
  // The top equity plays uses 7 tiles,
  // so exchanges should not be possible.
  play_top_n_equity_move(game, 0);

  generate_moves_for_game(game);
  SortedMoveList *test_not_an_exchange_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);
  assert(test_not_an_exchange_sorted_move_list->moves[0]->move_type ==
         MOVE_TYPE_PLAY);
  destroy_sorted_move_list(test_not_an_exchange_sorted_move_list);
  reset_game(game);

  load_cgp(game, cgp);
  // The second top equity play only uses
  // 4 tiles, so exchanges should be the best play.
  play_top_n_equity_move(game, 1);
  generate_moves_for_game(game);
  SortedMoveList *test_exchange_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  assert(test_exchange_sorted_move_list->moves[0]->move_type ==
         MOVE_TYPE_EXCHANGE);
  destroy_sorted_move_list(test_exchange_sorted_move_list);

  destroy_game(game);
}

void many_moves_tests(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);

  load_cgp(game, MANY_MOVES);
  generate_moves_for_game(game);
  assert(count_scoring_plays(game->gen->move_list) == 238895);
  assert(count_nonscoring_plays(game->gen->move_list) == 96);

  destroy_game(game);
}

void equity_test(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);

  Game *game = create_game(config);
  Player *player = game->players[0];
  player->strategy_params->move_sorting = SORT_BY_EQUITY;
  KLV *klv = player->strategy_params->klv;
  // A middlegame is chosen to avoid
  // the opening and endgame equity adjustments
  load_cgp(game, VS_ED);
  set_rack_to_string(player->rack, "AFGIIIS", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 1);
  assert(count_scoring_plays(game->gen->move_list) == 219);
  assert(count_nonscoring_plays(game->gen->move_list) == 64);

  SortedMoveList *equity_test_sorted_move_list =
      create_sorted_move_list(game->gen->move_list);

  double previous_equity = 1000000.0;
  Rack *move_rack = create_rack(config->letter_distribution->size);
  int number_of_moves = equity_test_sorted_move_list->count;

  for (int i = 0; i < number_of_moves - 1; i++) {
    Move *move = equity_test_sorted_move_list->moves[i];
    assert(move->equity <= previous_equity);
    set_rack_to_string(move_rack, "AFGIIIS", game->gen->letter_distribution);
    double leave_value = get_leave_value_for_move(klv, move, move_rack);
    assert(within_epsilon(move->equity, (((double)move->score) + leave_value)));
    previous_equity = move->equity;
  }
  assert(equity_test_sorted_move_list->moves[number_of_moves - 1]->move_type ==
         MOVE_TYPE_PASS);

  destroy_sorted_move_list(equity_test_sorted_move_list);
  destroy_rack(move_rack);
  destroy_game(game);
}

void top_equity_play_recorder_test(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);

  Game *game = create_game(config);
  Player *player = game->players[0];
  int saved_recorder_type = player->strategy_params->play_recorder_type;
  player->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
  char test_string[100];
  reset_string(test_string);

  load_cgp(game, VS_JEREMY);
  set_rack_to_string(player->rack, "DDESW??", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "14B hEaDW(OR)DS 106"));
  reset_string(test_string);

  reset_game(game);
  reset_rack(player->rack);

  load_cgp(game, VS_OXY);
  set_rack_to_string(player->rack, "ABEOPXZ", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780"));
  reset_string(test_string);
  // reset play recorder type as this is a shared config.
  player->strategy_params->play_recorder_type = saved_recorder_type;

  destroy_game(game);
}

void distinct_lexica_test(SuperConfig *superconfig) {
  Config *config = get_distinct_lexica_config(superconfig);

  Game *game = create_game(config);
  int player_1_saved_recorder_type =
      game->players[0]->strategy_params->play_recorder_type;
  int player_2_saved_recorder_type =
      game->players[1]->strategy_params->play_recorder_type;
  game->players[0]->strategy_params->play_recorder_type =
      PLAY_RECORDER_TYPE_TOP_EQUITY;
  game->players[1]->strategy_params->play_recorder_type =
      PLAY_RECORDER_TYPE_TOP_EQUITY;

  char test_string[100];
  reset_string(test_string);

  // Play SPORK, better than best NWL move of PORKS
  set_rack_to_string(game->players[0]->rack, "KOPRRSS",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);
  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "8H SPORK 32"));
  reset_string(test_string);

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  set_rack_to_string(game->players[1]->rack, "CEHIIRZ",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "H8 (S)CHIZIER 146"));
  reset_string(test_string);

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  set_rack_to_string(game->players[0]->rack, "GGLLOWY",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "11G W(I)GGLY 28"));
  reset_string(test_string);

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  set_rack_to_string(game->players[1]->rack, "AEEQRSU",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  write_user_visible_move_to_end_of_buffer(test_string, game->gen->board,
                                           game->gen->move_list->moves[0],
                                           game->gen->letter_distribution);
  assert(!strcmp(test_string, "13C QUEAS(I)ER 88"));
  reset_string(test_string);

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  game->players[0]->strategy_params->play_recorder_type =
      player_1_saved_recorder_type;

  game->players[1]->strategy_params->play_recorder_type =
      player_2_saved_recorder_type;

  destroy_game(game);
}

void test_movegen(SuperConfig *superconfig) {
  macondo_tests(superconfig);
  exchange_tests(superconfig);
  equity_test(superconfig);
  top_equity_play_recorder_test(superconfig);
  distinct_lexica_test(superconfig);
}
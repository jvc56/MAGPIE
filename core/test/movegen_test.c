#include "../src/movegen.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/bag.h"
#include "../src/cross_set.h"
#include "../src/game.h"
#include "../src/gameplay.h"
#include "../src/move.h"
#include "../src/player.h"
#include "../src/util.h"
#include "cross_set_test.h"
#include "rack_test.h"
#include "superconfig.h"
#include "test_constants.h"
#include "test_util.h"

int count_scoring_plays(MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < ml->count; i++) {
    if (ml->moves[i]->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      sum++;
    }
  }
  return sum;
}

int count_nonscoring_plays(MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < ml->count; i++) {
    if (ml->moves[i]->move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
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
  memset(game->gen->highest_equity_by_length, 100000.0,
         sizeof(double) * (RACK_SIZE + 1));
  game->gen->min_num_playthrough = 0;
  game->gen->max_num_playthrough = BOARD_DIM - 1;
  game->gen->max_tiles_to_play = RACK_SIZE;
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
  set_cross_set_letter(
      get_cross_set_pointer(game->gen->board, game->gen->current_row_index, 2,
                            BOARD_VERTICAL_DIRECTION, 0),
      ml);
  execute_recursive_gen(game->gen, game->gen->current_anchor_col, player,
                        game->gen->current_anchor_col,
                        game->gen->current_anchor_col, 1);
  // it should generate HITHERMOST only
  assert(game->gen->move_list->count == 1);
  assert_move(game, NULL, 0, "5B HI(THERMOS)T 36");

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

  assert_move(game, test_row_gen_sorted_move_list, 0, "5B AIR(GLOWS) 12");
  assert_move(game, test_row_gen_sorted_move_list, 1, "5C RE(GLOWS) 11");

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
  assert_move(game, NULL, 0, "15C A(VENGED) 12");

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
  assert_move(game, NULL, 0, "1L (F)A 5");

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

  assert_move(game, test_gen_all_moves_with_blanks_sorted_move_list, 0,
              "14B hEaDW(OR)DS 106");
  assert_move(game, test_gen_all_moves_with_blanks_sorted_move_list, 1,
              "14B hEaDW(OR)D 38");

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

  assert_move(game, test_giant_twenty_seven_timer_sorted_move_list, 0,
              "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

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
  assert(move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE);
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
  assert(game->gen->move_list->moves[0]->move_type == GAME_EVENT_PASS);

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

// print assertions to paste into leave_lookup_test
void print_leave_lookup_test(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  double *leaves = game->gen->leave_map->leave_values;
  // Initialize data to notice which elements are not set.
  for (int i = 0; i < (1 << RACK_SIZE); i++) {
    leaves[i] = DBL_MAX;
  }
  generate_leaves_for_game(game, true);
  const char rack[8] = "MOOORTT";
  for (int i = 0; i < (1 << RACK_SIZE); ++i) {
    const double value = leaves[i];
    if (value == DBL_MAX) {
      continue;
    }
    char leave_string[8];
    int k = 0;
    for (int j = 0; j < RACK_SIZE; ++j) {
      if (i & (1 << j)) {
        leave_string[k++] = rack[j];
      }
      leave_string[k] = '\0';
    }
    printf("assert(within_epsilon(leaves[%3d], %+14.6f));", i, value);
    printf(" // %s\n", leave_string);
  }
  destroy_game(game);
}

void leave_lookup_test(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  for (int i = 0; i < 2; i++) {
    int add_exchanges = i == 0;
    generate_leaves_for_game(game, add_exchanges);
    double *leaves = game->gen->leave_map->leave_values;
    assert(within_epsilon(leaves[0], +0.000000));         //
    assert(within_epsilon(leaves[1], -0.079110));         // M
    assert(within_epsilon(leaves[2], -1.092266));         // O
    assert(within_epsilon(leaves[3], +0.427566));         // MO
    assert(within_epsilon(leaves[6], -8.156165));         // OO
    assert(within_epsilon(leaves[7], -4.466051));         // MOO
    assert(within_epsilon(leaves[14], -18.868383));       // OOO
    assert(within_epsilon(leaves[15], -14.565833));       // MOOO
    assert(within_epsilon(leaves[16], +1.924450));        // R
    assert(within_epsilon(leaves[17], +0.965204));        // MR
    assert(within_epsilon(leaves[18], +1.631953));        // OR
    assert(within_epsilon(leaves[19], +2.601703));        // MOR
    assert(within_epsilon(leaves[22], -5.642596));        // OOR
    assert(within_epsilon(leaves[23], -1.488737));        // MOOR
    assert(within_epsilon(leaves[30], -17.137913));       // OOOR
    assert(within_epsilon(leaves[31], -12.899072));       // MOOOR
    assert(within_epsilon(leaves[48], -5.277321));        // RT
    assert(within_epsilon(leaves[49], -7.450112));        // MRT
    assert(within_epsilon(leaves[50], -4.813058));        // ORT
    assert(within_epsilon(leaves[51], -4.582363));        // MORT
    assert(within_epsilon(leaves[54], -11.206508));       // OORT
    assert(within_epsilon(leaves[55], -7.305244));        // MOORT
    assert(within_epsilon(leaves[62], -21.169294));       // OOORT
    assert(within_epsilon(leaves[63], -16.637489));       // MOOORT
    assert(within_epsilon(leaves[64], +0.878783));        // T
    assert(within_epsilon(leaves[65], -0.536439));        // MT
    assert(within_epsilon(leaves[66], +0.461634));        // OT
    assert(within_epsilon(leaves[67], +0.634061));        // MOT
    assert(within_epsilon(leaves[70], -6.678402));        // OOT
    assert(within_epsilon(leaves[71], -3.665847));        // MOOT
    assert(within_epsilon(leaves[78], -18.284534));       // OOOT
    assert(within_epsilon(leaves[79], -14.382346));       // MOOOT
    assert(within_epsilon(leaves[80], +2.934475));        // RT
    assert(within_epsilon(leaves[81], +0.090591));        // MRT
    assert(within_epsilon(leaves[82], +3.786163));        // ORT
    assert(within_epsilon(leaves[83], +2.442589));        // MORT
    assert(within_epsilon(leaves[86], -3.260469));        // OORT
    assert(within_epsilon(leaves[87], -0.355031));        // MOORT
    assert(within_epsilon(leaves[94], -15.671781));       // OOORT
    assert(within_epsilon(leaves[95], -12.082211));       // MOOORT
    assert(within_epsilon(leaves[112], -5.691820));       // RTT
    assert(within_epsilon(leaves[113], -10.848881));      // MRTT
    assert(within_epsilon(leaves[114], -3.967470));       // ORTT
    assert(within_epsilon(leaves[115], -7.316442));       // MORTT
    assert(within_epsilon(leaves[118], -9.621570));       // OORTT
    assert(within_epsilon(leaves[119], -8.197909));       // MOORTT
    assert(within_epsilon(leaves[126], -18.412781));      // OOORTT
    assert(within_epsilon(leaves[127], -100000.000000));  // MOOORTT
  }
  destroy_game(game);
}

void exchange_tests(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);

  char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
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
         GAME_EVENT_TILE_PLACEMENT_MOVE);
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
         GAME_EVENT_EXCHANGE);
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
  player->strategy_params->move_sorting = MOVE_SORT_EQUITY;
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
         GAME_EVENT_PASS);

  destroy_sorted_move_list(equity_test_sorted_move_list);
  destroy_rack(move_rack);
  destroy_game(game);
}

void set_up_gen_for_anchor(Generator *gen, int i) {
  reset_move_list(gen->move_list);
  gen->tiles_played = 0;
  gen->current_anchor_col = gen->anchor_list->anchors[i]->col;
  gen->current_row_index = gen->anchor_list->anchors[i]->row;
  gen->last_anchor_col = gen->anchor_list->anchors[i]->last_anchor_col;
  gen->vertical = gen->anchor_list->anchors[i]->vertical;
  gen->min_num_playthrough = gen->anchor_list->anchors[i]->min_num_playthrough;
  gen->max_num_playthrough = gen->anchor_list->anchors[i]->max_num_playthrough;
  gen->min_tiles_to_play = gen->anchor_list->anchors[i]->min_tiles_to_play;
  gen->max_tiles_to_play = gen->anchor_list->anchors[i]->max_tiles_to_play;
  gen->highest_shadow_equity =
      gen->anchor_list->anchors[i]->highest_possible_equity;
  memcpy(gen->max_tiles_starting_left_by,
         gen->anchor_list->anchors[i]->max_tiles_starting_left_by,
         sizeof(gen->max_tiles_starting_left_by));
  memcpy(gen->highest_equity_by_length,
         gen->anchor_list->anchors[i]->highest_equity_by_length,
         sizeof(gen->highest_equity_by_length));
  for (int len = gen->max_tiles_to_play; len > 0; len--) {
    for (int shorter_len = len - 1; shorter_len >= 0; shorter_len--) {
      if (gen->highest_equity_by_length[shorter_len] <
          gen->highest_equity_by_length[len]) {
        gen->highest_equity_by_length[shorter_len] =
            gen->highest_equity_by_length[len];
      }
    }
  }
  set_transpose(gen->board, gen->anchor_list->anchors[i]->transpose_state);
  load_row_letter_cache(gen, gen->current_row_index);
}

void bingo_anchor_tests(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Generator *gen = game->gen;
  AnchorList *al = gen->anchor_list;
  Player *player = game->players[game->player_on_turn_index];
  Rack *opp_rack = game->players[1 - game->player_on_turn_index]->rack;

  char kaki_yond[300] =
      "15/15/15/15/15/15/15/6KAkI5/8YOND3/15/15/15/15/15/15 MIRITIS/EEEEEEE "
      "14/22 0 lex CSW21";
  assert(load_cgp(game, kaki_yond) == CGP_PARSE_STATUS_SUCCESS);
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 1);
  assert_bingo_found(gen, "MIRITIS");
  gen->current_row_index = 8;
  gen->current_anchor_col = 6;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  gen->vertical = false;
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = (i == 6) ? 7 : 0;
  }
  bingo_gen(gen, player, opp_rack);
  // don't make 9b MIRITIS(YOND)
  assert(gen->move_list->count == 0);

  reset_game(game);
  char playthrough[300] =
      "15/15/15/15/15/11I3/7S3R3/3QUACK3E3/7YAULD3/15/15/15/15/15/15 "
      "AENORST/WEEWEED 70/25 0 lex CSW21";
  assert(load_cgp(game, playthrough) == CGP_PARSE_STATUS_SUCCESS);
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 4);
  player = game->players[game->player_on_turn_index];
  opp_rack = game->players[1 - game->player_on_turn_index]->rack;

  reset_anchor_list(al);
  gen->current_row_index = 5;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  set_descending_tile_scores(gen, player);
  load_row_letter_cache(gen, gen->current_row_index);
  shadow_play_for_anchor(gen, 7, player, opp_rack);
  split_anchors_for_bingos(al, true);
  assert(al->count == 3);
  sort_anchor_list(al);

  assert(al->anchors[0]->highest_possible_equity == 73);
  set_up_gen_for_anchor(gen, 0);
  init_leave_map(gen->leave_map, player->rack);
  recursive_gen(gen, 7, player, opp_rack,
                kwg_get_root_node_index(player->strategy_params->kwg), 7, 7,
                !gen->vertical);
  assert(gen->move_list->count == 2);
  SortedMoveList *sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "6E ANOESTR(I) 73");
  assert_move(game, sorted_move_list, 1, "6G SENOR(I)TA 73");
  destroy_sorted_move_list(sorted_move_list);

  assert(al->anchors[1]->highest_possible_equity == 72);
  set_up_gen_for_anchor(gen, 1);
  bingo_gen(gen, player, opp_rack);
  assert(gen->move_list->count == 2);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "6D ATONERS 72");
  assert_move(game, sorted_move_list, 1, "6D SANTERO 72");
  destroy_sorted_move_list(sorted_move_list);

  assert(al->anchors[2]->highest_possible_equity == 22);
  set_up_gen_for_anchor(gen, 2);
  init_leave_map(gen->leave_map, player->rack);
  recursive_gen(gen, 7, player, opp_rack,
                kwg_get_root_node_index(player->strategy_params->kwg), 7, 7,
                !gen->vertical);
  assert(gen->move_list->count == 168);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "6F ANESTR(I) 22");
  destroy_sorted_move_list(sorted_move_list);

  reset_game(game);
  char e_t[300] =
      "15/15/15/15/10E4/15/15/15/15/15/15/9T5/15/15/15 AAELNRT/ADJNOPS 311/106 "
      "0 lex CSW21";
  assert(load_cgp(game, e_t) == CGP_PARSE_STATUS_SUCCESS);
  player = game->players[game->player_on_turn_index];
  opp_rack = game->players[1 - game->player_on_turn_index]->rack;
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 0);

  StringBuilder *sb = create_string_builder();
  string_builder_add_game(game, sb);
  printf("%s\n", string_builder_peek(sb));
  destroy_string_builder(sb);
  
  reset_anchor_list(al);
  transpose(gen->board);

  gen->vertical = true;
  gen->current_row_index = 9;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  set_descending_tile_scores(gen, player);
  load_row_letter_cache(gen, gen->current_row_index);
  shadow_play_for_anchor(gen, 4, player, opp_rack);
  split_anchors_for_bingos(al, true);
  assert(al->count == 3);
  sort_anchor_list(al);

  assert(al->anchors[0]->highest_possible_equity == 64);
  assert(al->anchors[0]->min_tiles_to_play == 7);
  assert(al->anchors[0]->max_num_playthrough == 1);
  set_up_gen_for_anchor(gen, 0);
  init_leave_map(gen->leave_map, player->rack);
  recursive_gen(gen, 4, player, opp_rack,
                kwg_get_root_node_index(player->strategy_params->kwg), 4, 4,
                !gen->vertical);
  assert(gen->move_list->count == 2);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "J5 ALTERAN(T) 64");
  assert_move(game, sorted_move_list, 1, "J5 ALTERNA(T) 64");

  assert(al->anchors[1]->highest_possible_equity == 63);
  assert(al->anchors[1]->min_tiles_to_play == 7);
  assert(al->anchors[1]->max_num_playthrough == 0);
  set_up_gen_for_anchor(gen, 1);
  bingo_gen(gen, player, opp_rack);
  assert(gen->move_list->count == 0);

  assert(al->anchors[2]->highest_possible_equity == 12);
  set_up_gen_for_anchor(gen, 2);
  init_leave_map(gen->leave_map, player->rack);
  recursive_gen(gen, 4, player, opp_rack,
                kwg_get_root_node_index(player->strategy_params->kwg), 4, 4,
                !gen->vertical);
  assert(gen->move_list->count == 472);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "J1 ALTERN 12");
  destroy_sorted_move_list(sorted_move_list);

  destroy_game(game);
}

void bingo_gen_tests(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Generator *gen = game->gen;
  char word_square[300] =
      "15/15/15/15/15/15/15/3JEWELER5/3ELAPINE5/3WAXINGS5/15/"
      "3LINEMAN5/3ENGRAFT5/3RESENTS5/15 EEEIMPR/BDIORTU 339/169 0 lex CSW21";
  assert(load_cgp(game, word_square) == CGP_PARSE_STATUS_SUCCESS);
  Player *player = game->players[game->player_on_turn_index];

  // Seven-tile overlap, tests cross-checking and scoring
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 2);
  assert_bingo_found(gen, "EPIMERE");
  assert_bingo_found(gen, "PREEMIE");
  gen->current_row_index = 10;
  gen->current_anchor_col = 3;
  gen->vertical = false;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = (i <= 3) ? 7 : 0;
  }
  player->strategy_params->play_recorder_type = MOVE_RECORDER_ALL;
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 1);
  assert_move(game, NULL, 0, "11D EPIMERE 163");

  // Tests double-blank with blanks designated as same letter
  set_rack_to_string(player->rack, "??EIMPR", gen->letter_distribution);
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 108);
  reset_move_list(gen->move_list);
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 3);
  SortedMoveList *sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "11D EPIMeRe 157");
  assert_move(game, sorted_move_list, 1, "11D ePIMERe 157");
  assert_move(game, sorted_move_list, 2, "11D ePIMeRE 157");
  destroy_sorted_move_list(sorted_move_list);

  // Tests double-blank with blanks designated as two different letters
  set_rack_to_string(player->rack, "??EEIMR", gen->letter_distribution);
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 103);
  reset_move_list(gen->move_list);
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 3);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "11D EpIMERe 148");
  assert_move(game, sorted_move_list, 1, "11D EpIMeRE 148");
  assert_move(game, sorted_move_list, 2, "11D epIMERE 148");
  destroy_sorted_move_list(sorted_move_list);

  // Tests single-blank, non-overlapping tiles, last_anchor_col is set.
  set_rack_to_string(player->rack, "?EEIMRT", gen->letter_distribution);
  look_up_bingos_for_game(game);
  reset_move_list(gen->move_list);
  gen->current_anchor_col = 7;
  gen->last_anchor_col = 6;
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = (i == 0) ? 7 : 0;
  }
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 3);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "11H EREMITe 93");
  assert_move(game, sorted_move_list, 1, "11H EReMITE 92");
  assert_move(game, sorted_move_list, 2, "11H eREMITE 92");
  destroy_sorted_move_list(sorted_move_list);

  // Tests single blank set as any of three different letters
  reset_move_list(gen->move_list);
  gen->current_anchor_col = 8;
  gen->last_anchor_col = 7;
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 3);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  assert_move(game, sorted_move_list, 0, "11I REEMITs 84");
  assert_move(game, sorted_move_list, 1, "11I RETIMEd 84");
  assert_move(game, sorted_move_list, 2, "11I RETIMEs 84");
  destroy_sorted_move_list(sorted_move_list);

  // Tests anchor with no bingos
  reset_move_list(gen->move_list);
  gen->current_anchor_col = 9;
  gen->last_anchor_col = 8;
  // I don't know if this type of anchor gets created, seems like it could be
  // avoided, but if it shows up it should not produce any moves.
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = 0;
  }
  bingo_gen(gen, player, NULL);
  assert(gen->move_list->count == 0);

  reset_game(game);
  char outplay[300] =
      "7U6R/7R4MOO/7V4MUT/3J1PEACOAt2O/3ANA1S2T3L/3REX4R2QI/4W3L1E2I1/"
      "4BOWIE1mOGGY/8KOB2O1/5VIDUAL2N1/2AUF4TE2GI/3NEP3I4C/"
      "4HARDIEST2H/9S3ZE/7INTENDED AEFLRST/AEINY 452/368 0 lex CSW21";
  assert(load_cgp(game, outplay) == CGP_PARSE_STATUS_SUCCESS);
  player = game->players[game->player_on_turn_index];
  look_up_bingos_for_game(game);
  reset_move_list(gen->move_list);

  // Tests vertical play, find a bingo starting before current_anchor_col, and
  // endgame equity adjustment.
  assert(gen->number_of_bingos == 1);
  transpose(game->gen->board);
  gen->current_anchor_col = 10;
  gen->current_row_index = 1;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  gen->vertical = true;
  Rack *opp_rack = game->players[1 - game->player_on_turn_index]->rack;
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = ((i >= 2) && (i <= 6)) ? 7 : 0;
  }
  bingo_gen(gen, player, opp_rack);
  assert(gen->move_list->count == 1);
  assert_move(game, NULL, 0, "B9 FALTERS 81");
  assert(gen->move_list->moves[0]->equity == 81 + 2 * 8);

  // Test blanks and opening play with placement adjustment
  reset_game(game);
  assert(load_cgp(game, EMPTY_CGP) == CGP_PARSE_STATUS_SUCCESS);
  set_rack_to_string(player->rack, "??ACEFO", gen->letter_distribution);
  look_up_bingos_for_game(game);
  assert(gen->number_of_bingos == 9);
  gen->current_anchor_col = 7;
  gen->current_row_index = 7;
  // This would be INITIAL_LAST_ANCHOR_COL for a real empty-board anchor.
  // Setting it to 6 gives us just the 8H moves.
  gen->last_anchor_col = 6;
  gen->vertical = false;
  for (int i = 0; i < (BOARD_DIM); i++) {
    gen->max_tiles_starting_left_by[i] = (i == 0) ? 7 : 0;
  }
  bingo_gen(gen, player, NULL);
  // FACEOFF x 3
  // AFFORCE x 2
  // ARCHFOE
  // DOGFACE
  // FACONNE
  // FORECAR
  // GEOFACT
  // OUTFACE
  // PROFACE
  assert(gen->move_list->count == 12);
  sorted_move_list = create_sorted_move_list(gen->move_list);
  // When scores are equal, consonant is preferred to vowel in 8I square.
  assert_move(game, sorted_move_list, 0, "8H ArChFOE 78");
  assert_move(game, sorted_move_list, 1, "8H FOrECAr 76");
  assert_move(game, sorted_move_list, 2, "8H prOFACE 72");
  assert_move(game, sorted_move_list, 3, "8H FACEOff 72");
  assert_move(game, sorted_move_list, 4, "8H OutFACE 72");
  assert_move(game, sorted_move_list, 5, "8H dOgFACE 72");
  assert_move(game, sorted_move_list, 6, "8H fACEOFf 72");
  assert_move(game, sorted_move_list, 7, "8H fACEOfF 72");
  assert_move(game, sorted_move_list, 8, "8H gEOFACt 72");
  assert_move(game, sorted_move_list, 9, "8H AFfOrCE 70");
  assert_move(game, sorted_move_list, 10, "8H AfFOrCE 70");
  assert_move(game, sorted_move_list, 11, "8H FACOnnE 70");

  destroy_sorted_move_list(sorted_move_list);
  destroy_game(game);
}

void top_equity_play_recorder_test(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);

  Game *game = create_game(config);
  Player *player = game->players[0];
  int saved_recorder_type = player->strategy_params->play_recorder_type;
  player->strategy_params->play_recorder_type = MOVE_RECORDER_BEST;

  load_cgp(game, VS_JEREMY);
  set_rack_to_string(player->rack, "DDESW??", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);

  assert_move(game, NULL, 0, "14B hEaDW(OR)DS 106");

  reset_game(game);
  reset_rack(player->rack);

  load_cgp(game, VS_OXY);
  set_rack_to_string(player->rack, "ABEOPXZ", game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, 0);

  assert_move(game, NULL, 0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

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
  game->players[0]->strategy_params->play_recorder_type = MOVE_RECORDER_BEST;
  game->players[1]->strategy_params->play_recorder_type = MOVE_RECORDER_BEST;

  // Play SPORK, better than best NWL move of PORKS
  set_rack_to_string(game->players[0]->rack, "KOPRRSS",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);
  assert_move(game, NULL, 0, "8H SPORK 32");

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  set_rack_to_string(game->players[1]->rack, "CEHIIRZ",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "H8 (S)CHIZIER 146");

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  set_rack_to_string(game->players[0]->rack, "GGLLOWY",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "11G W(I)GGLY 28");

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  set_rack_to_string(game->players[1]->rack, "AEEQRSU",
                     game->gen->letter_distribution);
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "13C QUEAS(I)ER 88");

  play_move(game, game->gen->move_list->moves[0]);
  reset_move_list(game->gen->move_list);

  game->players[0]->strategy_params->play_recorder_type =
      player_1_saved_recorder_type;

  game->players[1]->strategy_params->play_recorder_type =
      player_2_saved_recorder_type;

  destroy_game(game);
}

void test_movegen(SuperConfig *superconfig) {
  bingo_anchor_tests(superconfig); return;
  macondo_tests(superconfig);
  exchange_tests(superconfig);
  bingo_anchor_tests(superconfig);
  bingo_gen_tests(superconfig);
  leave_lookup_test(superconfig);
  equity_test(superconfig);
  top_equity_play_recorder_test(superconfig);
  distinct_lexica_test(superconfig);
}
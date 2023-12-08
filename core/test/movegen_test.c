#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

#include "../src/def/rack_defs.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/move.h"
#include "../src/ent/movegen.h"
#include "../src/ent/player.h"
#include "../src/impl/cross_set.h"
#include "../src/impl/gameplay.h"
#include "../src/util/util.h"

#include "cross_set_test.h"
#include "rack_test.h"
#include "test_constants.h"
#include "test_util.h"
#include "testconfig.h"

int count_scoring_plays(const MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < move_list_get_count(ml); i++) {
    if (get_move_type(move_list_get_move(ml, i)) ==
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      sum++;
    }
  }
  return sum;
}

int count_nonscoring_plays(const MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < move_list_get_count(ml); i++) {
    if (get_move_type(move_list_get_move(ml, i)) !=
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      sum++;
    }
  }
  return sum;
}

void execute_recursive_gen(Generator *gen, int col, Player *player,
                           int leftstrip, int rightstrip, bool unique_play) {
  gen_set_move_sort_type(gen, player_get_move_sort_type(player));
  gen_set_move_record_type(gen, player_get_move_record_type(player));
  gen_set_apply_placement_adjustment(gen, true);

  init_leave_map(player_get_rack(player), gen_get_leave_map(gen));
  load_row_letter_cache(gen, gen_get_current_row_index(gen));
  recursive_gen(NULL, gen, col, player,
                kwg_get_root_node_index(player_get_kwg(player)), leftstrip,
                rightstrip, unique_play);
}

void generate_moves_for_movegen(const Rack *opp_rack, Generator *gen,
                                Player *player, bool add_exchange) {
  generate_moves(opp_rack, gen, player, add_exchange,
                 player_get_move_record_type(player),
                 player_get_move_sort_type(player), true);
}

void test_simple_case(Game *game, Player *player, const char *rack_string,
                      int current_anchor_col, int row, const char *row_string,
                      int expected_plays) {
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  MoveList *move_list = gen_get_move_list(gen);

  reset_game(game);
  reset_rack(player_get_rack(player));
  gen_set_current_anchor_col(gen, current_anchor_col);
  set_rack_to_string(ld, player_get_rack(player), rack_string);
  set_row(game, row, row_string);
  gen_set_current_anchor_col(gen, row);
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);
  assert(expected_plays == move_list_get_count(move_list));
  reset_game(game);
  reset_rack(player_get_rack(player));
}

void macondo_tests(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  Board *board = gen_get_board(gen);
  LetterDistribution *ld = gen_get_ld(gen);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = gen_get_move_list(gen);
  const KWG *kwg = player_get_kwg(player);

  // TestGenBase
  clear_all_crosses(board);
  gen_set_current_anchor_col(gen, 0);
  gen_set_current_row_index(gen, 4);

  set_rack_to_string(ld, player_get_rack(player), "AEINRST");
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);
  assert(move_list_get_count(move_list) == 0);

  // TestSimpleRowGen
  reset_board(board);
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
  set_rack_to_string(ld, player_get_rack(player), "ABEHINT");
  gen_set_current_anchor_col(gen, 9);
  set_row(game, 4, "   THERMOS  A");
  gen_set_current_row_index(gen, 4);
  uint8_t ml = hl_to_ml(ld, "I");
  clear_cross_set(board, gen_get_current_row_index(gen), 2,
                  BOARD_VERTICAL_DIRECTION, 0);
  set_cross_set_letter(get_cross_set_pointer(board,
                                             gen_get_current_row_index(gen), 2,
                                             BOARD_VERTICAL_DIRECTION, 0),
                       ml);
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);
  // it should generate HITHERMOST only
  assert(move_list_get_count(move_list) == 1);
  assert_move(game, NULL, 0, "5B HI(THERMOS)T 36");

  reset_rack(player_get_rack(player));

  // TestRowGen
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "AAEIRST");
  gen_set_current_row_index(gen, 4);
  gen_set_current_anchor_col(gen, 8);
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);

  assert(move_list_get_count(move_list) == 2);

  SortedMoveList *test_row_gen_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, test_row_gen_sorted_move_list, 0, "5B AIR(GLOWS) 12");
  assert_move(game, test_row_gen_sorted_move_list, 1, "5C RE(GLOWS) 11");

  destroy_sorted_move_list(test_row_gen_sorted_move_list);
  reset_rack(player_get_rack(player));

  // TestOtherRowGen
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "A");
  gen_set_current_row_index(gen, 14);
  gen_set_current_anchor_col(gen, 8);
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);
  assert(move_list_get_count(move_list) == 1);
  assert_move(game, NULL, 0, "15C A(VENGED) 12");

  reset_rack(player_get_rack(player));

  // TestOneMoreRowGen
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "A");
  gen_set_current_row_index(gen, 0);
  gen_set_current_anchor_col(gen, 11);
  execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                        gen_get_current_anchor_col(gen),
                        gen_get_current_anchor_col(gen), 1);
  assert(move_list_get_count(move_list) == 1);
  assert_move(game, NULL, 0, "1L (F)A 5");

  reset_rack(player_get_rack(player));

  // TestGenMoveJustOnce
  load_cgp(game, VS_MATT);
  transpose(board);
  set_rack_to_string(ld, player_get_rack(player), "AELT");
  gen_set_current_row_index(gen, 10);
  gen_set_dir(gen, BOARD_VERTICAL_DIRECTION);
  gen_set_last_anchor_col(gen, 100);
  for (int anchor_col = 8; anchor_col < 13; anchor_col++) {
    gen_set_current_anchor_col(gen, anchor_col);
    execute_recursive_gen(gen, gen_get_current_anchor_col(gen), player,
                          gen_get_current_anchor_col(gen),
                          gen_get_current_anchor_col(gen), 0);
    gen_set_last_anchor_col(gen, anchor_col);
  }
  assert(move_list_get_count(move_list) == 34);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesSingleTile
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "A");
  generate_moves_for_movegen(NULL, gen, player, 0);
  assert(move_list_get_count(move_list) == 25);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesFullRack
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "AABDELT");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 667);
  assert(count_nonscoring_plays(move_list) == 96);

  SortedMoveList *test_gen_all_moves_full_rack_sorted_move_list =
      create_sorted_move_list(move_list);

  int highest_scores[] = {38, 36, 36, 34, 34, 33, 30, 30, 30, 28};
  int number_of_highest_scores = sizeof(highest_scores) / sizeof(int);
  for (int i = 0; i < number_of_highest_scores; i++) {
    assert(get_score(test_gen_all_moves_full_rack_sorted_move_list->moves[i]) ==
           highest_scores[i]);
  }

  destroy_sorted_move_list(test_gen_all_moves_full_rack_sorted_move_list);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesFullRackAgain
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesSingleBlank
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "?");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 169);
  assert(count_nonscoring_plays(move_list) == 2);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesTwoBlanksOnly
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "??");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 1961);
  assert(count_nonscoring_plays(move_list) == 3);

  reset_rack(player_get_rack(player));

  // TestGenAllMovesWithBlanks
  load_cgp(game, VS_JEREMY);
  set_rack_to_string(ld, player_get_rack(player), "DDESW??");
  generate_moves_for_movegen(NULL, gen, player, 0);
  assert(count_scoring_plays(move_list) == 8285);
  assert(count_nonscoring_plays(move_list) == 1);

  SortedMoveList *test_gen_all_moves_with_blanks_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, test_gen_all_moves_with_blanks_sorted_move_list, 0,
              "14B hEaDW(OR)DS 106");
  assert_move(game, test_gen_all_moves_with_blanks_sorted_move_list, 1,
              "14B hEaDW(OR)D 38");

  destroy_sorted_move_list(test_gen_all_moves_with_blanks_sorted_move_list);

  reset_rack(player_get_rack(player));

  // TestGiantTwentySevenTimer
  load_cgp(game, VS_OXY);
  set_rack_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_movegen(NULL, gen, player, 0);
  assert(count_scoring_plays(move_list) == 513);
  assert(count_nonscoring_plays(move_list) == 1);

  SortedMoveList *test_giant_twenty_seven_timer_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, test_giant_twenty_seven_timer_sorted_move_list, 0,
              "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  destroy_sorted_move_list(test_giant_twenty_seven_timer_sorted_move_list);
  reset_game(game);
  reset_rack(player_get_rack(player));

  // TestGenerateEmptyBoard
  set_rack_to_string(ld, player_get_rack(player), "DEGORV?");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 3307);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_generate_empty_board_sorted_move_list =
      create_sorted_move_list(move_list);

  const Move *move = test_generate_empty_board_sorted_move_list->moves[0];
  assert(get_score(move) == 80);
  assert(move_get_tiles_played(move) == 7);
  assert(get_tiles_length(move) == 7);
  assert(get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(get_row_start(move) == 7);

  destroy_sorted_move_list(test_generate_empty_board_sorted_move_list);
  reset_rack(player_get_rack(player));

  // TestGenerateNoPlays
  load_cgp(game, VS_JEREMY);
  set_rack_to_string(ld, player_get_rack(player), "V");
  generate_moves_for_movegen(NULL, gen, player, 0);
  assert(count_scoring_plays(move_list) == 0);
  assert(count_nonscoring_plays(move_list) == 1);
  assert(get_move_type(move_list_get_move(move_list, 0)) == GAME_EVENT_PASS);

  reset_rack(player_get_rack(player));

  // TestRowEquivalent
  load_cgp(game, TEST_DUPE);

  Game *game_two = create_game(config);
  Generator *gen_two = game_get_gen(game_two);
  Board *board_two = gen_get_board(gen_two);
  LetterDistribution *ld_two = gen_get_ld(gen_two);

  set_row(game_two, 7, " INCITES");
  set_row(game_two, 8, "IS");
  set_row(game_two, 9, "T");
  update_all_anchors(board_two);
  generate_all_cross_sets(kwg, kwg, ld_two, board_two,
                          !gen_get_kwgs_are_distinct(gen_two));

  assert_boards_are_equal(board, board_two);

  reset_game(game);
  reset_game(game_two);
  reset_rack(player_get_rack(player));

  // TestGenExchange
  set_rack_to_string(ld, player_get_rack(player), "ABCDEF?");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_nonscoring_plays(move_list) == 128);

  destroy_game(game);
  destroy_game(game_two);
}

// print assertions to paste into leave_lookup_test
void print_leave_lookup_test(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  double *leaves = leave_map_get_leave_values(gen_get_leave_map(gen));
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

void leave_lookup_test(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);

  for (int i = 0; i < 2; i++) {
    bool add_exchanges = i == 0;
    generate_leaves_for_game(game, add_exchanges);
    const double *leaves = leave_map_get_leave_values(gen_get_leave_map(gen));
    assert(within_epsilon(leaves[0], +0.000000));        //
    assert(within_epsilon(leaves[1], -0.079110));        // M
    assert(within_epsilon(leaves[2], -1.092266));        // O
    assert(within_epsilon(leaves[3], +0.427566));        // MO
    assert(within_epsilon(leaves[6], -8.156165));        // OO
    assert(within_epsilon(leaves[7], -4.466051));        // MOO
    assert(within_epsilon(leaves[14], -18.868383));      // OOO
    assert(within_epsilon(leaves[15], -14.565833));      // MOOO
    assert(within_epsilon(leaves[16], +1.924450));       // R
    assert(within_epsilon(leaves[17], +0.965204));       // MR
    assert(within_epsilon(leaves[18], +1.631953));       // OR
    assert(within_epsilon(leaves[19], +2.601703));       // MOR
    assert(within_epsilon(leaves[22], -5.642596));       // OOR
    assert(within_epsilon(leaves[23], -1.488737));       // MOOR
    assert(within_epsilon(leaves[30], -17.137913));      // OOOR
    assert(within_epsilon(leaves[31], -12.899072));      // MOOOR
    assert(within_epsilon(leaves[48], -5.277321));       // RT
    assert(within_epsilon(leaves[49], -7.450112));       // MRT
    assert(within_epsilon(leaves[50], -4.813058));       // ORT
    assert(within_epsilon(leaves[51], -4.582363));       // MORT
    assert(within_epsilon(leaves[54], -11.206508));      // OORT
    assert(within_epsilon(leaves[55], -7.305244));       // MOORT
    assert(within_epsilon(leaves[62], -21.169294));      // OOORT
    assert(within_epsilon(leaves[63], -16.637489));      // MOOORT
    assert(within_epsilon(leaves[64], +0.878783));       // T
    assert(within_epsilon(leaves[65], -0.536439));       // MT
    assert(within_epsilon(leaves[66], +0.461634));       // OT
    assert(within_epsilon(leaves[67], +0.634061));       // MOT
    assert(within_epsilon(leaves[70], -6.678402));       // OOT
    assert(within_epsilon(leaves[71], -3.665847));       // MOOT
    assert(within_epsilon(leaves[78], -18.284534));      // OOOT
    assert(within_epsilon(leaves[79], -14.382346));      // MOOOT
    assert(within_epsilon(leaves[80], +2.934475));       // RT
    assert(within_epsilon(leaves[81], +0.090591));       // MRT
    assert(within_epsilon(leaves[82], +3.786163));       // ORT
    assert(within_epsilon(leaves[83], +2.442589));       // MORT
    assert(within_epsilon(leaves[86], -3.260469));       // OORT
    assert(within_epsilon(leaves[87], -0.355031));       // MOORT
    assert(within_epsilon(leaves[94], -15.671781));      // OOORT
    assert(within_epsilon(leaves[95], -12.082211));      // MOOORT
    assert(within_epsilon(leaves[112], -5.691820));      // RTT
    assert(within_epsilon(leaves[113], -10.848881));     // MRTT
    assert(within_epsilon(leaves[114], -3.967470));      // ORTT
    assert(within_epsilon(leaves[115], -7.316442));      // MORTT
    assert(within_epsilon(leaves[118], -9.621570));      // OORTT
    assert(within_epsilon(leaves[119], -8.197909));      // MOORTT
    assert(within_epsilon(leaves[126], -18.412781));     // OOORTT
    assert(within_epsilon(leaves[127], -100000.000000)); // MOOORTT
  }
  destroy_game(game);
}

void exchange_tests(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  MoveList *move_list = gen_get_move_list(gen);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);
  // The top equity plays uses 7 tiles,
  // so exchanges should not be possible.
  play_top_n_equity_move(game, 0);

  generate_moves_for_game(game);
  SortedMoveList *test_not_an_exchange_sorted_move_list =
      create_sorted_move_list(move_list);
  assert(get_move_type(test_not_an_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  destroy_sorted_move_list(test_not_an_exchange_sorted_move_list);

  load_cgp(game, cgp);
  // The second top equity play only uses
  // 4 tiles, so exchanges should be the best play.
  play_top_n_equity_move(game, 1);
  generate_moves_for_game(game);
  SortedMoveList *test_exchange_sorted_move_list =
      create_sorted_move_list(move_list);

  assert(get_move_type(test_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_EXCHANGE);
  assert(get_score(test_exchange_sorted_move_list->moves[0]) == 0);
  assert(get_tiles_length(test_exchange_sorted_move_list->moves[0]) ==
         move_get_tiles_played(test_exchange_sorted_move_list->moves[0]));
  destroy_sorted_move_list(test_exchange_sorted_move_list);

  destroy_game(game);
}

void many_moves_tests(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  MoveList *move_list = gen_get_move_list(gen);

  load_cgp(game, MANY_MOVES);
  generate_moves_for_game(game);
  assert(count_scoring_plays(move_list) == 238895);
  assert(count_nonscoring_plays(move_list) == 96);

  destroy_game(game);
}

void equity_test(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);

  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  int ld_size = letter_distribution_get_size(ld);

  Player *player = game_get_player(game, 0);
  MoveList *move_list = gen_get_move_list(gen);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  const KLV *klv = player_get_klv(player);
  // A middlegame is chosen to avoid
  // the opening and endgame equity adjustments
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_movegen(NULL, gen, player, 1);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  SortedMoveList *equity_test_sorted_move_list =
      create_sorted_move_list(move_list);

  double previous_equity = 1000000.0;
  Rack *move_rack = create_rack(ld_size);
  int number_of_moves = equity_test_sorted_move_list->count;

  for (int i = 0; i < number_of_moves - 1; i++) {
    const Move *move = equity_test_sorted_move_list->moves[i];
    assert(get_equity(move) <= previous_equity);
    set_rack_to_string(ld, move_rack, "AFGIIIS");
    double leave_value = get_leave_value_for_move(klv, move, move_rack);
    assert(within_epsilon(get_equity(move),
                          (double)get_score(move) + leave_value));
    previous_equity = get_equity(move);
  }
  assert(
      get_move_type(equity_test_sorted_move_list->moves[number_of_moves - 1]) ==
      GAME_EVENT_PASS);

  destroy_sorted_move_list(equity_test_sorted_move_list);
  destroy_rack(move_rack);
  destroy_game(game);
}

void top_equity_play_recorder_test(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);

  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  Player *player = game_get_player(game, 0);

  player_set_move_record_type(player, MOVE_RECORD_BEST);

  load_cgp(game, VS_JEREMY);
  set_rack_to_string(ld, player_get_rack(player), "DDESW??");
  generate_moves_for_movegen(NULL, gen, player, 0);

  assert_move(game, NULL, 0, "14B hEaDW(OR)DS 106");

  reset_rack(player_get_rack(player));

  load_cgp(game, VS_OXY);
  set_rack_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_movegen(NULL, gen, player, 0);

  assert_move(game, NULL, 0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  destroy_game(game);
}

void distinct_lexica_test(TestConfig *testconfig) {
  const Config *config = get_distinct_lexica_config(testconfig);

  Game *game = create_game(config);
  Generator *gen = game_get_gen(game);
  LetterDistribution *ld = gen_get_ld(gen);
  MoveList *move_list = gen_get_move_list(gen);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  player_set_move_record_type(player0, MOVE_RECORD_BEST);
  player_set_move_record_type(player1, MOVE_RECORD_BEST);

  // Play SPORK, better than best NWL move of PORKS
  set_rack_to_string(ld, player0_rack, "KOPRRSS");
  generate_moves_for_game(game);
  assert_move(game, NULL, 0, "8H SPORK 32");

  play_move(move_list_get_move(move_list, 0), game);
  reset_move_list(move_list);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  set_rack_to_string(ld, player1_rack, "CEHIIRZ");
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "H8 (S)CHIZIER 146");

  play_move(move_list_get_move(move_list, 0), game);
  reset_move_list(move_list);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  set_rack_to_string(ld, player0_rack, "GGLLOWY");
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "11G W(I)GGLY 28");

  play_move(move_list_get_move(move_list, 0), game);
  reset_move_list(move_list);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  set_rack_to_string(ld, player1_rack, "AEEQRSU");
  generate_moves_for_game(game);

  assert_move(game, NULL, 0, "13C QUEAS(I)ER 88");

  play_move(move_list_get_move(move_list, 0), game);
  reset_move_list(move_list);

  destroy_game(game);
}

void test_movegen(TestConfig *testconfig) {
  macondo_tests(testconfig);
  exchange_tests(testconfig);
  equity_test(testconfig);
  top_equity_play_recorder_test(testconfig);
  distinct_lexica_test(testconfig);
}
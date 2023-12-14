#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

#include "../src/def/rack_defs.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/move.h"
#include "../src/ent/move_gen.h"
#include "../src/ent/player.h"
#include "../src/impl/gameplay.h"

#include "../src/util/string_util.h"
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

// Use -1 for row if setting with CGP
// Use NULL for rack_string if setting with CGP
void assert_move_gen_row(Game *game, const char *rack_string,
                         const char *row_string, int row, int min_length,
                         int expected_plays, int *move_indexes,
                         const char **move_strings) {
  if (row_string) {
    StringBuilder *cgp_builder = create_string_builder();
    for (int i = 0; i < BOARD_DIM; i++) {
      if (i == row) {
        string_builder_add_string(cgp_builder, row_string);
      } else {
        string_builder_add_int(cgp_builder, BOARD_DIM);
      }
      if (i != BOARD_DIM - 1) {
        string_builder_add_string(cgp_builder, "/");
      }
    }

    string_builder_add_formatted_string(cgp_builder, " %s/ 0/0 0", rack_string);
    load_cgp_or_die(game, string_builder_peek(cgp_builder));
    destroy_string_builder(cgp_builder);
  }

  if (rack_string) {
    Player *player_on_turn =
        game_get_player(game, game_get_player_on_turn_index(game));
    set_rack_to_string(game_get_ld(game), player_get_rack(player_on_turn),
                       rack_string);
  }

  generate_moves_for_game_with_player_move_types(game);
  MoveList *move_list = game_get_move_list(game);
  SortedMoveList *sml = create_sorted_move_list(move_list);

  if (expected_plays >= 0) {
    int actual_plays = 0;
    for (int i = 0; i < sml->count; i++) {
      Move *move = sml->moves[i];
      if (get_row_start(move) == row &&
          get_dir(move) == BOARD_HORIZONTAL_DIRECTION &&
          (min_length < 0 || get_tiles_length(move) >= min_length)) {
        actual_plays++;
      } else {
        sml->count--;
        Move *tmp = sml->moves[i];
        sml->moves[i] = sml->moves[sml->count];
        sml->moves[sml->count] = tmp;
        i--;
      }
    }

    resort_sorted_move_list_by_score(sml);
    assert(expected_plays == actual_plays);
  }

  if (move_indexes) {
    int i = 0;
    while (true) {
      int move_index = move_indexes[i];
      if (move_index < 0) {
        break;
      }
      assert_move(game, sml, move_index, move_strings[i]);
      i++;
    }
  }
  destroy_sorted_move_list(sml);
}

void macondo_tests(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Board *board = game_get_board(game);
  LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = game_get_move_list(game);
  const KWG *kwg = player_get_kwg(player);

  // TestSimpleRowGen
  assert_move_gen_row(game, "P", "5REGNANT3", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, "O", "2PORTOLAN5", 2, 9, 1, NULL, NULL);
  assert_move_gen_row(game, "S", "2PORTOLAN5", 2, 9, 1, NULL, NULL);
  assert_move_gen_row(game, "?", "2PORTOLAN5", 2, 9, 2, NULL, NULL);
  assert_move_gen_row(game, "TY", "2SOVRAN7", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, "ING", "2LAUGH8", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, "ZA", "2BE11", 3, 0, 0, NULL, NULL);
  assert_move_gen_row(game, "AENPPSW", "8CHAWING", 14, 14, 1, NULL, NULL);
  assert_move_gen_row(game, "ABEHINT", "3THERMOS2A2", 4, 10, 2, NULL, NULL);
  assert_move_gen_row(game, "ABEHITT", "2THERMOS1A4", 8, 10, 1, NULL, NULL);
  assert_move_gen_row(game, "TT", "2THERMOS1A4", 10, 2, 3, NULL, NULL);

  // TestGenThroughBothWaysAllowedLetters
  assert_move_gen_row(
      game, "ABEHINT", "3THERMOS2A2", 2, 10, 2, (int[]){0, 1, -1},
      (const char *[]){"3B HI(THERMOS)T 36", "3B NE(THERMOS)T 30"});

  // TestRowGen
  load_cgp(game, VS_ED);
  assert_move_gen_row(game, "AAEIRST", NULL, 4, 7, 2, (int[]){0, 1, -1},
                      (const char *[]){"5B AIR(GLOWS) 12", "5C RE(GLOWS) 11"});

  // TestOtherRowGen
  load_cgp(game, VS_MATT);
  assert_move_gen_row(game, "A", NULL, 14, 7, 1, (int[]){0, -1},
                      (const char *[]){"15C A(VENGED) 12"});

  // TestOneMoreRowGen
  load_cgp(game, VS_MATT);
  assert_move_gen_row(game, "A", NULL, 0, 2, 1, (int[]){0, -1},
                      (const char *[]){"1L (F)A 5"});

  // TestGenAllMovesSingleTile
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "A");
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 24);

  // TestGenAllMovesFullRack
  load_cgp(game, VS_MATT);
  set_rack_to_string(ld, player_get_rack(player), "AABDELT");
  generate_moves_for_game_with_player_move_types(game);
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

  // TestGenAllMovesFullRackAgain
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  // TestGenAllMovesSingleBlank
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "?");
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 169);
  assert(count_nonscoring_plays(move_list) == 2);

  // TestGenAllMovesTwoBlanksOnly
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "??");
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 1961);
  assert(count_nonscoring_plays(move_list) == 3);

  // TestGenAllMovesWithBlanks
  load_cgp(game, VS_JEREMY);
  set_rack_to_string(ld, player_get_rack(player), "DDESW??");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_at_most_to_rack(game_get_bag(game),
                       player_get_rack(game_get_player(game, 1)), RACK_SIZE, 1);
  generate_moves_for_game_with_player_move_types(game);
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
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 513);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_giant_twenty_seven_timer_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, test_giant_twenty_seven_timer_sorted_move_list, 0,
              "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  destroy_sorted_move_list(test_giant_twenty_seven_timer_sorted_move_list);

  // TestGenerateEmptyBoard
  reset_game(game);
  set_rack_to_string(ld, player_get_rack(player), "DEGORV?");
  generate_moves_for_game_with_player_move_types(game);
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
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_at_most_to_rack(game_get_bag(game),
                       player_get_rack(game_get_player(game, 1)), RACK_SIZE, 1);
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 0);
  assert(count_nonscoring_plays(move_list) == 1);
  assert(get_move_type(move_list_get_move(move_list, 0)) == GAME_EVENT_PASS);

  reset_rack(player_get_rack(player));

  // TestRowEquivalent
  load_cgp(game, TEST_DUPE);

  Game *game_two = create_game(config);
  Board *board_two = game_get_board(game_two);
  LetterDistribution *ld_two = game_get_ld(game_two);

  set_row(game_two, 7, " INCITES");
  set_row(game_two, 8, "IS");
  set_row(game_two, 9, "T");
  update_all_anchors(board_two);
  generate_all_cross_sets(
      kwg, kwg, ld_two, board_two,
      game_get_data_is_shared(game_two, PLAYERS_DATA_TYPE_KWG));

  assert_boards_are_equal(board, board_two);

  reset_game(game);
  reset_game(game_two);
  reset_rack(player_get_rack(player));

  // TestGenExchange
  set_rack_to_string(ld, player_get_rack(player), "ABCDEF?");
  generate_moves_for_game_with_player_move_types(game);
  assert(count_nonscoring_plays(move_list) == 128);

  destroy_game(game);
  destroy_game(game_two);
}

// print assertions to paste into leave_lookup_test
void print_leave_lookup_test(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  MoveGen *gen = game_get_move_gen(game);

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
  MoveGen *gen = game_get_move_gen(game);

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
  MoveList *move_list = game_get_move_list(game);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp(game, cgp);
  // The top equity plays uses 7 tiles,
  // so exchanges should not be possible.
  play_top_n_equity_move(game, 0);

  generate_moves_for_game_with_player_move_types(game);
  SortedMoveList *test_not_an_exchange_sorted_move_list =
      create_sorted_move_list(move_list);
  assert(get_move_type(test_not_an_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  destroy_sorted_move_list(test_not_an_exchange_sorted_move_list);

  load_cgp(game, cgp);
  // The second top equity play only uses
  // 4 tiles, so exchanges should be the best play.
  play_top_n_equity_move(game, 1);
  generate_moves_for_game_with_player_move_types(game);
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
  MoveList *move_list = game_get_move_list(game);

  load_cgp(game, MANY_MOVES);
  generate_moves_for_game_with_player_move_types(game);
  assert(count_scoring_plays(move_list) == 238895);
  assert(count_nonscoring_plays(move_list) == 96);

  destroy_game(game);
}

void equity_test(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);

  Game *game = create_game(config);
  LetterDistribution *ld = game_get_ld(game);
  int ld_size = letter_distribution_get_size(ld);

  Player *player = game_get_player(game, 0);
  MoveList *move_list = game_get_move_list(game);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  const KLV *klv = player_get_klv(player);
  // A middlegame is chosen to avoid
  // the opening and endgame equity adjustments
  load_cgp(game, VS_ED);
  set_rack_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game_with_player_move_types(game);
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
  LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);

  player_set_move_record_type(player, MOVE_RECORD_BEST);

  load_cgp(game, VS_JEREMY);
  set_rack_to_string(ld, player_get_rack(player), "DDESW??");
  generate_moves_for_game_with_player_move_types(game);

  assert_move(game, NULL, 0, "14B hEaDW(OR)DS 106");

  reset_rack(player_get_rack(player));

  load_cgp(game, VS_OXY);
  set_rack_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_game_with_player_move_types(game);

  assert_move(game, NULL, 0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  destroy_game(game);
}

void distinct_lexica_test(TestConfig *testconfig) {
  Config *config = get_distinct_lexica_config(testconfig);

  Game *game = create_game(config);
  LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = game_get_move_list(game);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  player_set_move_record_type(player0, MOVE_RECORD_BEST);
  player_set_move_record_type(player1, MOVE_RECORD_BEST);

  // Play SPORK, better than best NWL move of PORKS
  set_rack_to_string(ld, player0_rack, "KOPRRSS");
  generate_moves_for_game_with_player_move_types(game);
  assert_move(game, NULL, 0, "8H SPORK 32");

  play_move(move_list_get_move(move_list, 0), game);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  set_rack_to_string(ld, player1_rack, "CEHIIRZ");
  generate_moves_for_game_with_player_move_types(game);

  assert_move(game, NULL, 0, "H8 (S)CHIZIER 146");

  play_move(move_list_get_move(move_list, 0), game);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  set_rack_to_string(ld, player0_rack, "GGLLOWY");
  generate_moves_for_game_with_player_move_types(game);

  assert_move(game, NULL, 0, "11G W(I)GGLY 28");

  play_move(move_list_get_move(move_list, 0), game);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  set_rack_to_string(ld, player1_rack, "AEEQRSU");

  generate_moves_for_game_with_player_move_types(game);
  player_set_move_record_type(player1, MOVE_RECORD_BEST);
  assert_move(game, NULL, 0, "13C QUEAS(I)ER 88");

  destroy_game(game);
}

void test_move_gen(TestConfig *testconfig) {
  macondo_tests(testconfig);
  exchange_tests(testconfig);
  equity_test(testconfig);
  top_equity_play_recorder_test(testconfig);
  distinct_lexica_test(testconfig);
}
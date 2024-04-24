#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/game_history_defs.h"
#include "../../src/def/move_defs.h"
#include "../../src/def/rack_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

// Use -1 for row if setting with CGP
// Use NULL for rack_string if setting with CGP
void assert_move_gen_row(Game *game, MoveList *move_list,
                         const char *rack_string, const char *row_string,
                         int row, int min_length, int expected_plays,
                         int *move_indexes, const char **move_strings) {
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
    rack_set_to_string(game_get_ld(game), player_get_rack(player_on_turn),
                       rack_string);
  }

  generate_moves_for_game(game, 0, move_list);
  SortedMoveList *sml = create_sorted_move_list(move_list);

  if (expected_plays >= 0) {
    int actual_plays = 0;
    for (int i = 0; i < sml->count; i++) {
      Move *move = sml->moves[i];
      if (move_get_row_start(move) == row &&
          move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION &&
          (min_length < 0 || move_get_tiles_length(move) >= min_length)) {
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
      assert_move(game, move_list, sml, move_index, move_strings[i]);
      i++;
    }
  }
  destroy_sorted_move_list(sml);
}

void macondo_tests() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(10000);

  // TestSimpleRowGen
  assert_move_gen_row(game, move_list, "P", "5REGNANT3", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, move_list, "O", "2PORTOLAN5", 2, 9, 1, NULL, NULL);
  assert_move_gen_row(game, move_list, "S", "2PORTOLAN5", 2, 9, 1, NULL, NULL);
  assert_move_gen_row(game, move_list, "?", "2PORTOLAN5", 2, 9, 2, NULL, NULL);
  assert_move_gen_row(game, move_list, "TY", "2SOVRAN7", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, move_list, "ING", "2LAUGH8", 2, 8, 1, NULL, NULL);
  assert_move_gen_row(game, move_list, "ZA", "2BE11", 3, 0, 0, NULL, NULL);
  assert_move_gen_row(game, move_list, "AENPPSW", "8CHAWING", 14, 14, 1, NULL,
                      NULL);
  assert_move_gen_row(game, move_list, "ABEHINT", "3THERMOS2A2", 4, 10, 2, NULL,
                      NULL);
  assert_move_gen_row(game, move_list, "ABEHITT", "2THERMOS1A4", 8, 10, 1, NULL,
                      NULL);
  assert_move_gen_row(game, move_list, "TT", "2THERMOS1A4", 10, 2, 3, NULL,
                      NULL);

  // TestGenThroughBothWaysboard_is_letter_allowed_in_cross_setLetters
  assert_move_gen_row(
      game, move_list, "ABEHINT", "3THERMOS2A2", 2, 10, 2, (int[]){0, 1, -1},
      (const char *[]){"3B HI(THERMOS)T 36", "3B NE(THERMOS)T 30"});

  // TestRowGen
  game_load_cgp(game, VS_ED);
  assert_move_gen_row(game, move_list, "AAEIRST", NULL, 4, 7, 2,
                      (int[]){0, 1, -1},
                      (const char *[]){"5B AIR(GLOWS) 12", "5C RE(GLOWS) 11"});

  // TestOtherRowGen
  game_load_cgp(game, VS_MATT);
  assert_move_gen_row(game, move_list, "A", NULL, 14, 7, 1, (int[]){0, -1},
                      (const char *[]){"15C A(VENGED) 12"});

  // TestOneMoreRowGen
  game_load_cgp(game, VS_MATT);
  assert_move_gen_row(game, move_list, "A", NULL, 0, 2, 1, (int[]){0, -1},
                      (const char *[]){"1L (F)A 5"});

  // TestGenAllMovesSingleTile
  game_load_cgp(game, VS_MATT);
  rack_set_to_string(ld, player_get_rack(player), "A");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 24);

  // TestGenAllMovesFullRack
  game_load_cgp(game, VS_MATT);
  rack_set_to_string(ld, player_get_rack(player), "AABDELT");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 667);
  assert(count_nonscoring_plays(move_list) == 96);

  SortedMoveList *test_gen_all_moves_full_rack_sorted_move_list =
      create_sorted_move_list(move_list);

  int highest_scores[] = {38, 36, 36, 34, 34, 33, 30, 30, 30, 28};
  int number_of_highest_scores = sizeof(highest_scores) / sizeof(int);
  for (int i = 0; i < number_of_highest_scores; i++) {
    assert(move_get_score(
               test_gen_all_moves_full_rack_sorted_move_list->moves[i]) ==
           highest_scores[i]);
  }

  destroy_sorted_move_list(test_gen_all_moves_full_rack_sorted_move_list);

  // TestGenAllMovesFullRackAgain
  game_load_cgp(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  // TestGenAllMovesSingleBlank
  game_load_cgp(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "?");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 169);
  assert(count_nonscoring_plays(move_list) == 2);

  // TestGenAllMovesTwoBlanksOnly
  game_load_cgp(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "??");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 1961);
  assert(count_nonscoring_plays(move_list) == 3);

  // TestGenAllMovesWithBlanks
  game_load_cgp(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "DDESW??");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_at_most_to_rack(game_get_bag(game),
                       player_get_rack(game_get_player(game, 1)), RACK_SIZE, 1);
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 8285);
  assert(count_nonscoring_plays(move_list) == 1);

  SortedMoveList *test_gen_all_moves_with_blanks_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, move_list, test_gen_all_moves_with_blanks_sorted_move_list,
              0, "14B hEaDW(OR)DS 106");
  assert_move(game, move_list, test_gen_all_moves_with_blanks_sorted_move_list,
              1, "14B hEaDW(OR)D 38");

  destroy_sorted_move_list(test_gen_all_moves_with_blanks_sorted_move_list);

  rack_reset(player_get_rack(player));

  // TestGiantTwentySevenTimer
  game_load_cgp(game, VS_OXY);
  rack_set_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 513);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_giant_twenty_seven_timer_sorted_move_list =
      create_sorted_move_list(move_list);

  assert_move(game, move_list, test_giant_twenty_seven_timer_sorted_move_list,
              0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  destroy_sorted_move_list(test_giant_twenty_seven_timer_sorted_move_list);

  // TestGenerateEmptyBoard
  game_reset(game);
  rack_set_to_string(ld, player_get_rack(player), "DEGORV?");
  generate_moves_for_game(game, 0, move_list);

  sort_and_print_move_list(game_get_board(game), game_get_ld(game), move_list);

  assert(count_scoring_plays(move_list) == 3307);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_generate_empty_board_sorted_move_list =
      create_sorted_move_list(move_list);

  const Move *move = test_generate_empty_board_sorted_move_list->moves[0];
  assert(move_get_score(move) == 80);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_tiles_length(move) == 7);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_row_start(move) == 7);

  destroy_sorted_move_list(test_generate_empty_board_sorted_move_list);
  rack_reset(player_get_rack(player));

  // Check that GONOPORE is the best play
  game_load_cgp(game, FRAWZEY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "GONOPOR");
  generate_moves_for_game(game, 0, move_list);

  SortedMoveList *test_generate_gonopore = create_sorted_move_list(move_list);

  move = test_generate_gonopore->moves[0];
  assert(move_get_score(move) == 86);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_row_start(move) == 14);
  assert(move_get_col_start(move) == 0);

  destroy_sorted_move_list(test_generate_gonopore);
  rack_reset(player_get_rack(player));

  // TestGenerateNoPlays
  game_load_cgp(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "V");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_at_most_to_rack(game_get_bag(game),
                       player_get_rack(game_get_player(game, 1)), RACK_SIZE, 1);
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 0);
  assert(count_nonscoring_plays(move_list) == 1);
  assert(move_get_type(move_list_get_move(move_list, 0)) == GAME_EVENT_PASS);

  rack_reset(player_get_rack(player));

  // TestRowEquivalent
  game_load_cgp(game, TEST_DUPE);

  Game *game_two = game_create(config);
  Board *board_two = game_get_board(game_two);

  set_row(game_two, 7, " INCITES");
  set_row(game_two, 8, "IS");
  set_row(game_two, 9, "T");
  board_update_all_anchors(board_two);
  game_gen_all_cross_sets(game_two);

  assert_boards_are_equal(board, board_two);

  game_reset(game);
  game_reset(game_two);
  rack_reset(player_get_rack(player));

  // TestGenExchange
  rack_set_to_string(ld, player_get_rack(player), "ABCDEF?");
  generate_moves_for_game(game, 0, move_list);
  assert(count_nonscoring_plays(move_list) == 128);

  move_list_destroy(move_list);
  game_destroy(game);
  game_destroy(game_two);
  config_destroy(config);
}

void leave_lookup_test() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  MoveList *move_list = move_list_create(1000);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  game_load_cgp(game, cgp);

  Rack *rack = rack_create(ld_get_size(ld));
  for (int i = 0; i < 2; i++) {
    int number_of_moves = move_list_get_count(move_list);
    for (int i = 0; i < number_of_moves; i++) {
      Move *move = move_list_get_move(move_list, i);
      // This is after the opening and before the endgame
      // so the other equity adjustments will not be in effect.
      double move_leave_value = move_get_equity(move) - move_get_score(move);
      if (i == 0) {
        rack_set_to_string(ld, rack, "MOOORRT");
      } else {
        rack_set_to_string(ld, rack, "BFQRTTV");
      }
      double leave_value = get_leave_value_for_move(klv, move, rack);
      within_epsilon(move_leave_value, leave_value);
    }
    play_top_n_equity_move(game, 0);
  }
  rack_destroy(rack);
  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void unfound_leave_lookup_test() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  MoveList *move_list = move_list_create(1);
  Rack *rack = player_get_rack(game_get_player(game, 0));

  char cgp[300] = "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
                  "UNFOUND/UNFOUND 0/0 0 lex CSW21;";
  game_load_cgp(game, cgp);

  // CGP loader won't accept this impossible rack so we set it manually here.
  rack_set_to_string(game_get_ld(game), rack, "PIZZAQQ");

  generate_moves_for_game(game, 0, move_list);
  Move *move = move_list_get_move(move_list, 0);

  assert_move(game, move_list, NULL, 0, "8D PIZZA 56");
  // Unfound leave of QQ gets 0.0 value.
  assert(within_epsilon(move_get_equity(move), 56.0));

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void exchange_tests() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  MoveList *move_list = move_list_create(10);

  char cgp[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                  "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                  "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  game_load_cgp(game, cgp);
  // The top equity plays uses 7 tiles,
  // so exchanges should not be possible.
  play_top_n_equity_move(game, 0);

  generate_moves(game, MOVE_RECORD_BEST, MOVE_SORT_EQUITY, 0, move_list);
  SortedMoveList *test_not_an_exchange_sorted_move_list =
      create_sorted_move_list(move_list);
  assert(move_get_type(test_not_an_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  destroy_sorted_move_list(test_not_an_exchange_sorted_move_list);

  game_load_cgp(game, cgp);
  // The second top equity play only uses
  // 4 tiles, so exchanges should be the best play.
  play_top_n_equity_move(game, 1);
  generate_moves_for_game(game, 0, move_list);
  SortedMoveList *test_exchange_sorted_move_list =
      create_sorted_move_list(move_list);

  assert(move_get_type(test_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_EXCHANGE);
  assert(move_get_score(test_exchange_sorted_move_list->moves[0]) == 0);
  assert(move_get_tiles_length(test_exchange_sorted_move_list->moves[0]) ==
         move_get_tiles_played(test_exchange_sorted_move_list->moves[0]));
  destroy_sorted_move_list(test_exchange_sorted_move_list);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void many_moves_tests() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  MoveList *move_list = move_list_create(239000);

  game_load_cgp(game, MANY_MOVES);
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 238895);
  assert(count_nonscoring_plays(move_list) == 96);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void equity_test() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(300);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  const KLV *klv = player_get_klv(player);
  // A middlegame is chosen to avoid
  // the opening and endgame equity adjustments
  game_load_cgp(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game(game, 0, move_list);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  SortedMoveList *equity_test_sorted_move_list =
      create_sorted_move_list(move_list);

  double previous_equity = 1000000.0;
  Rack *move_rack = rack_create(ld_size);
  int number_of_moves = equity_test_sorted_move_list->count;

  for (int i = 0; i < number_of_moves - 1; i++) {
    const Move *move = equity_test_sorted_move_list->moves[i];
    assert(move_get_equity(move) <= previous_equity);
    rack_set_to_string(ld, move_rack, "AFGIIIS");
    double leave_value = get_leave_value_for_move(klv, move, move_rack);
    assert(within_epsilon(move_get_equity(move),
                          (double)move_get_score(move) + leave_value));
    previous_equity = move_get_equity(move);
  }
  assert(
      move_get_type(equity_test_sorted_move_list->moves[number_of_moves - 1]) ==
      GAME_EVENT_PASS);

  destroy_sorted_move_list(equity_test_sorted_move_list);
  rack_destroy(move_rack);
  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void top_equity_play_recorder_test() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1);
  player_set_move_record_type(player, MOVE_RECORD_BEST);

  game_load_cgp(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "DDESW??");
  generate_moves_for_game(game, 0, move_list);

  assert_move(game, move_list, NULL, 0, "14B hEaDW(OR)DS 106");

  rack_reset(player_get_rack(player));

  game_load_cgp(game, VS_OXY);
  rack_set_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_game(game, 0, move_list);

  assert_move(game, move_list, NULL, 0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void distinct_lexica_test() {
  Config *config =
      create_config_or_die("setoptions l1 CSW21 l2 NWL20 s1 equity s2 equity "
                           "r1 best r2 best numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = move_list_create(1);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  // Play SPORK, better than best NWL move of PORKS
  rack_set_to_string(ld, player0_rack, "KOPRRSS");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "8H SPORK 32");

  play_move(move_list_get_move(move_list, 0), game);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  rack_set_to_string(ld, player1_rack, "CEHIIRZ");
  generate_moves_for_game(game, 0, move_list);

  assert_move(game, move_list, NULL, 0, "H8 (S)CHIZIER 146");

  play_move(move_list_get_move(move_list, 0), game);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  rack_set_to_string(ld, player0_rack, "GGLLOWY");
  generate_moves_for_game(game, 0, move_list);

  assert_move(game, move_list, NULL, 0, "11G W(I)GGLY 28");

  play_move(move_list_get_move(move_list, 0), game);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  rack_set_to_string(ld, player1_rack, "AEEQRSU");

  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "13C QUEAS(I)ER 88");

  move_list_destroy(move_list);
  game_destroy(game);

  load_config_or_die(
      config,
      "setoptions l2 CSW21 l1 NWL20 k2 CSW21 k1 NWL20 s1 equity s2 equity "
      "r1 best r2 best numplays 1");

  // Ensure loading from CGP correctly sets the distinct cross sets
  Game *game2 = game_create(config);

  load_cgp_or_die(
      game2,
      "15/15/15/15/15/15/15/7SPORK3/7C7/7H7/6WIGGLY3/7Z7/7I7/7E7/7R7 / 0/0 0");

  const LetterDistribution *ld2 = game_get_ld(game2);
  ld = game_get_ld(game2);
  MoveList *move_list2 = move_list_create(1);

  player0 = game_get_player(game2, 0);
  player1 = game_get_player(game2, 1);
  player0_rack = player_get_rack(player0);
  player1_rack = player_get_rack(player1);

  rack_set_to_string(ld2, player0_rack, "AEEQRSU");
  generate_moves_for_game(game2, 0, move_list2);
  assert_move(game2, move_list2, NULL, 0, "13C QUEAS(I)ER 88");

  game_start_next_player_turn(game2);

  rack_set_to_string(ld2, player1_rack, "AEEQRSU");
  generate_moves_for_game(game2, 0, move_list2);
  assert_move(game2, move_list2, NULL, 0, "L3 SQUEA(K)ER(Y) 100");

  move_list_destroy(move_list2);
  game_destroy(game2);

  config_destroy(config);
}

void consistent_tiebreaking_test() {
  Config *config =
      create_config_or_die("setoptions l1 CSW21 l2 NWL20 s1 equity s2 equity "
                           "r1 best r2 best numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = move_list_create(1);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  rack_set_to_string(ld, player0_rack, "EEEFVRR");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "8D FEVER 30");

  play_move(move_list_get_move(move_list, 0), game);

  // Should be NUNcLES instead of NoNFUELS
  rack_set_to_string(ld, player1_rack, "ELNNSU?");
  generate_moves_for_game(game, 0, move_list);

  assert_move(game, move_list, NULL, 0, "I2 NUNcLES 70");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void iso_test() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(10000);

  game_reset(game);
  rack_set_to_string(ld, player_get_rack(player), "DEGORV?");
  generate_moves_for_game(game, 0, move_list);
  // sort_and_print_move_list(game_get_board(game), game_get_ld(game),
  // move_list);

  printf("scm: %d\n", count_scoring_plays(move_list));
  assert(count_scoring_plays(move_list) == 3307);
  assert(count_nonscoring_plays(move_list) == 128);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_move_gen() {
  // iso_test();
  // return;
  leave_lookup_test();
  unfound_leave_lookup_test();
  macondo_tests();
  exchange_tests();
  equity_test();
  top_equity_play_recorder_test();
  distinct_lexica_test();
  consistent_tiebreaking_test();
}
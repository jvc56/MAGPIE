#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Expose internal helper from move_gen.c to use in tests
void get_blank_possibilities(const MoveGen *gen,
                             const BitRack *nonplaythrough_tiles,
                             int current_pos, bool *can_be_unblanked,
                             bool *can_be_blanked);

static inline void
set_playthrough_marked_from_string(MoveGen *gen, const LetterDistribution *ld,
                                   const char *s) {
  size_t n = strlen(s);
  gen->wmp_move_gen.word_length = (int)n;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '.') {
      gen->playthrough_marked[i] = PLAYED_THROUGH_MARKER;
    } else {
      const char tmp[2] = {c, '\0'};
      gen->playthrough_marked[i] = ld_hl_to_ml(ld, tmp);
    }
  }
  /* Clear any remaining positions to 0 for safety */
  for (size_t i = n; i < BOARD_DIM; i++) {
    gen->playthrough_marked[i] = 0;
  }
}

int count_scoring_plays(const MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < move_list_get_count(ml); i++) {
    if (move_get_type(move_list_get_move(ml, i)) ==
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      sum++;
    }
  }
  return sum;
}

int count_nonscoring_plays(const MoveList *ml) {
  int sum = 0;
  for (int i = 0; i < move_list_get_count(ml); i++) {
    if (move_get_type(move_list_get_move(ml, i)) !=
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      sum++;
    }
  }
  return sum;
}

// Use -1 for row if setting with CGP
// Use NULL for rack_string if setting with CGP
void assert_move_gen_row(Game *game, MoveList *move_list,
                         const char *rack_string, const char *row_string,
                         const int row, const int min_length,
                         const int expected_plays, const int *move_indexes,
                         const char **move_strings) {
  if (row_string) {
    StringBuilder *cgp_builder = string_builder_create();
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
    string_builder_destroy(cgp_builder);
  }

  if (rack_string) {
    const Player *player_on_turn =
        game_get_player(game, game_get_player_on_turn_index(game));
    rack_set_to_string(game_get_ld(game), player_get_rack(player_on_turn),
                       rack_string);
  }

  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  generate_moves_for_game(&move_gen_args);
  SortedMoveList *sml = sorted_move_list_create(move_list);

  if (expected_plays >= 0) {
    int actual_plays = 0;
    for (int i = 0; i < sml->count; i++) {
      const Move *move = sml->moves[i];
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
  sorted_move_list_destroy(sml);
}

void macondo_tests(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(10000);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

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

  // TestGenThroughBothWaysAllowedLetters
  assert_move_gen_row(
      game, move_list, "ABEHINT", "3THERMOS2A2", 2, 10, 2, (int[]){0, 1, -1},
      (const char *[]){"3B HI(THERMOS)T 36", "3B NE(THERMOS)T 30"});

  // TestRowGen
  load_cgp_or_die(game, VS_ED);
  assert_move_gen_row(game, move_list, "AAEIRST", NULL, 4, 7, 2,
                      (int[]){0, 1, -1},
                      (const char *[]){"5B AIR(GLOWS) 12", "5C RE(GLOWS) 11"});

  // TestOtherRowGen
  load_cgp_or_die(game, VS_MATT);
  assert_move_gen_row(game, move_list, "A", NULL, 14, 7, 1, (int[]){0, -1},
                      (const char *[]){"15C A(VENGED) 12"});

  // TestOneMoreRowGen
  load_cgp_or_die(game, VS_MATT);
  assert_move_gen_row(game, move_list, "A", NULL, 0, 2, 1, (int[]){0, -1},
                      (const char *[]){"1L (F)A 5"});

  // TestGenAllMovesSingleTile
  load_cgp_or_die(game, VS_MATT);
  rack_set_to_string(ld, player_get_rack(player), "A");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 24);

  // TestGenAllMovesFullRack
  load_cgp_or_die(game, VS_MATT);
  rack_set_to_string(ld, player_get_rack(player), "AABDELT");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 667);
  assert(count_nonscoring_plays(move_list) == 96);

  SortedMoveList *test_gen_all_moves_full_rack_sorted_move_list =
      sorted_move_list_create(move_list);

  const int highest_scores[] = {38, 36, 36, 34, 34, 33, 30, 30, 30, 28};
  const int number_of_highest_scores = sizeof(highest_scores) / sizeof(int);
  for (int i = 0; i < number_of_highest_scores; i++) {
    const Equity score_eq =
        move_get_score(test_gen_all_moves_full_rack_sorted_move_list->moves[i]);
    assert(equity_to_int(score_eq) == highest_scores[i]);
  }

  sorted_move_list_destroy(test_gen_all_moves_full_rack_sorted_move_list);

  // TestGenAllMovesFullRackAgain
  load_cgp_or_die(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  // TestGenAllMovesSingleBlank
  load_cgp_or_die(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "?");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 169);
  assert(count_nonscoring_plays(move_list) == 2);

  // TestGenAllMovesTwoBlanksOnly
  load_cgp_or_die(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "??");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 1961);
  assert(count_nonscoring_plays(move_list) == 3);

  // TestGenAllMovesWithBlanks
  load_cgp_or_die(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "DDESW??");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_to_full_rack(game, 1);
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 8285);
  assert(count_nonscoring_plays(move_list) == 1);

  SortedMoveList *test_gen_all_moves_with_blanks_sorted_move_list =
      sorted_move_list_create(move_list);

  assert_move(game, move_list, test_gen_all_moves_with_blanks_sorted_move_list,
              0, "14B hEaDW(OR)DS 106");
  assert_move(game, move_list, test_gen_all_moves_with_blanks_sorted_move_list,
              1, "14B hEaDW(OR)D 38");

  sorted_move_list_destroy(test_gen_all_moves_with_blanks_sorted_move_list);

  rack_reset(player_get_rack(player));

  // TestGiantTwentySevenTimer
  load_cgp_or_die(game, VS_OXY);
  rack_set_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 513);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_giant_twenty_seven_timer_sorted_move_list =
      sorted_move_list_create(move_list);

  assert_move(game, move_list, test_giant_twenty_seven_timer_sorted_move_list,
              0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  sorted_move_list_destroy(test_giant_twenty_seven_timer_sorted_move_list);

  // TestGenerateEmptyBoard
  game_reset(game);
  rack_set_to_string(ld, player_get_rack(player), "DEGORV?");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 3307);
  assert(count_nonscoring_plays(move_list) == 128);

  SortedMoveList *test_generate_empty_board_sorted_move_list =
      sorted_move_list_create(move_list);

  const Move *move = test_generate_empty_board_sorted_move_list->moves[0];
  assert_move_score(move, 80);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_tiles_length(move) == 7);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_row_start(move) == 7);

  sorted_move_list_destroy(test_generate_empty_board_sorted_move_list);
  rack_reset(player_get_rack(player));

  // Check that GONOPORE is the best play
  load_cgp_or_die(game, FRAWZEY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "GONOPOR");
  generate_moves_for_game(&move_gen_args);

  SortedMoveList *test_generate_gonopore = sorted_move_list_create(move_list);

  move = test_generate_gonopore->moves[0];
  assert_move_score(move, 86);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_row_start(move) == 14);
  assert(move_get_col_start(move) == 0);

  sorted_move_list_destroy(test_generate_gonopore);
  rack_reset(player_get_rack(player));

  // TestGenerateNoPlays
  load_cgp_or_die(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "V");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_to_full_rack(game, 1);
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 0);
  assert(count_nonscoring_plays(move_list) == 1);
  assert(move_get_type(move_list_get_move(move_list, 0)) == GAME_EVENT_PASS);

  rack_reset(player_get_rack(player));

  // TestRowEquivalent
  load_cgp_or_die(game, TEST_DUPE);

  Game *game_two = config_game_create(config);
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
  generate_moves_for_game(&move_gen_args);
  assert(count_nonscoring_plays(move_list) == 128);

  move_list_destroy(move_list);
  game_destroy(game);
  game_destroy(game_two);
  config_destroy(config);
}

void leave_lookup_test(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  MoveList *move_list = move_list_create(1000);

  const char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp_or_die(game, cgp);

  Rack *rack = rack_create(ld_get_size(ld));
  for (int i = 0; i < 2; i++) {
    const int number_of_moves = move_list_get_count(move_list);
    for (int j = 0; j < number_of_moves; j++) {
      const Move *move = move_list_get_move(move_list, j);
      // This is after the opening and before the endgame
      // so the other equity adjustments will not be in effect.
      const Equity move_leave_value =
          move_get_equity(move) - move_get_score(move);
      if (j == 0) {
        rack_set_to_string(ld, rack, "MOOORRT");
      } else {
        rack_set_to_string(ld, rack, "BFQRTTV");
      }
      const Equity leave_value = get_leave_value_for_move(klv, move, rack);
      assert(move_leave_value == leave_value);
    }
    play_top_n_equity_move(game, 0);
  }
  rack_destroy(rack);
  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void unfound_leave_lookup_test(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(1);
  Rack *rack = player_get_rack(game_get_player(game, 0));
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  const char cgp[300] = "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
                        "UNFOUND/UNFOUND 0/0 0 lex CSW21;";
  load_cgp_or_die(game, cgp);

  // CGP loader won't accept this impossible rack so we set it manually here.
  rack_set_to_string(game_get_ld(game), rack, "PIZZAQQ");

  generate_moves_for_game(&move_gen_args);
  const Move *move = move_list_get_move(move_list, 0);

  assert_move(game, move_list, NULL, 0, "8D PIZZA 56");
  // Unfound leave of QQ gets 0.0 value.
  assert_move_equity_int(move, 56);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void exchange_tests(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(10);

  const char cgp[300] =
      "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
      "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
      "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  load_cgp_or_die(game, cgp);
  // The top equity plays uses 7 tiles,
  // so exchanges should not be possible.
  play_top_n_equity_move(game, 0);

  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  generate_moves(&move_gen_args);
  SortedMoveList *test_not_an_exchange_sorted_move_list =
      sorted_move_list_create(move_list);
  assert(move_get_type(test_not_an_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  sorted_move_list_destroy(test_not_an_exchange_sorted_move_list);

  load_cgp_or_die(game, cgp);
  // The second top equity play only uses
  // 4 tiles, so exchanges should be the best play.
  play_top_n_equity_move(game, 1);
  generate_moves_for_game(&move_gen_args);
  SortedMoveList *test_exchange_sorted_move_list =
      sorted_move_list_create(move_list);

  assert(move_get_type(test_exchange_sorted_move_list->moves[0]) ==
         GAME_EVENT_EXCHANGE);
  assert_move_score(test_exchange_sorted_move_list->moves[0], 0);
  assert(move_get_tiles_length(test_exchange_sorted_move_list->moves[0]) ==
         move_get_tiles_played(test_exchange_sorted_move_list->moves[0]));
  sorted_move_list_destroy(test_exchange_sorted_move_list);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void movegen_many_moves(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(239000);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  load_cgp_or_die(game, MANY_MOVES);
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 238895);
  assert(count_nonscoring_plays(move_list) == 96);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void equity_test(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(300);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  const KLV *klv = player_get_klv(player);
  // A middlegame is chosen to avoid
  // the opening and endgame equity adjustments
  load_cgp_or_die(game, VS_ED);
  rack_set_to_string(ld, player_get_rack(player), "AFGIIIS");
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 219);
  assert(count_nonscoring_plays(move_list) == 64);

  SortedMoveList *equity_test_sorted_move_list =
      sorted_move_list_create(move_list);

  Equity previous_equity = EQUITY_MAX_VALUE;
  Rack *move_rack = rack_create(ld_size);
  int number_of_moves = equity_test_sorted_move_list->count;

  for (int i = 0; i < number_of_moves - 1; i++) {
    const Move *move = equity_test_sorted_move_list->moves[i];
    assert(move_get_equity(move) <= previous_equity);
    rack_set_to_string(ld, move_rack, "AFGIIIS");
    const Equity leave_value = get_leave_value_for_move(klv, move, move_rack);
    assert(move_get_equity(move) == move_get_score(move) + leave_value);
    previous_equity = move_get_equity(move);
  }
  assert(
      move_get_type(equity_test_sorted_move_list->moves[number_of_moves - 1]) ==
      GAME_EVENT_PASS);

  sorted_move_list_destroy(equity_test_sorted_move_list);
  rack_destroy(move_rack);
  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void top_equity_play_recorder_test(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  player_set_move_record_type(player, MOVE_RECORD_BEST);

  load_cgp_or_die(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "DDESW??");
  generate_moves_for_game(&move_gen_args);

  assert_move(game, move_list, NULL, 0, "14B hEaDW(OR)DS 106");

  rack_reset(player_get_rack(player));

  load_cgp_or_die(game, VS_OXY);
  rack_set_to_string(ld, player_get_rack(player), "ABEOPXZ");
  generate_moves_for_game(&move_gen_args);

  assert_move(game, move_list, NULL, 0, "A1 OX(Y)P(HEN)B(UT)AZ(ON)E 1780");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void small_play_recorder_test(void) {
  // TODO(olaugh): WMP doesn't support small move. Still under analysis whether
  // it would be better than recursive_gen for endgame.
  Config *config =
      config_create_or_die("set -lex NWL20 -s1 score -s2 score -r1 small -r2 "
                           "small -numsmallplays 100000 -wmp false");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create_small(100000);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  player_set_move_record_type(player, MOVE_RECORD_ALL_SMALL);

  load_cgp_or_die(game, VS_JEREMY);
  rack_set_to_string(ld, player_get_rack(player), "DDESW??");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_to_full_rack(game, 1);

  generate_moves_for_game(&move_gen_args);
  int expected_count = 8286; // 8285 scoring moves and 1 pass
  assert(move_list_get_count(move_list) == expected_count);

  // Copy to a temp array by value. The qsort comparator expects SmallMove
  // and not SmallMove*
  SmallMove *temp_small_moves =
      malloc_or_die(expected_count * sizeof(SmallMove));
  for (size_t i = 0; i < (size_t)expected_count; ++i) {
    temp_small_moves[i] = *(move_list->small_moves[i]);
  }

  qsort(temp_small_moves, expected_count, sizeof(SmallMove),
        compare_small_moves_by_score);

  assert(small_move_get_score(&temp_small_moves[0]) == 106); // 14B hEaDW(OR)DS
  assert(small_move_get_score(&temp_small_moves[1]) == 38);  // 14B hEaDW(OR)D

  small_move_to_move(move_list->spare_move, &temp_small_moves[0],
                     game_get_board(game));

  assert(move_list->spare_move->col_start == 1);
  assert(move_list->spare_move->row_start == 13);
  assert(move_list->spare_move->dir == BOARD_HORIZONTAL_DIRECTION);
  assert(move_list->spare_move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert_move_score(move_list->spare_move, 106);
  assert(move_list->spare_move->tiles_length == 9);
  assert(move_list->spare_move->tiles_played == 7);
  assert(memcmp(move_list->spare_move->tiles,
                // h E a D W _ _ D S
                (MachineLetter[]){8 | 0x80, 5, 1 | 0x80, 4, 23, 0, 0, 4, 19},
                9) == 0);

  free(temp_small_moves);
  small_move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void distinct_lexica_test(bool w1) {
  Config *config =
      w1 ? config_create_or_die("set -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
                                "-r1 best -r2 best -numplays 1 -w1 true")
         : config_create_or_die("set -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
                                "-r1 best -r2 best -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = move_list_create(1);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  // Play SPORK, better than best NWL move of PORKS
  rack_set_to_string(ld, player0_rack, "KOPRRSS");
  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "8H SPORK 32");

  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  rack_set_to_string(ld, player1_rack, "CEHIIRZ");
  generate_moves_for_game(&move_gen_args);

  assert_move(game, move_list, NULL, 0, "H8 (S)CHIZIER 146");

  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  rack_set_to_string(ld, player0_rack, "GGLLOWY");
  generate_moves_for_game(&move_gen_args);

  assert_move(game, move_list, NULL, 0, "11G W(I)GGLY 28");

  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  rack_set_to_string(ld, player1_rack, "AEEQRSU");

  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "13C QUEAS(I)ER 88");

  move_list_destroy(move_list);
  game_destroy(game);

  load_and_exec_config_or_die(
      config,
      "set -l2 CSW21 -l1 NWL20 -k2 CSW21 -k1 NWL20 -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1");

  // Ensure loading from CGP correctly sets the distinct cross sets
  Game *game2 = config_game_create(config);

  load_cgp_or_die(
      game2,
      "15/15/15/15/15/15/15/7SPORK3/7C7/7H7/6WIGGLY3/7Z7/7I7/7E7/7R7 / 0/0 0");

  const LetterDistribution *ld2 = game_get_ld(game2);
  MoveList *move_list2 = move_list_create(1);

  player0 = game_get_player(game2, 0);
  player1 = game_get_player(game2, 1);
  player0_rack = player_get_rack(player0);
  player1_rack = player_get_rack(player1);

  rack_set_to_string(ld2, player0_rack, "AEEQRSU");
  move_gen_args.game = game2;
  move_gen_args.move_list = move_list2;
  generate_moves_for_game(&move_gen_args);
  assert_move(game2, move_list2, NULL, 0, "13C QUEAS(I)ER 88");

  game_start_next_player_turn(game2);

  rack_set_to_string(ld2, player1_rack, "AEEQRSU");
  move_gen_args.game = game2;
  move_gen_args.move_list = move_list2;
  generate_moves_for_game(&move_gen_args);
  assert_move(game2, move_list2, NULL, 0, "L3 SQUEA(K)ER(Y) 100");

  move_list_destroy(move_list2);
  game_destroy(game2);

  config_destroy(config);
}

void wordsmog_test(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21_alpha -wmp false -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -var wordsmog");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);

  assert_validated_and_generated_moves(game, "FEEZEEE", "8D", "ZEEFE", 54,
                                       true);
  assert_validated_and_generated_moves(game, "HOWDYES", "7D", "ODHYW", 55,
                                       true);
  assert_validated_and_generated_moves(game, "SIXATAS", "6D", "SIXAT", 88,
                                       true);
  assert_validated_and_generated_moves(game, "OSSTTUU", "E2", "OSST(IDE)TUU",
                                       94, true);
  assert_validated_and_generated_moves(game, "AEGNSUV", "C8", "SVAEGU", 46,
                                       true);
  assert_validated_and_generated_moves(game, "AEHLLOY", "B10", "HLOYL", 61,
                                       true);
  assert_validated_and_generated_moves(game, "BEENROW", "A11", "WBEON", 62,
                                       true);
  assert_validated_and_generated_moves(game, "ADEEIMN", "5G", "DAEEIMN", 88,
                                       true);
  // SPHINXLIKE
  assert_validated_and_generated_moves(game, "PINLIK?", "F2", "KIIL(XHE)sPN",
                                       136, true);
  // CANONIZERS
  assert_validated_and_generated_moves(game, "AEINNR?", "D3", "ANI(SOZ)ERcN",
                                       182, true);

  load_and_exec_config_or_die(config, "set -ld english_blank_is_5");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  game = config_get_game(config);

  assert_validated_and_generated_moves(game, "AEIINO?", "8B", "AEpIINO", 82,
                                       true);
  config_destroy(config);
}

void consistent_tiebreaking_test(void) {
  Config *config =
      config_create_or_die("set -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
                           "-r1 best -r2 best -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = move_list_create(1);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  rack_set_to_string(ld, player0_rack, "EEEFVRR");
  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "8D FEVER 30");

  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Should be NUNcLES instead of NoNFUELS
  rack_set_to_string(ld, player1_rack, "ELNNSU?");
  generate_moves_for_game(&move_gen_args);

  assert_move(game, move_list, NULL, 0, "I2 NUNcLES 70");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void movegen_game_update_test(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1");

  // Check that ld updates and that blanks can be any score
  load_and_exec_config_or_die(config, "set -ld english_blank_is_5");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 E?INAOI/ 0/0 0");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8C EpINAOI 82");

  // Check that bingo bonus udpates
  load_and_exec_config_or_die(config, "set -bb 40");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 BUSUUTI/ 0/0 0");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8D BUSUUTI 64");

  // Check that lexicon updates
  load_and_exec_config_or_die(config, "set -lex NWL20");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 BUSUUTI/ 0/0 0");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "(exch BUUU)");

  // Check that move sorting updates
  load_and_exec_config_or_die(config, "set -s1 score");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8E BITS 12");

  // Check that board layout updates
  load_and_exec_config_or_die(config,
                              "set -bdn quadruple_word_opening15 -bb 50");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DITZIER/ 0/0 0");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8D DITZIER 126");

  // Check that the variant updates
  load_and_exec_config_or_die(
      config, "set -lex CSW21_alpha -wmp false -var wordsmog -bdn standard15");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DITZIER/ 0/0 0");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8B DEZIIRT 104");

  config_destroy(config);
}

void movegen_var_bingo_bonus_test(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  ValidatedMoves *vms = NULL;

  const char *opening_busuuti_cgp =
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 BUSUUTI/ 0/0 0";

  char *opening_busuuti_cgp_cmd =
      get_formatted_string("cgp %s", opening_busuuti_cgp);

  // Check that bingo bonus udpates
  load_and_exec_config_or_die(config, opening_busuuti_cgp_cmd);
  load_and_exec_config_or_die(config, "set -bb 40");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8D BUSUUTI 64");

  vms = assert_validated_move_success(config_get_game(config),
                                      opening_busuuti_cgp, "8D.BUSUUTI", 0,
                                      false, false);
  assert_move_score(validated_moves_get_move(vms, 0), 64);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, opening_busuuti_cgp_cmd);
  load_and_exec_config_or_die(config, "set -bb 30");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8D BUSUUTI 54");

  vms = assert_validated_move_success(config_get_game(config),
                                      opening_busuuti_cgp, "8D.BUSUUTI", 0,
                                      false, false);
  assert_move_score(validated_moves_get_move(vms, 0), 54);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, opening_busuuti_cgp_cmd);
  load_and_exec_config_or_die(config, "set -bb 300");
  load_and_exec_config_or_die(config, "gen");
  assert_move(config_get_game(config), config_get_move_list(config), NULL, 0,
              "8D BUSUUTI 324");

  vms = assert_validated_move_success(config_get_game(config),
                                      opening_busuuti_cgp, "8D.BUSUUTI", 0,
                                      false, false);
  assert_move_score(validated_moves_get_move(vms, 0), 324);
  validated_moves_destroy(vms);

  free(opening_busuuti_cgp_cmd);
  config_destroy(config);
}

void movegen_only_one_player_wmp(void) {
  Config *config = config_create_or_die("set -lex CSW21 -w1 true -w2 false");
  Game *game = config_game_create(config);
  const WMP *wmp1 = player_get_wmp(game_get_player(game, 0));
  assert(wmp1 != NULL);
  assert(wmp1 != (WMP *)0xbebebebebebebebe);
  const WMP *wmp2 = player_get_wmp(game_get_player(game, 1));
  assert(wmp2 == NULL);
  game_destroy(game);
  config_destroy(config);

  config = config_create_or_die("set -lex CSW21 -w1 false -w2 true");
  game = config_game_create(config);
  wmp1 = player_get_wmp(game_get_player(game, 0));
  assert(wmp1 == NULL);
  wmp2 = player_get_wmp(game_get_player(game, 1));
  assert(wmp2 != NULL);
  assert(wmp2 != (WMP *)0xbebebebebebebebe);
  game_destroy(game);
  config_destroy(config);
}

void movegen_within_x_of_best_test(bool use_wmp) {
  char *config_str =
      get_formatted_string("set -lex CSW21 -wmp %s -s1 equity -s2 equity "
                           "-r1 equity -r2 equity "
                           "-numplays 100",
                           use_wmp ? "true" : "false");
  Config *config = config_create_or_die(config_str);
  free(config_str);
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(10000);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };
  int all_move_list_count = 0;
  int move_list_count = 0;
  SortedMoveList *sml = NULL;

  player_set_move_record_type(player, MOVE_RECORD_ALL);
  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  move_gen_args.eq_margin_movegen = int_to_equity(1);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  all_move_list_count = sml->count;
  assert_move(game, NULL, sml, 0, "8D ZILLION 102");
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;
  player_set_move_record_type(player, MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST);

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  move_gen_args.eq_margin_movegen = int_to_equity(1);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "8D ZILLION 102");
  assert(sml->count == 1);
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  move_gen_args.eq_margin_movegen = int_to_equity(15);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "8D ZILLION 102");
  assert(sml->count == 1);
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  move_gen_args.eq_margin_movegen = int_to_equity(25);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert(sml->count == 7);
  assert_move(game, NULL, sml, 0, "8D ZILLION 102");
  for (int i = 0; i < sml->count; i++) {
    const Move *move = sml->moves[i];
    assert(move_get_tiles_played(move) == 7);
  }
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  move_gen_args.eq_margin_movegen = int_to_equity(75);
  generate_moves_for_game(&move_gen_args);
  move_list_count = move_list_get_count(move_list);
  // All placements of ZILLIONS and all placements of ZILL are within 75 equity
  assert(move_list_count == 11);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "8D ZILLION 102");
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ZILLION");
  // All moves should be within 1000 equity of the best move
  // except for the pass
  move_gen_args.eq_margin_movegen = int_to_equity(1000);
  generate_moves_for_game(&move_gen_args);
  assert(all_move_list_count == move_list_get_count(move_list) + 1);
  rack_reset(player_get_rack(player));

  load_cgp_or_die(game, EMPTY_CGP);
  rack_set_to_string(ld, player_get_rack(player), "AUUUUVV");
  move_gen_args.eq_margin_movegen = int_to_equity(10);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "(exch UUUUVV)");
  for (int i = 0; i < sml->count; i++) {
    const Move *move = sml->moves[i];
    assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  }
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, UNPLAYABLE_V_CGP);
  rack_set_to_string(ld, player_get_rack(player), "V");
  // Have the opponent draw any 7 tiles to prevent exchanges
  // from being generated
  draw_to_full_rack(game, 1);
  generate_moves_for_game(&move_gen_args);
  assert(count_scoring_plays(move_list) == 0);
  assert(count_nonscoring_plays(move_list) == 1);
  assert(move_get_type(move_list_get_move(move_list, 0)) == GAME_EVENT_PASS);
  rack_reset(player_get_rack(player));

  load_cgp_or_die(game, PRETZEL_OPENING_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ENTRIES");
  move_gen_args.eq_margin_movegen = int_to_equity(0);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert(sml->count == 6);
  for (int i = 0; i < sml->count; i++) {
    const Move *move = sml->moves[i];
    assert(move_get_score(move) == int_to_equity(131));
  }
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, PRETZEL_OPENING_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ENTRIES");
  move_gen_args.eq_margin_movegen = int_to_equity(48);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert(sml->count == 6);
  for (int i = 0; i < sml->count; i++) {
    const Move *move = sml->moves[i];
    assert(move_get_score(move) == int_to_equity(131));
  }
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  load_cgp_or_die(game, PRETZEL_OPENING_CGP);
  rack_set_to_string(ld, player_get_rack(player), "ENTRIES");
  move_gen_args.eq_margin_movegen = int_to_equity(50);
  generate_moves_for_game(&move_gen_args);
  sml = sorted_move_list_create(move_list);
  assert(sml->count == 8);
  for (int i = 0; i < sml->count; i++) {
    const Move *move = sml->moves[i];
    assert(move_get_tiles_played(move) == 7);
  }
  rack_reset(player_get_rack(player));
  sorted_move_list_destroy(sml);
  sml = NULL;

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void movegen_should_not_gen_exchanges(void) {
  Config *config = config_create_default_test();
  load_and_exec_config_or_die(
      config,
      "set -lex NWL23 -ld english -r2 equity -numplays 40 -mmargin 30 "
      "-sr tt -threads 16 -scond "
      "95 "
      "-s2 equity -plies 5 -minp 100 -thres gk16 -it 1000000 -wmp true -s1 "
      "equity -r1 equity ");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "WAgTAILS5K1/7P4QI1/3NEUTRINO1UN1/JOTA3I3BEDU/I2EFF1N2ZAS1N/"
      "HM5G2AGO1R/AE4REP2G2E/D6SOOTY1TA/ID11EL/1AR10L1/1WO7UNBOX/1TE10M1/"
      "1E11I1/1D10ICY/15 AILORRV/ 303/458 0");

  load_and_exec_config_or_die(config, "gen");

  const MoveList *move_list = config_get_move_list(config);
  const int num_moves = move_list_get_count(move_list);
  for (int i = 0; i < num_moves; i++) {
    const Move *move = move_list_get_move(move_list, i);
    assert(move_get_type(move) != GAME_EVENT_EXCHANGE);
  }

  config_destroy(config);
}

void movegen_does_not_return_early_from_anchor(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(10);
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  // A8 BRRR 72 should be 1 equity better than A8 BRR 69
  set_klv_leave_value(klv, ld, "NNN", 0);
  set_klv_leave_value(klv, ld, "NNNR", int_to_equity(3) - 1);

  load_cgp_or_die(game, RATPACK_CGP);
  rack_set_to_string(ld, player_rack, "BRRRNNN");
  generate_moves(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "A8 BRRR 72");

  // A8 BRR 69 should be 1 equity better than A8 BRRR 72
  set_klv_leave_value(klv, ld, "NNN", 0);
  set_klv_leave_value(klv, ld, "NNNR", int_to_equity(3) + 1);

  load_cgp_or_die(game, RATPACK_CGP);
  rack_set_to_string(ld, player_rack, "BRRRNNN");
  generate_moves(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "A8 BRR 69");

  // A8 BRR 69 is equal to A8 BRRR 72, but we rank them according to canonical
  // move ordering. Optimization to greedily return the first move with top
  // equity has been disabled.
  set_klv_leave_value(klv, ld, "NNN", 0);
  set_klv_leave_value(klv, ld, "NNNR", int_to_equity(3));

  load_cgp_or_die(game, RATPACK_CGP);
  rack_set_to_string(ld, player_rack, "BRRRNNN");
  generate_moves(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "A8 BRRR 72");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void movegen_one_tile_nonwmp(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(10);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  SortedMoveList *sml = NULL;

  load_cgp_or_die(game, QI_QI_CGP);
  rack_set_to_string(ld, player_rack, "F");
  generate_moves(&move_gen_args);
  assert(move_list->count == 3);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "9G (I)F 10");
  assert_move(game, NULL, sml, 1, "(exch F)");
  assert_move(game, NULL, sml, 2, "pass");
  sorted_move_list_destroy(sml);

  load_cgp_or_die(game, IF_IF_CGP);
  rack_set_to_string(ld, player_rack, "Q");
  generate_moves(&move_gen_args);
  assert(move_list->count == 3);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "8G Q(I) 22");
  assert_move(game, NULL, sml, 1, "(exch Q)");
  assert_move(game, NULL, sml, 2, "pass");
  sorted_move_list_destroy(sml);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void movegen_one_tile_wmp(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, 0);
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(10);
  MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,  };

  SortedMoveList *sml = NULL;

  load_cgp_or_die(game, QI_QI_CGP);
  rack_set_to_string(ld, player_rack, "F");
  generate_moves(&move_gen_args);
  assert(move_list->count == 3);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "9G (I)F 10");
  assert_move(game, NULL, sml, 1, "(exch F)");
  assert_move(game, NULL, sml, 2, "pass");
  sorted_move_list_destroy(sml);

  load_cgp_or_die(game, IF_IF_CGP);
  rack_set_to_string(ld, player_rack, "Q");
  generate_moves(&move_gen_args);
  assert(move_list->count == 3);
  sml = sorted_move_list_create(move_list);
  assert_move(game, NULL, sml, 0, "8G Q(I) 22");
  assert_move(game, NULL, sml, 1, "(exch Q)");
  assert_move(game, NULL, sml, 2, "pass");
  sorted_move_list_destroy(sml);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_natural(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));
  gen->wmp_move_gen.word_length = 7;

  // No playthrough tiles. All of these tiles are newly placed.
  set_playthrough_marked_from_string(gen, ld, "NATURAL");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "NATURAL");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  for (int pos = 0; pos < 7; pos++) {
    bool can_be_unblanked = false;
    bool can_be_blanked = false;
    get_blank_possibilities(gen, &nonplay_bitrack, pos, &can_be_unblanked,
                            &can_be_blanked);
    // There are no blanks on the rack, so of course none of the letters can be
    // blanked, and all of them can (must) be unblanked.
    assert(can_be_unblanked);
    assert(!can_be_blanked);
  }

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_blanked(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  // No playthrough tiles. All of these tiles are newly placed.
  set_playthrough_marked_from_string(gen, ld, "BLANKED");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BLANKE?");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  for (int pos = 0; pos < 7; pos++) {
    bool can_be_unblanked = false;
    bool can_be_blanked = false;
    get_blank_possibilities(gen, &nonplay_bitrack, pos, &can_be_unblanked,
                            &can_be_blanked);
    // 'D' is at position 6 in "BLANKED"
    if (pos == 6) {
      assert(!can_be_unblanked);
      assert(can_be_blanked);
    } else {
      assert(can_be_unblanked);
      assert(!can_be_blanked);
    }
  }

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_bananas_1(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  set_playthrough_marked_from_string(gen, ld, "BANANAS");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BANNS??");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  // Check position 1 (the first A): it should be possible to be
  // either unblanked or blanked.
  bool can_be_unblanked = false;
  bool can_be_blanked = false;
  get_blank_possibilities(gen, &nonplay_bitrack, 1, &can_be_unblanked,
                          &can_be_blanked);
  assert(can_be_unblanked);
  assert(can_be_blanked);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_bananas_2(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  set_playthrough_marked_from_string(gen, ld, "BaNANAS");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BANNS??");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  // Check only position 3 (the second A): it should be possible to be
  // either unblanked or blanked. We used one blank for the first A, but we have
  // another.
  bool can_be_unblanked = false;
  bool can_be_blanked = false;
  get_blank_possibilities(gen, &nonplay_bitrack, 3, &can_be_unblanked,
                          &can_be_blanked);
  assert(can_be_unblanked);
  assert(can_be_blanked);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_bananas_3(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  set_playthrough_marked_from_string(gen, ld, "BaNaNAS");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BANNS??");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  // Check only position 5 (the third A): it can only be unblanked, since
  // we've already used both blanks for the first two A's.
  bool can_be_unblanked = false;
  bool can_be_blanked = false;
  get_blank_possibilities(gen, &nonplay_bitrack, 5, &can_be_unblanked,
                          &can_be_blanked);
  assert(can_be_unblanked);
  assert(!can_be_blanked);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_bananas_4(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  set_playthrough_marked_from_string(gen, ld, "B.NaNAS");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BNNS??");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  // Check only position 5 (the third A): only one blank has been used.
  // We must use the second blank for this A to use all our blanks.
  bool can_be_unblanked = false;
  bool can_be_blanked = false;
  get_blank_possibilities(gen, &nonplay_bitrack, 5, &can_be_unblanked,
                          &can_be_blanked);
  assert(!can_be_unblanked);
  assert(can_be_blanked);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void wmp_blank_possibilities_bananas_5(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  MoveGen *gen = get_movegen(0);
  memset(gen, 0, sizeof(*gen));

  set_playthrough_marked_from_string(gen, ld, "BA.ANAS");

  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "BANS??");
  BitRack nonplay_bitrack = bit_rack_create_from_rack(ld, rack);

  // Check only position 3 (the second A): no blanks have been used and we have
  // 2 on our rack. We must make this A blank.
  bool can_be_unblanked = false;
  bool can_be_blanked = false;
  get_blank_possibilities(gen, &nonplay_bitrack, 3, &can_be_unblanked,
                          &can_be_blanked);
  assert(!can_be_unblanked);
  assert(can_be_blanked);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_move_gen(void) {
  leave_lookup_test();
  unfound_leave_lookup_test();
  macondo_tests();
  exchange_tests();
  equity_test();
  top_equity_play_recorder_test();
  small_play_recorder_test();
  distinct_lexica_test(false);
  distinct_lexica_test(true);
  consistent_tiebreaking_test();
  wordsmog_test();
  movegen_game_update_test();
  movegen_var_bingo_bonus_test();
  movegen_only_one_player_wmp();
  movegen_within_x_of_best_test(false);
  movegen_within_x_of_best_test(true);
  movegen_many_moves();
  movegen_should_not_gen_exchanges();
  movegen_does_not_return_early_from_anchor();
  movegen_one_tile_nonwmp();
  movegen_one_tile_wmp();
  wmp_blank_possibilities_natural();
  wmp_blank_possibilities_blanked();
  wmp_blank_possibilities_bananas_1();
  wmp_blank_possibilities_bananas_2();
  wmp_blank_possibilities_bananas_3();
  wmp_blank_possibilities_bananas_4();
  wmp_blank_possibilities_bananas_5();
}

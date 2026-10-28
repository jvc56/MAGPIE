#include "if_only_test.h"

#ifdef IF_ONLY

#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

// Row 8 holds RETAINS from 8D, its S at J8 a fake blank over a Q.
#define IF_ONLY_RETAINS_BOARD                                                  \
  "15/15/15/15/15/15/15/3RETAINs5/15/15/15/15/15/15/15"

static Config *if_only_config_create(void) {
  return config_create_or_die("set -lex CSW21 -var ifonly -s1 equity -s2 "
                              "equity -r1 all -r2 all -numplays 1");
}

static void assert_move_string(const Game *game, const Move *move,
                               const char *expected) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  assert_strings_equal(string_builder_peek(sb), expected);
  string_builder_destroy(sb);
}

static void test_if_only_notation(void) {
  Config *config = if_only_config_create();
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "AEINQRT");

  // The Q played upside down as the S.
  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, 0, "8D RETAINs/Q", false, true, ERROR_STATUS_SUCCESS);
  const Move *move = validated_moves_get_move(vms, 0);
  assert_move_score(move, 64);
  assert(move_get_fake_blank_index(move) == 6);
  assert(move_get_fake_blank_tile(move) == ld_hl_to_ml(ld, "Q"));
  assert_move_string(game, move, "8D RETAINs/Q");
  validated_moves_destroy(vms);

  // A fake blank needs a play of 50 points or more. One that scores less is
  // accepted where phonies are, and is a phony.
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8G QaT/I", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_SCORE_TOO_LOW));
  vms = validated_moves_create_and_assert_status(game, 0, "8G QaT/I", true,
                                                 true, ERROR_STATUS_SUCCESS);
  assert(validated_moves_is_phony(vms, 0));
  validated_moves_destroy(vms);
  vms = validated_moves_create_and_assert_status(
      game, 0, "8D RETAINs/Q", true, true, ERROR_STATUS_SUCCESS);
  assert(!validated_moves_is_phony(vms, 0));
  validated_moves_destroy(vms);
  // An entry for each blank played, and at most one fake blank.
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8D RETAINs/QA", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_INVALID_BLANKS));
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8D RETAiNs/IQ", true, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_INVALID_BLANKS));
  // Every blank placed names its source, and a suffix needs a blank.
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "AENRST?");
  vms = validated_moves_create_and_assert_status(
      game, 0, "8D RETAiNS/?", false, true, ERROR_STATUS_SUCCESS);
  assert(!move_has_fake_blank(validated_moves_get_move(vms, 0)));
  assert_move_string(game, validated_moves_get_move(vms, 0), "8D RETAiNS/?");
  validated_moves_destroy(vms);
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8D RETAiNS", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_INVALID_BLANKS));
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "AEINRST");
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8D RETAINS/", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_INVALID_BLANKS));
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "AEINQRT");
  // There is no fake blank to swap.
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "@H8 8D RETAINs/Q", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_INVALID_SWAP));
  config_destroy(config);

  // A classic game has neither.
  config = config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 "
                                "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  game = config_get_game(config);
  ld = game_get_ld(game);
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "AEINQRT");
  validated_moves_destroy(validated_moves_create_and_assert_status(
      game, 0, "8D RETAINs/Q", false, true,
      ERROR_STATUS_MOVE_VALIDATION_IF_ONLY_NOT_ALLOWED));
  config_destroy(config);
}

static void test_if_only_cgp_and_swap(void) {
  Config *config = if_only_config_create();
  load_and_exec_config_or_die(
      config, "cgp " IF_ONLY_RETAINS_BOARD "/Q ABCDEFS/ 64/0 0");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  assert(board_is_fake_blank(board, 7, 9));
  assert(board_get_fake_blank_tile(board, 7, 9) == ld_hl_to_ml(ld, "Q"));
  char *cgp = game_get_cgp(game, false);
  assert_strings_equal(cgp, IF_ONLY_RETAINS_BOARD "/Q ABCDEFS/ 64/0 0");
  free(cgp);

  // Swapping the S onto the board gives the rack the Q for FEEDBACk.
  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, 0, "@J8 E6 FEEDBACk/Q", false, true, ERROR_STATUS_SUCCESS);
  const Move *move = validated_moves_get_move(vms, 0);
  assert(move_has_swap(move));
  assert(move_get_swap_row(move) == 7 && move_get_swap_col(move) == 9);
  assert(move_get_swap_tile(move) == ld_hl_to_ml(ld, "Q"));
  assert_move_string(game, move, "@J8 E6 FE(E)DBACk/Q");
  play_move(move, game, NULL);
  validated_moves_destroy(vms);
  assert(!board_is_fake_blank(board, 7, 9));
  assert(board_get_letter(board, 7, 9) == ld_hl_to_ml(ld, "S"));
  assert(board_is_fake_blank(board, 12, 4));
  cgp = game_get_cgp(game, false);
  assert(strstr(cgp, "/Q ") != NULL);
  free(cgp);

  // A swap stands when the play after it comes off: here, the C played
  // upside down in a play under 50.
  load_and_exec_config_or_die(
      config, "cgp " IF_ONLY_RETAINS_BOARD "/Q ABCDEFS/ 64/0 0");
  game = config_get_game(config);
  board = game_get_board(game);
  vms = validated_moves_create_and_assert_status(
      game, 0, "@J8 9C bAD/C", true, true, ERROR_STATUS_SUCCESS);
  move = validated_moves_get_move(vms, 0);
  assert(validated_moves_is_phony(vms, 0));
  game_set_backup_mode(game, BACKUP_MODE_GCG);
  play_move(move, game, NULL);
  return_phony_letters(game, move);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  validated_moves_destroy(vms);
  assert(board_get_letter(board, 7, 9) == ld_hl_to_ml(ld, "S"));
  assert(!board_is_fake_blank(board, 7, 9));
  assert(board_get_letter(board, 8, 2) == ALPHABET_EMPTY_SQUARE_MARKER);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "ABCDEFQ");
  assert(player_get_score(game_get_player(game, 0)) == int_to_equity(64));
  config_destroy(config);
}

// Every generated move, written out and validated again, is the same move
// with the same score, and a best-move generation finds the best of them.
static void assert_generation_matches_validation(Game *game, int *flips,
                                                 int *swaps) {
  MoveList *move_list = move_list_create(10000);
  const MoveGenArgs all_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&all_args);
  const int player_index = game_get_player_on_turn_index(game);
  Equity best_equity = EQUITY_MIN_VALUE;
  for (int i = 0; i < move_list_get_count(move_list); i++) {
    const Move *move = move_list_get_move(move_list, i);
    if (move_get_equity(move) > best_equity) {
      best_equity = move_get_equity(move);
    }
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_has_fake_blank(move)) {
      assert(move_get_score(move) >=
             int_to_equity(IF_ONLY_FAKE_BLANK_MIN_SCORE));
      (*flips)++;
    }
    if (move_has_swap(move)) {
      (*swaps)++;
    }
    StringBuilder *sb = string_builder_create();
    string_builder_add_ucgi_move(sb, move, game_get_board(game),
                                 game_get_ld(game));
    ValidatedMoves *vms = validated_moves_create_and_assert_status(
        game, player_index, string_builder_peek(sb), false, true,
        ERROR_STATUS_SUCCESS);
    const Move *validated = validated_moves_get_move(vms, 0);
    if (compare_moves_without_equity(move, validated, true) != -1) {
      printf("generated and validated moves differ: %s\n",
             string_builder_peek(sb));
      assert(0);
    }
    validated_moves_destroy(vms);
    string_builder_destroy(sb);
  }

  MoveList *best_list = move_list_create(1);
  const MoveGenArgs best_args = {
      .game = game,
      .move_list = best_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&best_args);
  const Move *best_move = move_list_get_move(best_list, 0);
  assert(move_get_equity(best_move) == best_equity);
  // The best move is a move of the position, not one from another pass.
  StringBuilder *best_sb = string_builder_create();
  string_builder_add_ucgi_move(best_sb, best_move, game_get_board(game),
                               game_get_ld(game));
  ValidatedMoves *best_vms = validated_moves_create_and_assert_status(
      game, player_index, string_builder_peek(best_sb), false, true,
      ERROR_STATUS_SUCCESS);
  assert(compare_moves_without_equity(
             best_move, validated_moves_get_move(best_vms, 0), true) == -1);
  validated_moves_destroy(best_vms);
  string_builder_destroy(best_sb);
  move_list_destroy(best_list);
  move_list_destroy(move_list);
}

// A flip play that places two blanks is generated with each as the fake one.
static void test_if_only_two_blank_flips(void) {
  Config *config = if_only_config_create();
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  Game *game = config_get_game(config);
  rack_set_to_string(game_get_ld(game),
                     player_get_rack(game_get_player(game, 0)), "AEINRT?");
  MoveList *move_list = move_list_create(100000);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  bool found_first = false;
  bool found_second = false;
  for (int i = 0; i < move_list_get_count(move_list); i++) {
    const Move *move = move_list_get_move(move_list, i);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        !move_has_fake_blank(move)) {
      continue;
    }
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                            false);
    if (strings_equal(string_builder_peek(sb), "8D AcETINs/R?")) {
      found_first = true;
    }
    if (strings_equal(string_builder_peek(sb), "8D AcETINs/?R")) {
      found_second = true;
    }
    string_builder_destroy(sb);
  }
  assert(found_first && found_second);
  move_list_destroy(move_list);
  config_destroy(config);
}

static void test_if_only_generation(void) {
  Config *config = if_only_config_create();
  int flips = 0;
  int swaps = 0;
  for (int game_idx = 0; game_idx < 4; game_idx++) {
    load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
    Game *game = config_get_game(config);
    game_seed(game, 100 + game_idx);
    draw_to_full_rack(game, 0);
    draw_to_full_rack(game, 1);
    while (!game_over(game)) {
      assert_generation_matches_validation(game, &flips, &swaps);
      play_top_n_equity_move(game, 0);
    }
  }
  assert(flips > 0);
  assert(swaps > 0);
  config_destroy(config);
}

void test_if_only(void) {
  test_if_only_notation();
  test_if_only_cgp_and_swap();
  test_if_only_two_blank_flips();
  test_if_only_generation();
}

#else

void test_if_only(void) {}

#endif

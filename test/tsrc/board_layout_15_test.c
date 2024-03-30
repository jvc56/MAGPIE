#include <assert.h>

#include "../../src/ent/board.h"
#include "../../src/ent/board_layout.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/move.h"
#include "../../src/ent/validated_move.h"

#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "test_util.h"

BoardLayout *create_test_board_layout(const char *board_layout_filename) {
  BoardLayout *bl = board_layout_create();
  char *board_layout_filepath =
      get_formatted_string("test/testdata/%s", board_layout_filename);
  board_layout_load_status_t actual_status =
      board_layout_load(bl, board_layout_filepath);
  free(board_layout_filepath);
  if (actual_status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
    printf("board layout load failure: %d", actual_status);
  }
  assert(actual_status == BOARD_LAYOUT_LOAD_STATUS_SUCCESS);
  return bl;
}

void load_game_with_test_board(Game *game, const char *board_layout_filename) {
  printf("loading %s\n", board_layout_filename);
  game_reset(game);
  BoardLayout *bl = create_test_board_layout(board_layout_filename);
  board_apply_layout(bl, game_get_board(game));
  board_layout_destroy(bl);
}

char *remove_parentheses(const char *input) {
  int len = strlen(input);
  char *result = (char *)malloc_or_die((len + 1) * sizeof(char));
  int j = 0;
  for (int i = 0; i < len; i++) {
    if (input[i] != '(' && input[i] != ')') {
      result[j++] = input[i];
    }
  }
  result[j] = '\0';
  return result;
}

void assert_validated_and_generated_moves(Game *game, const char *rack_string,
                                          const char *move_position,
                                          const char *move_tiles,
                                          int move_score,
                                          bool play_move_on_board) {
  Player *player = game_get_player(game, game_get_player_on_turn_index(game));
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1);

  rack_set_to_string(game_get_ld(game), player_rack, rack_string);

  generate_moves_for_game(game, 0, move_list);
  char *gen_move_string;
  if (strings_equal(move_position, "exch")) {
    gen_move_string = get_formatted_string("(exch %s)", move_tiles);
  } else {
    gen_move_string =
        get_formatted_string("%s %s %d", move_position, move_tiles, move_score);
  }
  assert_move(game, move_list, NULL, 0, gen_move_string);
  free(gen_move_string);

  char *move_tiles_no_parens = remove_parentheses(move_tiles);
  char *vm_move_string;
  if (strings_equal(move_position, "exch")) {
    vm_move_string = get_formatted_string("ex.%s", move_tiles_no_parens);
  } else {
    vm_move_string =
        get_formatted_string("%s.%s", move_position, move_tiles_no_parens);
  }
  free(move_tiles_no_parens);

  ValidatedMoves *vms =
      validated_moves_create(game, 0, vm_move_string, false, true);
  free(vm_move_string);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  if (play_move_on_board) {
    play_move(move_list_get_move(move_list, 0), game);
  }

  validated_moves_destroy(vms);
  move_list_destroy(move_list);
}

void test_board_layout_15() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(!board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "asymmetric15.txt");

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "3_2_start_coords_15.txt");

  assert(board_get_anchor(board, 3, 2, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 3, 2, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "quadruple_word_opening15.txt");

  assert_validated_and_generated_moves(game, "QUONKES", "8D", "QUONK", 112,
                                       false);

  load_game_with_test_board(game, "quadruple_letter_opening15.txt");

  assert_validated_and_generated_moves(game, "TALAQES", "8H", "TALAQ", 88,
                                       false);

  load_game_with_test_board(game, "3_2_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "AGUIZED", "C3", "AGUIZED", 110,
                                       false);

  load_game_with_test_board(game, "0_0_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "QUIZEST", "1A", "QUIZ", 96,
                                       false);

  load_game_with_test_board(game, "0_14_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PUCKEST", "O1", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "1L", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, "14_0_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PUCKEST", "15A", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "A12", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, "14_14_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PIGGIEI", "15J", "PIGGIE", 36,
                                       false);

  // Test bricks
  load_game_with_test_board(game, "start_is_bricked_15.txt");

  // No tile placement moves have been generated since the start is bricked,
  // therefore no placement of BUSUUTI is valid in this position.
  assert_validated_and_generated_moves(game, "BUSUUTI", "exch", "BUUU", 0,
                                       false);

  load_game_with_test_board(game, "8D_is_bricked_15.txt");

  // Best move is at H4 because:
  // - the double letter square at 8D is bricked
  // - the board is no longer symmetrical about the diagonal, so vertical
  //   plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "H4", "QUONK", 56,
                                       false);

  load_game_with_test_board(game, "8G_and_7H_are_bricked_15.txt");

  // Best move is at 8H because:
  // - the square at 8G and 7H are bricked preventing access
  //   to the 8D and 4H double letter squares.
  // - the board is symmetrical about the diagonal, so only
  //   horizontal plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "8H", "QUONK", 46,
                                       false);

  load_game_with_test_board(game, "5_by_5_bricked_box_15.txt");

  assert_validated_and_generated_moves(game, "FRAWZEY", "8F", "AWFY", 26, true);

  assert_validated_and_generated_moves(game, "BINGERS", "7F", "BEING", 31,
                                       true);
  assert_validated_and_generated_moves(game, "NARASES", "6F", "ANSA", 31, true);
  assert_validated_and_generated_moves(game, "KARATES", "F6", "(ABA)KA", 13,
                                       true);
  assert_validated_and_generated_moves(game, "CRETINS", "H6", "(SIF)T", 7,
                                       true);
  assert_validated_and_generated_moves(game, "SEXTAIN", "10H", "SAX", 34, true);
  assert_validated_and_generated_moves(game, "ENTREES", "J9", "E(X)", 9, true);
  // There are no more legal plays available other than exchanging.
  assert_validated_and_generated_moves(game, "AEINT??", "exch", "AT", 0, true);

  load_game_with_test_board(game, "single_row_15.txt");
  assert_validated_and_generated_moves(game, "KGOTLAT", "8D", "KGOTLA", 32,
                                       true);
  // The only legal play is extending KGOTLA to LEKGOTLA
  assert_validated_and_generated_moves(game, "RATLINE", "8B", "LE(KGOTLA)", 13,
                                       true);
  // Validate play over block
  load_game_with_test_board(game, "5_by_5_bricked_box_15.txt");

  ValidatedMoves *vms =
      validated_moves_create(game, 0, "8H.FRAWZEY", false, false);
  move_validation_status_t vms_error_status =
      validated_moves_get_validation_status(vms);
  assert(vms_error_status == MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}
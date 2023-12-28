#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "../src/def/cross_set_defs.h"

#include "../src/ent/config.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"

#include "../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

// this test func only works for single-char alphabets
uint64_t cross_set_from_string(const LetterDistribution *letter_distribution,
                               const char *letters) {
  if (strings_equal(letters, "TRIVIAL")) {
    return TRIVIAL_CROSS_SET;
  }
  uint64_t c = 0;
  char letter[2];
  letter[1] = '\0';

  for (size_t i = 0; i < string_length(letters); i++) {
    letter[0] = letters[i];
    set_cross_set_letter(&c, hl_to_ml(letter_distribution, letter));
  }
  return c;
}

// This test function only works for single-char alphabets
void set_col(Game *game, int col, const char *col_content) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  for (int i = 0; i < BOARD_DIM; i++) {
    set_letter(board, i, col, ALPHABET_EMPTY_SQUARE_MARKER);
  }

  char letter[2];
  letter[1] = '\0';

  for (size_t i = 0; i < string_length(col_content); i++) {
    if (col_content[i] != ' ') {
      letter[0] = col_content[i];
      set_letter(board, i, col, hl_to_ml(ld, letter));
      incrememt_tiles_played(board, 1);
    }
  }
}

void test_gen_cross_set(Game *game, int row, int col, int dir, int player_index,
                        const char *letters, int expected_cross_score,
                        bool run_gcs) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  int cross_set_index = get_cross_set_index(false, player_index);
  if (run_gcs) {
    gen_cross_set(player_get_kwg(game_get_player(game, player_index)), ld,
                  board, row, col, dir, cross_set_index);
  }
  uint64_t expected_cross_set = cross_set_from_string(ld, letters);
  uint64_t actual_cross_set =
      get_cross_set(board, row, col, dir, cross_set_index);
  assert(expected_cross_set == actual_cross_set);
  int actual_cross_score =
      get_cross_score(board, row, col, dir, cross_set_index);
  assert(expected_cross_score == actual_cross_score);
}

void test_gen_cross_set_row(Game *game, int row, int col, int dir,
                            int player_index, const char *row_content,
                            const char *letters, int expected_cross_score,
                            bool run_gcs) {
  set_row(game, row, row_content);
  test_gen_cross_set(game, row, col, dir, player_index, letters,
                     expected_cross_score, run_gcs);
}

void test_gen_cross_set_col(Game *game, int row, int col, int dir,
                            int player_index, const char *col_content,
                            const char *letters, int expected_cross_score,
                            bool run_gcs) {
  set_col(game, col, col_content);
  test_gen_cross_set(game, row, col, dir, player_index, letters,
                     expected_cross_score, run_gcs);
}

void test_cross_set() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = create_game(config);
  Player *player0 = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player0);

  // TestGencross_setLoadedGame
  load_cgp(game, VS_MATT);
  test_gen_cross_set(game, 10, 10, BOARD_HORIZONTAL_DIRECTION, 0, "E", 11,
                     true);
  test_gen_cross_set(game, 2, 4, BOARD_HORIZONTAL_DIRECTION, 0, "DHKLRSV", 9,
                     true);
  test_gen_cross_set(game, 8, 7, BOARD_HORIZONTAL_DIRECTION, 0, "S", 11, true);
  test_gen_cross_set(game, 12, 8, BOARD_HORIZONTAL_DIRECTION, 0, "", 11, true);
  test_gen_cross_set(game, 3, 1, BOARD_HORIZONTAL_DIRECTION, 0, "", 10, true);
  test_gen_cross_set(game, 6, 8, BOARD_HORIZONTAL_DIRECTION, 0, "", 5, true);
  test_gen_cross_set(game, 2, 10, BOARD_HORIZONTAL_DIRECTION, 0, "M", 2, true);

  // TestGencross_setEdges
  reset_game(game);
  test_gen_cross_set_row(game, 4, 0, 0, 0, " A", "ABDFHKLMNPTYZ", 1, true);
  test_gen_cross_set_row(game, 4, 1, 0, 0, "A", "ABDEGHILMNRSTWXY", 1, true);
  test_gen_cross_set_row(game, 4, 13, 0, 0, "              F", "EIO", 4, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "             F ", "AE", 4, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "          WECH ", "T", 12, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "           ZZZ ", "", 30, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "       ZYZZYVA ", "S", 43, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "        ZYZZYV ", "A", 42, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "       Z Z Y A ",
                         "ABDEGHILMNRSTWXY", 1, true);
  test_gen_cross_set_row(game, 4, 12, 0, 0, "       z z Y A ", "E", 5, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "OxYpHeNbUTAzON ", "E", 15, true);
  test_gen_cross_set_row(game, 4, 6, 0, 0, "OXYPHE BUTAZONE", "N", 40, true);
  test_gen_cross_set_row(game, 4, 0, 0, 0, " YHJKTKHKTLV", "", 42, true);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "   YHJKTKHKTLV ", "", 42, true);
  test_gen_cross_set_row(game, 4, 6, 0, 0, "YHJKTK HKTLV", "", 42, true);

  // Test setting cross sets with tiles on either side
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NATURES", "E", 9, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D N", "AEIOU", 3, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NT", "EIU", 4, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NTS", "EIU", 5, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "R VOTED", "E", 10, true);
  test_gen_cross_set_row(game, 4, 5, 1, 0, "PHENY BUTAZONE", "L", 32, true);
  test_gen_cross_set_row(game, 4, 6, 1, 0, "OXYPHE BUTAZONE", "N", 40, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "R XED", "A", 12, true);
  test_gen_cross_set_row(game, 4, 2, 1, 0, "BA ED", "AKLNRSTY", 7, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "X Z", "", 18, true);
  test_gen_cross_set_row(game, 4, 6, 1, 0, "STRONG L", "Y", 8, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "W SIWYG", "Y", 16, true);
  test_gen_cross_set_row(game, 4, 0, 1, 0, " EMSTVO", "Z", 11, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "T UNFOLD", "", 11, true);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "S OBCONIc", "", 11, true);

  // TestGenAllcross_sets
  load_cgp(game, VS_ED);
  test_gen_cross_set(game, 8, 8, BOARD_HORIZONTAL_DIRECTION, 0, "OS", 8, false);
  test_gen_cross_set(game, 8, 8, BOARD_VERTICAL_DIRECTION, 0, "S", 9, false);
  test_gen_cross_set(game, 5, 11, BOARD_HORIZONTAL_DIRECTION, 0, "S", 5, false);
  test_gen_cross_set(game, 5, 11, BOARD_VERTICAL_DIRECTION, 0, "AO", 2, false);
  test_gen_cross_set(game, 8, 13, BOARD_HORIZONTAL_DIRECTION, 0, "AEOU", 1,
                     false);
  test_gen_cross_set(game, 8, 13, BOARD_VERTICAL_DIRECTION, 0, "AEIMOUY", 3,
                     false);
  test_gen_cross_set(game, 9, 13, BOARD_HORIZONTAL_DIRECTION, 0, "HMNPST", 1,
                     false);
  test_gen_cross_set(game, 9, 13, BOARD_VERTICAL_DIRECTION, 0, "TRIVIAL", 0,
                     false);
  test_gen_cross_set(game, 14, 14, BOARD_HORIZONTAL_DIRECTION, 0, "TRIVIAL", 0,
                     false);
  test_gen_cross_set(game, 14, 14, BOARD_VERTICAL_DIRECTION, 0, "TRIVIAL", 0,
                     false);
  test_gen_cross_set(game, 12, 12, BOARD_HORIZONTAL_DIRECTION, 0, "", 0, false);
  test_gen_cross_set(game, 12, 12, BOARD_VERTICAL_DIRECTION, 0, "", 0, false);

  // TestUpdateSinglecross_set
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  load_cgp(game, VS_MATT);
  set_letter(board, 8, 10, 19);
  set_letter(board, 9, 10, 0);
  set_letter(board, 10, 10, 4);
  set_letter(board, 11, 10, 11);
  gen_cross_set(kwg, ld, board, 7, 10, BOARD_HORIZONTAL_DIRECTION, 0);
  transpose(board);
  gen_cross_set(kwg, ld, board, 10, 7, BOARD_VERTICAL_DIRECTION, 0);
  transpose(board);
  assert(get_cross_set(board, 7, 10, BOARD_HORIZONTAL_DIRECTION, 0) == 0);
  assert(get_cross_set(board, 7, 10, BOARD_VERTICAL_DIRECTION, 0) == 0);

  destroy_game(game);
  destroy_config(config);
}
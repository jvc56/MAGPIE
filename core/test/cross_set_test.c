#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "../src/config.h"
#include "../src/cross_set.h"
#include "../src/game.h"
#include "../src/letter_distribution.h"

#include "superconfig.h"
#include "test_constants.h"
#include "test_util.h"

// this test func only works for single-char alphabets
uint64_t cross_set_from_string(const char *letters,
                               LetterDistribution *letter_distribution) {
  if (strings_equal(letters, "TRIVIAL")) {
    return TRIVIAL_CROSS_SET;
  }
  uint64_t c = 0;
  char letter[2];
  letter[1] = '\0';

  for (size_t i = 0; i < string_length(letters); i++) {
    letter[0] = letters[i];
    set_cross_set_letter(&c, human_readable_letter_to_machine_letter(
                                 letter_distribution, letter));
  }
  return c;
}

// This test function only works for single-char alphabets
void set_row(Game *game, int row, const char *row_content) {
  for (int i = 0; i < BOARD_DIM; i++) {
    set_letter(game->gen->board, row, i, ALPHABET_EMPTY_SQUARE_MARKER);
  }
  char letter[2];
  letter[1] = '\0';
  for (size_t i = 0; i < string_length(row_content); i++) {
    if (row_content[i] != ' ') {
      letter[0] = row_content[i];
      set_letter(game->gen->board, row, i,
                 human_readable_letter_to_machine_letter(
                     game->gen->letter_distribution, letter));
      game->gen->board->tiles_played++;
    }
  }
}

// This test function only works for single-char alphabets
void set_col(Game *game, int col, const char *col_content) {
  for (int i = 0; i < BOARD_DIM; i++) {
    set_letter(game->gen->board, i, col, ALPHABET_EMPTY_SQUARE_MARKER);
  }

  char letter[2];
  letter[1] = '\0';

  for (size_t i = 0; i < string_length(col_content); i++) {
    if (col_content[i] != ' ') {
      letter[0] = col_content[i];
      set_letter(game->gen->board, i, col,
                 human_readable_letter_to_machine_letter(
                     game->gen->letter_distribution, letter));
      game->gen->board->tiles_played++;
    }
  }
}

void test_gen_cross_set(Game *game, int row, int col, int dir, int player_index,
                        const char *letters, int expected_cross_score,
                        int run_gcs) {
  int cross_set_index = get_cross_set_index(game->gen, player_index);
  if (run_gcs) {
    gen_cross_set(game->gen->board, row, col, dir, cross_set_index,
                  game->players[player_index]->strategy_params->kwg,
                  game->gen->letter_distribution);
  }
  uint64_t expected_cross_set =
      cross_set_from_string(letters, game->gen->letter_distribution);
  uint64_t actual_cross_set =
      get_cross_set(game->gen->board, row, col, dir, cross_set_index);
  assert(expected_cross_set == actual_cross_set);
  int actual_cross_score =
      get_cross_score(game->gen->board, row, col, dir, cross_set_index);
  assert(expected_cross_score == actual_cross_score);
}

void test_gen_cross_set_row(Game *game, int row, int col, int dir,
                            int player_index, const char *row_content,
                            const char *letters, int expected_cross_score,
                            int run_gcs) {
  set_row(game, row, row_content);
  test_gen_cross_set(game, row, col, dir, player_index, letters,
                     expected_cross_score, run_gcs);
}

void test_gen_cross_set_col(Game *game, int row, int col, int dir,
                            int player_index, const char *col_content,
                            const char *letters, int expected_cross_score,
                            int run_gcs) {
  set_col(game, col, col_content);
  test_gen_cross_set(game, row, col, dir, player_index, letters,
                     expected_cross_score, run_gcs);
}

void test_cross_set(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);
  Game *game = create_game(config);
  KWG *kwg = game->players[0]->strategy_params->kwg;

  // TestGencross_setLoadedGame
  load_cgp(game, VS_MATT);
  test_gen_cross_set(game, 10, 10, BOARD_HORIZONTAL_DIRECTION, 0, "E", 11, 1);
  test_gen_cross_set(game, 2, 4, BOARD_HORIZONTAL_DIRECTION, 0, "DHKLRSV", 9,
                     1);
  test_gen_cross_set(game, 8, 7, BOARD_HORIZONTAL_DIRECTION, 0, "S", 11, 1);
  test_gen_cross_set(game, 12, 8, BOARD_HORIZONTAL_DIRECTION, 0, "", 11, 1);
  test_gen_cross_set(game, 3, 1, BOARD_HORIZONTAL_DIRECTION, 0, "", 10, 1);
  test_gen_cross_set(game, 6, 8, BOARD_HORIZONTAL_DIRECTION, 0, "", 5, 1);
  test_gen_cross_set(game, 2, 10, BOARD_HORIZONTAL_DIRECTION, 0, "M", 2, 1);

  // TestGencross_setEdges
  reset_game(game);
  test_gen_cross_set_row(game, 4, 0, 0, 0, " A", "ABDFHKLMNPTYZ", 1, 1);
  test_gen_cross_set_row(game, 4, 1, 0, 0, "A", "ABDEGHILMNRSTWXY", 1, 1);
  test_gen_cross_set_row(game, 4, 13, 0, 0, "              F", "EIO", 4, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "             F ", "AE", 4, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "          WECH ", "T", 12, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "           ZZZ ", "", 30, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "       ZYZZYVA ", "S", 43, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "        ZYZZYV ", "A", 42, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "       Z Z Y A ",
                         "ABDEGHILMNRSTWXY", 1, 1);
  test_gen_cross_set_row(game, 4, 12, 0, 0, "       z z Y A ", "E", 5, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "OxYpHeNbUTAzON ", "E", 15, 1);
  test_gen_cross_set_row(game, 4, 6, 0, 0, "OXYPHE BUTAZONE", "N", 40, 1);
  test_gen_cross_set_row(game, 4, 0, 0, 0, " YHJKTKHKTLV", "", 42, 1);
  test_gen_cross_set_row(game, 4, 14, 0, 0, "   YHJKTKHKTLV ", "", 42, 1);
  test_gen_cross_set_row(game, 4, 6, 0, 0, "YHJKTK HKTLV", "", 42, 1);

  // Test setting cross sets with tiles on either side
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NATURES", "E", 9, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D N", "AEIOU", 3, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NT", "EIU", 4, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "D NTS", "EIU", 5, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "R VOTED", "E", 10, 1);
  test_gen_cross_set_row(game, 4, 5, 1, 0, "PHENY BUTAZONE", "L", 32, 1);
  test_gen_cross_set_row(game, 4, 6, 1, 0, "OXYPHE BUTAZONE", "N", 40, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "R XED", "A", 12, 1);
  test_gen_cross_set_row(game, 4, 2, 1, 0, "BA ED", "AKLNRSTY", 7, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "X Z", "", 18, 1);
  test_gen_cross_set_row(game, 4, 6, 1, 0, "STRONG L", "Y", 8, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "W SIWYG", "Y", 16, 1);
  test_gen_cross_set_row(game, 4, 0, 1, 0, " EMSTVO", "Z", 11, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "T UNFOLD", "", 11, 1);
  test_gen_cross_set_row(game, 4, 1, 1, 0, "S OBCONIc", "", 11, 1);

  // TestGenAllcross_sets
  reset_game(game);
  load_cgp(game, VS_ED);
  test_gen_cross_set(game, 8, 8, BOARD_HORIZONTAL_DIRECTION, 0, "OS", 8, 0);
  test_gen_cross_set(game, 8, 8, BOARD_VERTICAL_DIRECTION, 0, "S", 9, 0);
  test_gen_cross_set(game, 5, 11, BOARD_HORIZONTAL_DIRECTION, 0, "S", 5, 0);
  test_gen_cross_set(game, 5, 11, BOARD_VERTICAL_DIRECTION, 0, "AO", 2, 0);
  test_gen_cross_set(game, 8, 13, BOARD_HORIZONTAL_DIRECTION, 0, "AEOU", 1, 0);
  test_gen_cross_set(game, 8, 13, BOARD_VERTICAL_DIRECTION, 0, "AEIMOUY", 3, 0);
  test_gen_cross_set(game, 9, 13, BOARD_HORIZONTAL_DIRECTION, 0, "HMNPST", 1,
                     0);
  test_gen_cross_set(game, 9, 13, BOARD_VERTICAL_DIRECTION, 0, "TRIVIAL", 0, 0);
  test_gen_cross_set(game, 14, 14, BOARD_HORIZONTAL_DIRECTION, 0, "TRIVIAL", 0,
                     0);
  test_gen_cross_set(game, 14, 14, BOARD_VERTICAL_DIRECTION, 0, "TRIVIAL", 0,
                     0);
  test_gen_cross_set(game, 12, 12, BOARD_HORIZONTAL_DIRECTION, 0, "", 0, 0);
  test_gen_cross_set(game, 12, 12, BOARD_VERTICAL_DIRECTION, 0, "", 0, 0);

  // TestUpdateSinglecross_set
  reset_game(game);
  load_cgp(game, VS_MATT);
  set_letter(game->gen->board, 8, 10, 19);
  set_letter(game->gen->board, 9, 10, 0);
  set_letter(game->gen->board, 10, 10, 4);
  set_letter(game->gen->board, 11, 10, 11);
  gen_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION, 0, kwg,
                game->gen->letter_distribution);
  transpose(game->gen->board);
  gen_cross_set(game->gen->board, 10, 7, BOARD_VERTICAL_DIRECTION, 0, kwg,
                game->gen->letter_distribution);
  transpose(game->gen->board);
  assert(get_cross_set(game->gen->board, 7, 10, BOARD_HORIZONTAL_DIRECTION,
                       0) == 0);
  assert(get_cross_set(game->gen->board, 7, 10, BOARD_VERTICAL_DIRECTION, 0) ==
         0);

  destroy_game(game);
}
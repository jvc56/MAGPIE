#include <assert.h>

#include "../../src/def/board_defs.h"

#include "../../src/ent/board.h"

#include "../../src/ent/board.h"
#include "../../src/ent/equity.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"

#include "test_constants.h"
#include "test_util.h"

#include "../../src/str/game_string.h"
#include "../../src/str/rack_string.h"

#define BOTH_CI (-1)

void assert_unusable_spot(const Game *game, int row, int col, int dir,
                          int tiles, int ci) {
  const Board *board = game_get_board(game);
  if (ci == BOTH_CI) {
    assert_unusable_spot(game, row, col, dir, tiles, 0);
    assert_unusable_spot(game, row, col, dir, tiles, 1);
    return;
  }
  const BoardSpot *spot =
      board_get_readonly_spot(board, row, col, dir, tiles, ci);
  assert(!spot->is_usable);
}

void assert_usable_spot_aux(const Game *game, int row, int col, int dir,
                            int tiles, int ci, const char *playthrough,
                            const uint8_t multipliers[WORD_ALIGNING_RACK_SIZE],
                            int additional_score, int word_length) {
  const Board *board = game_get_board(game);
  const BoardSpot *spot =
      board_get_readonly_spot(board, row, col, dir, tiles, ci);
  assert(spot->is_usable);

  Rack *playthrough_bit_rack = bit_rack_to_rack(&spot->playthrough_bit_rack);
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, playthrough_bit_rack, game_get_ld(game), false);
  assert(strcmp(string_builder_peek(sb), playthrough) == 0);
  string_builder_destroy(sb);

  assert(spot->word_length == word_length);
  for (int i = 0; i < WORD_ALIGNING_RACK_SIZE; i++) {
    assert(spot->descending_effective_multipliers[i] == multipliers[i]);
  }
  assert(spot->additional_score == int_to_equity(additional_score));

  rack_destroy(playthrough_bit_rack);
}

void assert_usable_spot(const Game *game, int row, int col, int dir, int tiles,
                        int ci, const char *playthrough,
                        const uint8_t multipliers[WORD_ALIGNING_RACK_SIZE],
                        int additional_score, int word_length) {
  if (ci == BOTH_CI) {
    assert_usable_spot(game, row, col, dir, tiles, 0, playthrough, multipliers,
                       additional_score, word_length);
    assert_usable_spot(game, row, col, dir, tiles, 1, playthrough, multipliers,
                       additional_score, word_length);
    return;
  }
  assert_usable_spot_aux(game, row, col, dir, tiles, ci, playthrough,
                         multipliers, additional_score, word_length);
}

void test_standard_empty_board(void) {
  Config *config = config_create_or_die("set -lex NWL20 -wmp true");
  Game *game = config_game_create(config);
  Board *board = game_get_board(game);
  const int starting_row = board->start_coords[0];
  const int starting_col = board->start_coords[1];
  assert(starting_row == starting_col);
  assert(game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG));
  assert(game_get_data_is_shared(game, PLAYERS_DATA_TYPE_WMP));
  assert(board_are_bonus_squares_symmetric_by_transposition(board));
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        int needed_to_reach_start = BOARD_DIM + 1;
        if (dir == BOARD_HORIZONTAL_DIRECTION && row == starting_row &&
            col <= starting_col) {
          needed_to_reach_start = starting_col - col + 1;
        }
        for (int tiles = 1; tiles < needed_to_reach_start; tiles++) {
          if (tiles > RACK_SIZE) {
            break;
          }
          const BoardSpot *spot =
              board_get_readonly_spot(board, row, col, dir, tiles, 0);
          assert(!spot->is_usable);
        }
        for (int tiles = needed_to_reach_start; tiles <= RACK_SIZE; tiles++) {
          const BoardSpot *spot =
              board_get_readonly_spot(board, row, col, dir, tiles, 0);
          assert(spot->playthrough_bit_rack == bit_rack_create_empty());
          const int end_col = col + tiles - 1;
          const bool hits_dls = col <= 3 || end_col >= 11;
          uint8_t expected_multipliers[WORD_ALIGNING_RACK_SIZE] = {0};
          int tile_idx = 0;
          if (hits_dls) {
            expected_multipliers[tile_idx++] = 4;
          }
          for (; tile_idx < tiles; tile_idx++) {
            expected_multipliers[tile_idx] = 2;
          }
          for (tile_idx = 0; tile_idx < WORD_ALIGNING_RACK_SIZE; tile_idx++) {
            assert(spot->descending_effective_multipliers[tile_idx] ==
                   expected_multipliers[tile_idx]);
          }
          assert(spot->is_usable);
          assert(spot->word_length == tiles);
          if (spot->word_length == RACK_SIZE) {
            assert(spot->additional_score == game_get_bingo_bonus(game));
          } else {
            assert(spot->additional_score == 0);
          }
        }
      }
    }
  }

  game_destroy(game);
  config_destroy(config);
}

void test_asymmetrical_bricked_empty_board(void) {
  Config *config = config_create_or_die("set -lex NWL20 -wmp true");
  Game *game = config_game_create(config);
  const char *data_paths = config_get_data_paths(config);
  load_game_with_test_board(game, data_paths, "8D_is_bricked_15");
  StringBuilder *sb = string_builder_create();
  string_builder_add_game(sb, game, NULL);
  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  Board *board = game_get_board(game);
  const int starting_row = board->start_coords[0];
  const int starting_col = board->start_coords[1];
  assert(starting_row == starting_col);
  assert(!board_are_bonus_squares_symmetric_by_transposition(board));

  // HORIZONTAL SPOTS
  // Outside of row 8 (7 when zero-indexed), nothing is usable.
  // In the starting row, columns 0 through 3 are unusable because the brick
  // blocks access to the starting square.
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int dir = BOARD_HORIZONTAL_DIRECTION;
      int needed_to_reach_start = BOARD_DIM + 1;
      if (row == starting_row && col <= starting_col && col > 3) {
        needed_to_reach_start = starting_col - col + 1;
      }
      for (int tiles = 1; tiles < needed_to_reach_start; tiles++) {
        if (tiles > RACK_SIZE) {
          break;
        }
        const BoardSpot *spot =
            board_get_readonly_spot(board, row, col, dir, tiles, 0);
        assert(!spot->is_usable);
      }
      for (int tiles = needed_to_reach_start; tiles <= RACK_SIZE; tiles++) {
        const BoardSpot *spot =
            board_get_readonly_spot(board, row, col, dir, tiles, 0);
        assert(spot->playthrough_bit_rack == bit_rack_create_empty());
        const int end_col = col + tiles - 1;
        const bool hits_dls = col <= 3 || end_col >= 11;
        uint8_t expected_multipliers[WORD_ALIGNING_RACK_SIZE] = {0};
        int tile_idx = 0;
        if (hits_dls) {
          expected_multipliers[tile_idx++] = 4;
        }
        for (; tile_idx < tiles; tile_idx++) {
          expected_multipliers[tile_idx] = 2;
        }
        for (tile_idx = 0; tile_idx < WORD_ALIGNING_RACK_SIZE; tile_idx++) {
          assert(spot->descending_effective_multipliers[tile_idx] ==
                 expected_multipliers[tile_idx]);
        }
        assert(spot->is_usable);
        assert(spot->word_length == tiles);
        if (spot->word_length == RACK_SIZE) {
          assert(spot->additional_score == game_get_bingo_bonus(game));
        } else {
          assert(spot->additional_score == 0);
        }
      }
    }
  }

  // VERTICAL SPOTS
  // Vertical plays will be generated because the board is asymmetrical about
  // the diagonal. Outside of row 8 (7 when zero-indexed), nothing is usable.
  // All spots covering the center square will considered usable.
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const int dir = BOARD_VERTICAL_DIRECTION;
      int needed_to_reach_start = BOARD_DIM + 1;
      if (col == starting_col && row <= starting_row) {
        needed_to_reach_start = starting_row - row + 1;
      }
      for (int tiles = 1; tiles < needed_to_reach_start; tiles++) {
        if (tiles > RACK_SIZE) {
          break;
        }
        const BoardSpot *spot =
            board_get_readonly_spot(board, row, col, dir, tiles, 0);
        assert(!spot->is_usable);
      }
      for (int tiles = needed_to_reach_start; tiles <= RACK_SIZE; tiles++) {
        const BoardSpot *spot =
            board_get_readonly_spot(board, row, col, dir, tiles, 0);
        assert(spot->playthrough_bit_rack == bit_rack_create_empty());
        const int end_row = row + tiles - 1;
        const bool hits_dls = row <= 3 || end_row >= 11;
        uint8_t expected_multipliers[WORD_ALIGNING_RACK_SIZE] = {0};
        int tile_idx = 0;
        if (hits_dls) {
          expected_multipliers[tile_idx++] = 4;
        }
        for (; tile_idx < tiles; tile_idx++) {
          expected_multipliers[tile_idx] = 2;
        }
        for (tile_idx = 0; tile_idx < WORD_ALIGNING_RACK_SIZE; tile_idx++) {
          assert(spot->descending_effective_multipliers[tile_idx] ==
                 expected_multipliers[tile_idx]);
        }
        assert(spot->is_usable);
        assert(spot->word_length == tiles);
        if (spot->word_length == RACK_SIZE) {
          assert(spot->additional_score == game_get_bingo_bonus(game));
        } else {
          assert(spot->additional_score == 0);
        }
      }
    }
  }

  game_destroy(game);
  config_destroy(config);
}

void test_standard_with_word_on_board(void) {
  char vac[300] =
      "15/15/15/15/15/15/15/6VAC6/15/15/15/15/15/15/15 / 0/0 0 lex NWL20;";
  Config *config = config_create_or_die("set -l1 NWL20 -l2 CSW21 -wmp true");
  Game *game = config_game_create(config);
  assert(game_load_cgp(game, vac) == CGP_PARSE_STATUS_SUCCESS);

  assert(!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG));
  assert(!game_get_data_is_shared(game, PLAYERS_DATA_TYPE_WMP));

  StringBuilder *sb = string_builder_create();
  string_builder_add_game(sb, game, NULL);
  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);

  // HORIZONTAL SPOTS
  // Plays through VAC in row 8 (7 when zero-indexed).
  // Plays in row 9 (8 when zero-indexed) only in CSW (hooking CH).
  // Do not generate vertical two letter one-tile plays (_A or A_) as horizontal
  // pseudo-one-letter plays.
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
        const int dir = BOARD_HORIZONTAL_DIRECTION;
        if (row <= 6 || row >= 9) {
          assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
        }
        // Plays through VAC
        if (row == 7) {
          if (col == 0) {
            // Need 6 tiles to touch played tiles.
            if (tiles < 6) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else if (tiles == 6) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){6, 3, 3, 3, 3, 3, 0, 0}, 24, 9);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){6, 3, 3, 3, 3, 3, 3, 0}, 74, 10);
            }
          } else if (col == 1) {
            // Need 5 tiles to touch played tiles.
            if (tiles < 5) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else if (tiles == 5) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){2, 1, 1, 1, 1, 0, 0, 0}, 8, 8);
            } else if (tiles == 6) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){2, 1, 1, 1, 1, 1, 0, 0}, 8, 9);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){2, 1, 1, 1, 1, 1, 1, 0}, 58, 10);
            }
          } else if (col == 6) {
            // (col >= 2 && col <= 5) untested, assumed covered by col = 1 case
            if (tiles == 1) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){1, 0, 0, 0, 0, 0, 0, 0}, 8, 4);
            } else if (tiles == 2) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){1, 1, 0, 0, 0, 0, 0, 0}, 8, 5);
            } else if (tiles == 6) {
              // tiles >= 3 && tiles <= 5 untested
              // six tiles reaches TWS, e.g. (VAC)ATIONS
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "ACV",
                                 (uint8_t[]){6, 3, 3, 3, 3, 3, 0, 0}, 24, 9);
            } else if (tiles == 7) {
              // seven tiles would go off the edge of the board
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            }
          }
        }
      }
    }
  }

  game_destroy(game);
  config_destroy(config);
}

void test_board_spot(void) {
  // test_standard_empty_board();
  // test_asymmetrical_bricked_empty_board();
  test_standard_with_word_on_board();
}
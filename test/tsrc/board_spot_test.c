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
          if (tiles == 1) {
            assert(!spot->is_usable);
            continue;
          }
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
        if (tiles == 1) {
          assert(!spot->is_usable);
          continue;
        }
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
        if (tiles == 1) {
          assert(!spot->is_usable);
          continue;
        }
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
  // Board contains 8G VAC
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
        if (row == 7) {
          // Plays through VAC
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
        } else if (row == 8) {
          // Plays underlapping the A and C of VAC (but only in CSW).
          if (col <= 6 || col >= 9) {
            assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
          } else if (col == 7) {
            assert_unusable_spot(game, row, col, dir, tiles, 0);
            if (tiles == 1) {
              assert_unusable_spot(game, row, col, dir, tiles, 1);
            } else if (tiles == 2) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 0, 0, 0, 0, 0, 0}, 4, 2);
            } else if (tiles == 3) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 1, 0, 0, 0, 0, 0}, 4, 3);
            } else if (tiles == 4) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 1, 1, 0, 0, 0, 0}, 4, 4);
            } else if (tiles == 5) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 1, 1, 1, 0, 0, 0}, 4, 5);
            } else if (tiles == 6) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 2, 1, 1, 1, 0, 0}, 4, 6);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, 1, "",
                                 (uint8_t[]){4, 2, 2, 1, 1, 1, 1, 0}, 54, 7);
            }
          }
        }
      }
    }
  }

  // VERTICAL SPOTS
  // Plays through each letter of VAC in cols 7, 8, 9 (6, 7, 8 when 0-indexed).
  // Plays hooking VAC -> VACS in col 10 (9 when 0-indexed).
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
        const int dir = BOARD_VERTICAL_DIRECTION;
        if (col <= 5 || col >= 10) {
          assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
        }
        if (col == 6) {
          // Plays through V
          if (row == 0) {
            // Need 7 tiles to touch played tiles.
            if (tiles < 7) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 1, 1, 1, 1, 1, 0}, 54, 8);
            }
          } else if (row == 1) {
            // Need 6 tiles to touch played tiles.
            if (tiles < 6) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else if (tiles == 6) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 1, 1, 1, 1, 0, 0}, 4, 7);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 2, 1, 1, 1, 1, 0}, 54, 8);
            }
          } else if (row == 6) {
            // rows >= 2 && rows <= 5 untested
            if (tiles == 1) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 0, 0, 0, 0, 0, 0, 0}, 4, 2);
            } else if (tiles == 2) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 0, 0, 0, 0, 0, 0}, 4, 3);
            } else if (tiles == 6) {
              // tiles >= 3 && tiles <= 5 untested
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 2, 1, 1, 1, 0, 0}, 4, 7);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 2, 1, 1, 1, 1, 0}, 54, 8);
            }
          } else if (row == 7) {
            // Plays through A
            if (tiles == 1) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 0, 0, 0, 0, 0, 0, 0}, 4, 2);
            } else if (tiles == 2) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 1, 0, 0, 0, 0, 0, 0}, 4, 3);
            } else if (tiles == 6) {
              // tiles >= 3 && tiles <= 5 untested
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 1, 1, 1, 1, 0, 0}, 4, 7);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "V",
                                 (uint8_t[]){2, 2, 1, 1, 1, 1, 1, 0}, 54, 8);
            }
          } else if (row >= 8) {
            assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
          }
        } else if (col == 7) {
          // Plays through A
          if (row == 0) {
            // Need 7 tiles to touch played tiles.
            if (tiles < 7) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "A",
                                 (uint8_t[]){6, 3, 3, 3, 3, 3, 3, 0}, 53, 8);
            }
          } else if (row == 6) {
            // skipping uninteresting cases
            // Test _A one-tile play
            if (tiles == 1) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "A",
                                 (uint8_t[]){1, 0, 0, 0, 0, 0, 0, 0}, 1, 2);
            }
          } else if (row == 7) {
            if (tiles == 1) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "A",
                                 (uint8_t[]){1, 0, 0, 0, 0, 0, 0, 0}, 1, 2);
            }
          }
        } else if (col == 9) {
          // Skipping plays through C, not substantially different than
          // the logic for finding plays through V.
          if (row == 0) {
            // Can't reach far enough to hook VAC from the top row.
            assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
          } else if (row == 1) {
            // Need 7 tiles to hook VAC
            if (tiles < 7) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "",
                                 (uint8_t[]){3, 3, 2, 1, 1, 1, 1, 0}, 58, 7);
            }
          } else if (row == 2) {
            // Need 6 tiles to hook VAC
            if (tiles < 6) {
              assert_unusable_spot(game, row, col, dir, tiles, BOTH_CI);
            } else if (tiles == 6) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "",
                                 (uint8_t[]){3, 2, 1, 1, 1, 1, 0, 0}, 8, 6);
            } else if (tiles == 7) {
              assert_usable_spot(game, row, col, dir, tiles, BOTH_CI, "",
                                 (uint8_t[]){3, 2, 1, 1, 1, 1, 1, 0}, 58, 7);
            }
          } else if (row == 7) {
            if (tiles == 1) {
              // Don't generate (VAC)S as a vertical play.
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

void test_oxyphenbutazone_board_spot(void) {
  Config *config = config_create_or_die("set -lex NWL20 -wmp true");
  Game *game = config_game_create(config);
  assert(game_load_cgp(game, VS_OXY) == CGP_PARSE_STATUS_SUCCESS);

  // 56 = 27x2 + 2 for TWSxTWSxTWSxDLS + DLS
  // 30 = 27 + 3 for TWSxTWSxTWS + TWS
  // 28 = 27 + 1 for TWSxTWS + cross word without premium
  // playthrough tiles EHNNOTUY = 14. 14x27 = 378.
  // PACIFYING = 20. 20x3 = 60.
  // IS = 2
  // REQUALIFIED = 24
  // RAINWASHING = 18. 18x3 = 54.
  // WAKEnERS = 14
  // OnETIME = 8
  // JACULATING = 20. 20x3 = 60.
  // bingo bonus = 50
  // 378 + 60 + 2 + 24 + 54 + 14 + 8 + 60 + 50 = 650
  assert_usable_spot(game, 0, 0, BOARD_VERTICAL_DIRECTION, 7, 0, "EHNNOTUY",
                     (uint8_t[]){56, 56, 30, 30, 30, 28, 28, 0}, 650, 15);

  game_destroy(game);
  config_destroy(config);
}

void test_wof_board_spot(void) {
  char wof[300] = "15/15/15/15/15/15/15/3QINTAR6/2GU4E6/2LI4C6/2ON4O6/2OI3HM6/"
                  "2ME3OB6/1ASSEZ1PE6/7ED6 EFLORTW/ABNST?? 203/138 0";

  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  assert(game_load_cgp(game, wof) == CGP_PARSE_STATUS_SUCCESS);

  assert_usable_spot(game, 11, 6, BOARD_VERTICAL_DIRECTION, 2, 0, "",
    (uint8_t[]){4, 2, 0, 0, 0, 0, 0, 0}, 11, 2);

  assert_usable_spot(game, 10, 6, BOARD_VERTICAL_DIRECTION, 3, 0, "",
                     (uint8_t[]){4, 2, 1, 0, 0, 0, 0, 0}, 11, 3);

  game_destroy(game);
  config_destroy(config);
}

void test_board_spot(void) {
  // test_standard_empty_board();
  // test_asymmetrical_bricked_empty_board();
  // test_standard_with_word_on_board();
  // test_oxyphenbutazone_board_spot();
  test_wof_board_spot();
}
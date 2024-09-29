#include <assert.h>
#include <time.h>

#include "../../src/def/autoplay_defs.h"

#include "../../src/ent/autoplay_results.h"
#include "../../src/ent/board.h"
#include "../../src/ent/board_layout.h"
#include "../../src/ent/game.h"
#include "../../src/ent/move.h"
#include "../../src/ent/validated_move.h"
#include "../../src/impl/config.h"

#include "../../src/impl/autoplay.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "test_constants.h"
#include "test_util.h"

void test_board_layout_error_super(void) {
  // Most of the error enums are tested in the 15 version.
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "standard15",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_ROWS);
}

void test_board_layout_correctness_super(void) {
  // Use the CEL lexicon since it can support words longer than 15
  // which we need to test here.
  Config *config = config_create_or_die(
      "set -lex CEL_super -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  // Verify the opening play score
  assert_validated_and_generated_moves(game, "IONIZED", "11K", "IONIZED", 104,
                                       false);

  // Verify that the super distribution is being used.
  assert_validated_and_generated_moves(game, "PIZZA??", "11J", "PIZzAZz", 120,
                                       false);

  load_cgp_or_die(game, ANTHROPOMORPHISATIONS_CGP);
  assert_validated_and_generated_moves(game, "ANTHROS", "11A",
                                       "ANTHRO(POMORPHISATION)S", 155, true);

  load_and_exec_config_or_die(
      config, "autoplay games,fj 100 -gp false -threads 11 -seed 100");

  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_super(void) {
  test_board_layout_error_super();
  test_board_layout_correctness_super();
}
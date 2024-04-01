#include <assert.h>
#include <time.h>

#include "../../src/def/autoplay_defs.h"

#include "../../src/ent/autoplay_results.h"
#include "../../src/ent/board.h"
#include "../../src/ent/board_layout.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/move.h"
#include "../../src/ent/validated_move.h"

#include "../../src/impl/autoplay.h"
#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "test_constants.h"
#include "test_util.h"

void test_board_layout_error_super() {
  // Most of the error enums are tested in the 15 version.
  assert_board_layout_error("standard15.txt",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_ROWS);
}

void test_board_layout_correctness_super() {
  // Use the CEL lexicon since it can support words longer than 15
  // which we need to test here.
  Config *config = create_config_or_die(
      "setoptions lex CEL s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);

  // Verify the opening play score
  assert_validated_and_generated_moves(game, "IONIZED", "11K", "IONIZED", 104,
                                       false);

  // Verify that the super distribution is being used.
  assert_validated_and_generated_moves(game, "PIZZA??", "11J", "PIZzAZz", 120,
                                       false);

  load_cgp_or_die(game, ANTHROPOMORPHISATIONS_CGP);
  assert_validated_and_generated_moves(game, "ANTHROS", "11A",
                                       "ANTHRO(POMORPHISATION)S", 155, true);

  uint64_t seed = time(NULL);

  char *options_string =
      get_formatted_string("setoptions i 500 gp threads 11 rs %ld", seed);

  load_config_or_die(config, options_string);

  printf("running 21 autoplay with: %s\n", options_string);

  free(options_string);

  AutoplayResults *ar = autoplay_results_create();

  autoplay_status_t status = autoplay(config, ar);
  assert(status == AUTOPLAY_STATUS_SUCCESS);

  autoplay_results_destroy(ar);
  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_super() {
  test_board_layout_error_super();
  test_board_layout_correctness_super();
}
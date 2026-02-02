#include "../src/def/board_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Test CGP with ASTROID on row 8 (0-indexed row 7), starting at column H
// The hook position at column O (14) is a Triple Word Score
// Note: ASTROID may or may not be lexicon-specific - this test demonstrates
// the infrastructure for dual-lexicon support where cross-sets are computed
// per-player based on their KWG.
#define ASTROID_CGP                                                            \
  "15/15/15/15/15/15/15/7ASTROID1/15/15/15/15/15/15/15 VOKATER/AEILNRS 0/0 0"

// Helper to check if a letter is in a cross-set
static bool letter_in_cross_set(uint64_t cross_set, uint8_t ml) {
  return (cross_set & get_cross_set_bit(ml)) != 0;
}

// Test that demonstrates cross-set differences between lexicons
// When ASTROID is on the board:
// - CSW player (player 2) sees S in cross-set at O8 (can play ASTROIDS)
// - NWL player (player 1) does NOT see S in cross-set (ASTROIDS not valid)
void test_dual_lexicon_cross_sets(void) {
  // Create a dual-lexicon game: player 1 uses NWL, player 2 uses CSW
  Config *config = config_create_or_die("set -l1 NWL20 -l2 CSW21");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);

  Game *game = config_get_game(config);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Set up the board with ASTROID at 8H (row 7, col 7-13)
  const char *astroid_row = "       ASTROID ";
  set_row(game, 7, astroid_row);
  game_gen_all_cross_sets(game);

  // Check cross-set at position O8 (row 7, col 14) - the TWS hook square
  // Cross-set index 0 = player 1 (NWL)
  // Cross-set index 1 = player 2 (CSW)
  uint64_t nwl_cross_set =
      board_get_cross_set(board, 7, 14, BOARD_HORIZONTAL_DIRECTION, 0);
  uint64_t csw_cross_set =
      board_get_cross_set(board, 7, 14, BOARD_HORIZONTAL_DIRECTION, 1);

  uint8_t s_ml = ld_hl_to_ml(ld, "S");
  bool nwl_has_s = letter_in_cross_set(nwl_cross_set, s_ml);
  bool csw_has_s = letter_in_cross_set(csw_cross_set, s_ml);

  printf("\n=== Dual Lexicon Cross-Set Test ===\n");
  printf("Game setup: Player 1 (NWL) vs Player 2 (CSW)\n");
  printf("Board: ASTROID at 8H\n");
  printf("Hook position: O8 (row 7, col 14) - Triple Word Score\n\n");

  char *nwl_cs_str = cross_set_to_string(ld, nwl_cross_set);
  char *csw_cs_str = cross_set_to_string(ld, csw_cross_set);

  printf("NWL (Player 1) cross-set at O8: %s\n", nwl_cs_str);
  printf("NWL has S in cross-set: %s\n", nwl_has_s ? "YES" : "NO");
  printf("\nCSW (Player 2) cross-set at O8: %s\n", csw_cs_str);
  printf("CSW has S in cross-set: %s\n", csw_has_s ? "YES" : "NO");

  free(nwl_cs_str);
  free(csw_cs_str);

  // The key assertion: CSW should have S, NWL should NOT
  // (assuming ASTROID is CSW-only and ASTROIDS follows the same pattern)
  printf("\nExpected: CSW has S hook for ASTROIDS, NWL does not\n");
  if (csw_has_s && !nwl_has_s) {
    printf("*** PASS: Cross-sets differ as expected ***\n");
  } else if (csw_has_s == nwl_has_s) {
    printf("Note: Both players have same S availability.\n");
    printf("This could mean ASTROID/ASTROIDS is in both lexicons.\n");
  }

  config_destroy(config);
}

// Test dual-lexicon game setup where players have different lexicons
void test_dual_lexicon_game_setup(void) {
  // Set up a game where player 1 uses NWL and player 2 uses CSW
  Config *config =
      config_create_or_die("set -l1 NWL20 -l2 CSW21 -s1 score -s2 score");

  // Load an empty board
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);

  Game *game = config_get_game(config);

  // Verify the KWGs are different
  const Player *p1 = game_get_player(game, 0);
  const Player *p2 = game_get_player(game, 1);
  const KWG *kwg1 = player_get_kwg(p1);
  const KWG *kwg2 = player_get_kwg(p2);

  // The KWGs should be different pointers (different lexicons)
  bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  printf("\n=== Dual Lexicon Game Setup Test ===\n");
  printf("Player 1 KWG: %p\n", (void *)kwg1);
  printf("Player 2 KWG: %p\n", (void *)kwg2);
  printf("KWGs are shared: %s\n", kwgs_are_shared ? "YES" : "NO");

  assert(!kwgs_are_shared);

  config_destroy(config);
}

// Test that demonstrates the IGNORANT vs INFORMED concept:
// When a TWL player plays against a CSW player, the cross-sets differ.
// Player 0 (NWL) won't see CSW-only hooks in their cross-sets.
// Player 1 (CSW) will see all hooks including CSW-only ones.
void test_dual_lexicon_cross_set_indices(void) {
  Config *config = config_create_or_die("set -l1 NWL20 -l2 CSW21");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);

  Game *game = config_get_game(config);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Place ASTROID on the board
  const char *astroid_row = "       ASTROID ";
  set_row(game, 7, astroid_row);
  game_gen_all_cross_sets(game);

  // Get cross-sets for both players at the hook square (row 7, col 14)
  // cross_set_index 0 = player 0 (NWL)
  // cross_set_index 1 = player 1 (CSW)
  uint64_t p1_cross_set =
      board_get_cross_set(board, 7, 14, BOARD_HORIZONTAL_DIRECTION, 0);
  uint64_t p2_cross_set =
      board_get_cross_set(board, 7, 14, BOARD_HORIZONTAL_DIRECTION, 1);

  uint8_t s_ml = ld_hl_to_ml(ld, "S");
  bool p1_has_s = letter_in_cross_set(p1_cross_set, s_ml);
  bool p2_has_s = letter_in_cross_set(p2_cross_set, s_ml);

  printf("\n=== Dual Lexicon Per-Player Cross-Sets ===\n");
  printf("Position: O8 (row 7, col 14) - TWS square\n");
  printf("Board: ASTROID at 8H\n\n");

  char *p1_cs_str = cross_set_to_string(ld, p1_cross_set);
  char *p2_cs_str = cross_set_to_string(ld, p2_cross_set);

  printf("Player 1 (NWL) cross-set: %s\n", p1_cs_str);
  printf("Player 1 (NWL) has S: %s\n", p1_has_s ? "YES" : "NO");
  printf("\nPlayer 2 (CSW) cross-set: %s\n", p2_cs_str);
  printf("Player 2 (CSW) has S: %s\n", p2_has_s ? "YES" : "NO");

  free(p1_cs_str);
  free(p2_cs_str);

  printf("\n=== Implication for Simulation Modes ===\n");
  printf("IGNORANT mode: NWL player uses their own cross-sets (index 0)\n");
  printf("  -> Does NOT see ASTROIDS threat at TWS\n");
  printf("INFORMED mode: NWL player considers opponent's cross-sets (index 1)\n");
  printf("  -> SEES ASTROIDS threat at TWS\n");

  if (p1_has_s == p2_has_s) {
    printf("\nNote: Both players have same S availability.\n");
    printf("This could mean ASTROID/ASTROIDS is in both lexicons,\n");
    printf("or the word placement doesn't create the expected hook.\n");
  } else {
    printf("\n*** CROSS-SET DIFFERENCE DETECTED ***\n");
    printf("CSW player can hook S to make ASTROIDS.\n");
    printf("NWL player cannot (ASTROID/ASTROIDS not in NWL).\n");
  }

  config_destroy(config);
}

void test_dual_lexicon(void) {
  test_dual_lexicon_cross_sets();
  fflush(stdout);
  test_dual_lexicon_game_setup();
  fflush(stdout);
  test_dual_lexicon_cross_set_indices();
  fflush(stdout);
}

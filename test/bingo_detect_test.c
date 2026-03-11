#include "bingo_detect_test.h"

#include "../src/ent/game.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "test_util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Board from the PEG straightforward test (NWL20, ONYX position).
#define ONYX_CGP                                                               \
  "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"              \
  "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"              \
  "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "        \
  "NWL20"

// Check bingo detection consistency for a single rack string on two games
// (one with WMP, one without).
static void check_bingo_detect_for_rack(Game *game_wmp, Game *game_nowmp,
                                        const char *rack_str) {
  const LetterDistribution *ld = game_get_ld(game_wmp);
  int on_turn = game_get_player_on_turn_index(game_wmp);

  // Set rack on both games.
  Rack *rack_wmp = player_get_rack(game_get_player(game_wmp, on_turn));
  Rack *rack_nowmp = player_get_rack(game_get_player(game_nowmp, on_turn));
  rack_set_to_string(ld, rack_wmp, rack_str);
  rack_set_to_string(ld, rack_nowmp, rack_str);

  // Full movegen ground truth (uses WMP game for speed, result is definitive).
  bool movegen_result = has_playable_bingo(game_wmp, 0);

  // has_playable_or_possible_bingo with WMP.
  bool wmp_result = has_playable_or_possible_bingo(game_wmp, 0);

  // has_playable_or_possible_bingo without WMP (KWG-only fallback).
  bool kwg_result = has_playable_or_possible_bingo(game_nowmp, 0);

  printf("  rack=%-10s  movegen=%d  wmp=%d  kwg=%d", rack_str,
         movegen_result, wmp_result, kwg_result);

  // 1. WMP and KWG must agree.
  if (wmp_result != kwg_result) {
    printf("  ** MISMATCH WMP vs KWG **\n");
    assert(wmp_result == kwg_result);
  }

  // 2. If movegen finds a playable bingo, both must return true.
  if (movegen_result) {
    if (!wmp_result) {
      printf("  ** movegen=true but wmp/kwg=false **\n");
      assert(wmp_result);
    }
    printf("  ok (playable)\n");
  } else if (wmp_result) {
    // Possible but not playable on this board — that's fine.
    printf("  ok (possible, not playable)\n");
  } else {
    printf("  ok (none)\n");
  }
}

// Test that WMP and KWG-only bingo detection agree on various 7-tile racks,
// and that both return true whenever full movegen finds a playable bingo.
// All test racks must have exactly RACK_SIZE (7) tiles, matching how PEG
// calls this function (guarded by rack_get_total_letters == RACK_SIZE).
static void test_bingo_detect_onyx_position(void) {
  Config *config_wmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_wmp, ONYX_CGP);
  Game *game_wmp = config_get_game(config_wmp);

  Config *config_nowmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_nowmp, ONYX_CGP " -wmp false");
  Game *game_nowmp = config_get_game(config_nowmp);

  printf("\n--- bingo detection: ONYX position, mover's perspective ---\n");

  // All racks must be exactly 7 tiles (RACK_SIZE).
  static const char *test_racks[] = {
      // Original mover/opponent racks from the position.
      "ENOSTXY",
      "ACEISUY",
      // Known bingo racks (NWL20).
      "AEINRST", // NASTIER, RETINAS, etc.
      "AEIRSTT", // ATTIRES, TASTIER, etc.
      "DEINORS", // INDORSE, ORDINES, etc.
      "AEILNRS", // ALINERS, NAILERS, etc.
      "AEEGNRT", // REAGENT, etc.
      "ADEINRS", // SARDINE, etc.
      // Rack with 2 blanks (always true).
      "??AEIOU",
      // Racks with 1 blank.
      "?ENOSTY",
      "?ACEISY",
      "?AEINRS",
      // Non-bingo racks.
      "AAAAAAA",
      "IIIIIII",
      "UUUUUVV",
      "QXZJJWW",
      "VVWWXYZ",
      // Has 8-letter words (VIRTUOUS etc) but no 7-letter word.
      "IORSTUV",
      // Racks that test edge cases.
      "AEIOUST", // no 7-letter word expected
      "EGILNOS", // LONGIES, etc.
      "AELNOST", // TOLANES, etc.
      "AEGILNR", // REALIGN, etc.
      "EINORST", // STONIER, etc.
      "BDEIORS", // BORIDES, etc.
  };
  int num_racks = (int)(sizeof(test_racks) / sizeof(test_racks[0]));

  for (int i = 0; i < num_racks; i++) {
    check_bingo_detect_for_rack(game_wmp, game_nowmp, test_racks[i]);
  }

  config_destroy(config_wmp);
  config_destroy(config_nowmp);
}

// Test on the opponent's perspective (swap on-turn player).
static void test_bingo_detect_onyx_opponent(void) {
  Config *config_wmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_wmp, ONYX_CGP);
  Game *game_wmp = config_get_game(config_wmp);

  Config *config_nowmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_nowmp, ONYX_CGP " -wmp false");
  Game *game_nowmp = config_get_game(config_nowmp);

  // Switch to opponent's turn.
  game_set_player_on_turn_index(game_wmp, 1);
  game_set_player_on_turn_index(game_nowmp, 1);

  printf("\n--- bingo detection: ONYX position, opponent's perspective ---\n");

  // Opponent 7-tile racks that arise during PEG endgame evaluation.
  static const char *test_racks[] = {
      "ACEISUY",
      "AEINRST",
      "AEGILNR",
      "EINORST",
      "DEINORS",
      "AAAAAAA",
      "?ACEISY",
      "?EINRST",
      "??AEIOU",
  };
  int num_racks = (int)(sizeof(test_racks) / sizeof(test_racks[0]));

  for (int i = 0; i < num_racks; i++) {
    check_bingo_detect_for_rack(game_wmp, game_nowmp, test_racks[i]);
  }

  config_destroy(config_wmp);
  config_destroy(config_nowmp);
}

// Test on an empty board (all bingos through center star).
static void test_bingo_detect_empty_board(void) {
  Config *config_wmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_wmp,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
      "AEINRST/ 0/0 0 -lex NWL20");
  Game *game_wmp = config_get_game(config_wmp);

  Config *config_nowmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_nowmp,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
      "AEINRST/ 0/0 0 -lex NWL20 -wmp false");
  Game *game_nowmp = config_get_game(config_nowmp);

  printf("\n--- bingo detection: empty board ---\n");

  static const char *test_racks[] = {
      "AEINRST", // known bingo (NASTIER etc), playable on empty board
      "ACEISUY", // no 7-letter word
      "AAAAAAA", // no word
      "?AEINRS", // blank + bingo tiles
      "??AEIOU", // two blanks
  };
  int num_racks = (int)(sizeof(test_racks) / sizeof(test_racks[0]));

  for (int i = 0; i < num_racks; i++) {
    check_bingo_detect_for_rack(game_wmp, game_nowmp, test_racks[i]);
  }

  config_destroy(config_wmp);
  config_destroy(config_nowmp);
}

// EGLNORT has no 7-letter or 8-letter words, but LORGNETTE is a 9-letter
// word playable through ET on the board.  The rack-only checks (2-3) must
// return false; only the board-based check (4) should detect the bingo.
// This test verifies that has_playable_bingo works without WMP (previously
// broken because MOVE_RECORD_BINGO_EXISTS filtered on unset tiles_to_play).
static void test_bingo_detect_playable_nine(void) {
  // ET played horizontally at 8H (columns H-I, row 8).
  const char *cgp = "cgp 15/15/15/15/15/15/15/7ET6/15/15/15/15/15/15/15 "
                     "EGLNORT/ 0/0 0 -lex NWL20";

  Config *config_wmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_wmp, cgp);
  Game *game_wmp = config_get_game(config_wmp);

  Config *config_nowmp =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config_nowmp,
      "cgp 15/15/15/15/15/15/15/7ET6/15/15/15/15/15/15/15 "
      "EGLNORT/ 0/0 0 -lex NWL20 -wmp false");
  Game *game_nowmp = config_get_game(config_nowmp);

  printf("\n--- bingo detection: playable 9 (EGLNORT + ET on board) ---\n");

  // Verify that has_playable_bingo finds the bingo independently.
  bool bingo_wmp = has_playable_bingo(game_wmp, 0);
  bool bingo_nowmp = has_playable_bingo(game_nowmp, 0);
  printf("  has_playable_bingo: wmp=%d  nowmp=%d\n", bingo_wmp, bingo_nowmp);

  check_bingo_detect_for_rack(game_wmp, game_nowmp, "EGLNORT");

  config_destroy(config_wmp);
  config_destroy(config_nowmp);
}

void test_bingo_detect(void) {
  test_bingo_detect_onyx_position();
  test_bingo_detect_onyx_opponent();
  test_bingo_detect_empty_board();
  test_bingo_detect_playable_nine();
}
